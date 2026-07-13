#!/usr/bin/env bash
set -euo pipefail

QEMU_ROOT="${QEMU_ROOT:-$(git rev-parse --show-toplevel)}"
OPENSBI_ROOT="${OPENSBI_ROOT:-$HOME/Documents/kvm/opensbi}"
WORKDIR="${WORKDIR:-$HOME/Documents/kvm/smmpt-integration}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

TESTDIR="$QEMU_ROOT/tests/tcg/riscv64/smmpt"
OUTDIR="$WORKDIR/mfence-priv-test"
MONITOR="/tmp/qemu-smmpt-mfence-priv-monitor"
SERIAL_LOG="$WORKDIR/mfence-priv-serial.log"

mkdir -p "$OUTDIR"

"${CROSS_COMPILE}gcc" \
    -march=rv64imac_zicsr_zifencei \
    -mabi=lp64 \
    -mcmodel=medany \
    -nostdlib \
    -nostartfiles \
    -ffreestanding \
    -Wl,-T,"$TESTDIR/smmpt_mfence_priv.ld" \
    -Wl,--build-id=none \
    -o "$OUTDIR/smmpt_mfence_priv.elf" \
    "$TESTDIR/smmpt_mfence_priv.S"

"${CROSS_COMPILE}objcopy" \
    -O binary \
    "$OUTDIR/smmpt_mfence_priv.elf" \
    "$OUTDIR/smmpt_mfence_priv.bin"

rm -f "$MONITOR" "$SERIAL_LOG"

"$QEMU_ROOT/build/qemu-system-riscv64" \
    -machine virt \
    -cpu rv64,smmpt-policy=2 \
    -m 512M \
    -smp 1 \
    -bios "$OUTDIR/smmpt_mfence_priv.bin" \
    -display none \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR,server,nowait" \
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
    if grep -q 'MFI\|MFF' "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

echo '=== serial tail ==='
tail -30 "$SERIAL_LOG"

if grep -q 'MFF' "$SERIAL_LOG"; then
    echo "[fail] S-mode mfence.mcpa did not raise the expected exception"
    exit 1
fi

if ! grep -q 'MFI' "$SERIAL_LOG"; then
    echo "[fail] S-mode illegal-instruction marker was not observed"
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

MFENCE_INVALIDATES="$(get_counter mfence-invalidates)"

case "$MFENCE_INVALIDATES" in
    ''|*[!0-9]*)
        echo "[fail] unable to parse mfence-invalidates counter"
        exit 1
        ;;
esac

if [ "$MFENCE_INVALIDATES" -ne 0 ]; then
    echo "[fail] S-mode execution reached the mfence.mcpa helper"
    exit 1
fi

echo "[pass] S-mode mfence.mcpa privilege test"
