#!/usr/bin/env bash
set -euo pipefail

QEMU_ROOT="${QEMU_ROOT:-$(git rev-parse --show-toplevel)}"
OPENSBI_ROOT="${OPENSBI_ROOT:-$HOME/Documents/kvm/opensbi}"
WORKDIR="${WORKDIR:-$HOME/Documents/kvm/smmpt-integration}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

TESTDIR="$QEMU_ROOT/tests/tcg/riscv64/smmpt"
OUTDIR="$WORKDIR/sv39-revoke-test"
MONITOR="/tmp/qemu-smmpt-revoke-monitor"
SERIAL_LOG="$WORKDIR/sv39-revoke-serial.log"
MMU_LOG="$WORKDIR/sv39-revoke-mmu.log"

mkdir -p "$OUTDIR"

"${CROSS_COMPILE}gcc" \
    -march=rv64imac_zicsr_zifencei \
    -mabi=lp64 \
    -mcmodel=medany \
    -nostdlib \
    -nostartfiles \
    -ffreestanding \
    -Wl,-T,"$TESTDIR/smmpt_sv39_revoke.ld" \
    -Wl,--build-id=none \
    -o "$OUTDIR/smmpt_sv39_revoke.elf" \
    "$TESTDIR/smmpt_sv39_revoke.S"

rm -f "$MONITOR" "$SERIAL_LOG" "$MMU_LOG"

"$QEMU_ROOT/build/qemu-system-riscv64" \
    -machine virt \
    -cpu rv64,smmpt-policy=2 \
    -m 512M \
    -smp 1 \
    -bios "$OPENSBI_ROOT/build/platform/generic/firmware/fw_jump.bin" \
    -kernel "$OUTDIR/smmpt_sv39_revoke.elf" \
    -dtb "$WORKDIR/virt-smmpt.dtb" \
    -display none \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR,server,nowait" \
    -d mmu \
    -D "$MMU_LOG" \
    -daemonize

cleanup()
{
    if [ -S "$MONITOR" ]; then
        echo quit |
            socat - "UNIX-CONNECT:$MONITOR" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

for _ in $(seq 1 50); do
    if grep -q 'RHT\|RHF\|RHE' "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

echo '=== serial tail ==='
tail -30 "$SERIAL_LOG"

if grep -q 'RHF' "$SERIAL_LOG"; then
    echo "[fail] stale PTAC authorization survived mfence.mcpa"
    exit 1
fi

if grep -q 'RHE' "$SERIAL_LOG"; then
    echo "[fail] OpenSBI permission-revise ecall failed"
    exit 1
fi

if ! grep -q 'RHT' "$SERIAL_LOG"; then
    echo "[fail] expected revoke sequence RHT was not observed"
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
        awk -F ':' -v key="$name" '
            {
                lhs = $1
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", lhs)

                if (lhs == key) {
                    value = $2
                    gsub(/[[:space:]]/, "", value)
                    print value
                    exit
                }
            }
        '
}

DENIED="$(get_counter denied)"
PTAC_LOOKUPS="$(get_counter lookups)"
PTAC_HITS="$(get_counter hits)"
PTAC_MISSES="$(get_counter misses)"
PTAC_INVALIDATIONS="$(get_counter invalidations)"
MFENCE_INVALIDATES="$(get_counter mfence-invalidates)"
PTE_FULL_LOOKUPS="$(get_counter pte-full-lookups)"
PTE_REUSES="$(get_counter pte-reuses)"

for value in \
    "$DENIED" \
    "$PTAC_LOOKUPS" \
    "$PTAC_HITS" \
    "$PTAC_MISSES" \
    "$PTAC_INVALIDATIONS" \
    "$MFENCE_INVALIDATES" \
    "$PTE_FULL_LOOKUPS" \
    "$PTE_REUSES"
do
    case "$value" in
        ''|*[!0-9]*)
            echo "[fail] unable to parse SmMPT revoke counters"
            exit 1
            ;;
    esac
done

if [ "$DENIED" -le 0 ]; then
    echo "[fail] revoked page-table fetch did not produce an SmMPT denial"
    exit 1
fi

if [ "$MFENCE_INVALIDATES" -le 0 ]; then
    echo "[fail] mfence.mcpa was not observed"
    exit 1
fi

if [ "$PTAC_HITS" -le 0 ]; then
    echo "[fail] pre-revocation PTAC reuse was not exercised"
    exit 1
fi

if [ "$PTAC_MISSES" -lt 2 ]; then
    echo "[fail] post-fence PTAC miss was not observed"
    exit 1
fi

if [ "$PTE_FULL_LOOKUPS" -lt 2 ]; then
    echo "[fail] post-fence full PTE lookup was not observed"
    exit 1
fi

if [ "$PTE_REUSES" -le 0 ]; then
    echo "[fail] PTAC did not reuse pre-revocation authorization"
    exit 1
fi

echo "[pass] SmMPT PTAC revocation test"
