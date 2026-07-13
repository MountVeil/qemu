#!/usr/bin/env bash
set -euo pipefail

QEMU_ROOT="${QEMU_ROOT:-$(git rev-parse --show-toplevel)}"
OPENSBI_ROOT="${OPENSBI_ROOT:-$HOME/Documents/kvm/opensbi}"
WORKDIR="${WORKDIR:-$HOME/Documents/kvm/smmpt-integration}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

TESTDIR="$QEMU_ROOT/tests/tcg/riscv64/smmpt"
OUTDIR="$WORKDIR/sv39-test"
MONITOR="/tmp/qemu-smmpt-monitor"

mkdir -p "$OUTDIR"

"${CROSS_COMPILE}gcc" \
    -march=rv64imac_zicsr_zifencei \
    -mabi=lp64 \
    -mcmodel=medany \
    -nostdlib \
    -nostartfiles \
    -ffreestanding \
    -Wl,-T,"$TESTDIR/smmpt_sv39.ld" \
    -Wl,--build-id=none \
    -o "$OUTDIR/smmpt_sv39.elf" \
    "$TESTDIR/smmpt_sv39.S"

rm -f "$MONITOR"
rm -f "$WORKDIR/sv39-serial.log"
rm -f "$WORKDIR/sv39-mmu.log"

"$QEMU_ROOT/build/qemu-system-riscv64" \
    -machine virt \
    -m 512M \
    -smp 1 \
    -bios "$OPENSBI_ROOT/build/platform/generic/firmware/fw_jump.bin" \
    -kernel "$OUTDIR/smmpt_sv39.elf" \
    -dtb "$WORKDIR/virt-smmpt.dtb" \
    -display none \
    -serial "file:$WORKDIR/sv39-serial.log" \
    -monitor "unix:$MONITOR,server,nowait" \
    -d mmu \
    -D "$WORKDIR/sv39-mmu.log" \
    -daemonize

cleanup()
{
    if [ -S "$MONITOR" ]; then
        echo quit | socat - "UNIX-CONNECT:$MONITOR" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

sleep 1

if ! tail -30 "$WORKDIR/sv39-serial.log" | grep -q 'S'; then
    echo "[fail] S-mode Sv39 payload did not report success"
    tail -50 "$WORKDIR/sv39-serial.log"
    exit 1
fi

SMMPT_INFO="$(
    echo 'info smmpt' |
    socat - "UNIX-CONNECT:$MONITOR" |
    tr -d '\r'
)"

printf '%s\n' "$SMMPT_INFO"

get_counter()
{
    local name="$1"

    printf '%s\n' "$SMMPT_INFO" |
        awk -v key="$name" '
            $1 == key ":" {
                print $2
                exit
            }
        '
}

FINAL_CHECKS="$(get_counter final-checks)"
PTE_FETCH_CHECKS="$(get_counter pte-fetch-checks)"
AD_UPDATE_CHECKS="$(get_counter ad-update-checks)"
ALLOWED="$(get_counter allowed)"
DENIED="$(get_counter denied)"

for value in \
    "$FINAL_CHECKS" \
    "$PTE_FETCH_CHECKS" \
    "$AD_UPDATE_CHECKS" \
    "$ALLOWED" \
    "$DENIED"
do
    case "$value" in
        ''|*[!0-9]*)
            echo "[fail] unable to parse SmMPT counters"
            exit 1
            ;;
    esac
done

if [ "$FINAL_CHECKS" -le 0 ]; then
    echo "[fail] final SmMPT checks were not exercised"
    exit 1
fi

if [ "$PTE_FETCH_CHECKS" -le 0 ]; then
    echo "[fail] PTE-fetch SmMPT checks were not exercised"
    exit 1
fi

if [ "$AD_UPDATE_CHECKS" -le 0 ]; then
    echo "[fail] A/D-update SmMPT checks were not exercised"
    exit 1
fi

if [ "$DENIED" -ne 0 ]; then
    echo "[fail] unexpected SmMPT denial"
    exit 1
fi

if [ "$ALLOWED" -ne \
     "$((FINAL_CHECKS + PTE_FETCH_CHECKS + AD_UPDATE_CHECKS))" ]; then
    echo "[fail] SmMPT counter invariant violated"
    exit 1
fi

PTE_DUMP="$(
    printf 'xp /4gx 0x80201000\n' |
    socat - "UNIX-CONNECT:$MONITOR"
)"

printf '%s\n' "$PTE_DUMP"

if ! printf '%s\n' "$PTE_DUMP" |
     grep -q '0x00000000000000cf'; then
    echo "[fail] entry 0 A/D bits were not updated"
    exit 1
fi

if ! printf '%s\n' "$PTE_DUMP" |
     grep -q '0x00000000200000cf'; then
    echo "[fail] entry 2 A/D bits were not updated"
    exit 1
fi

echo "[pass] strict SmMPT Sv39 integration test"
