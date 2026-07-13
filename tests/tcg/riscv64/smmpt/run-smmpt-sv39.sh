#!/usr/bin/env bash
set -euo pipefail

QEMU_ROOT="${QEMU_ROOT:-$(git rev-parse --show-toplevel)}"
OPENSBI_ROOT="${OPENSBI_ROOT:-$HOME/Documents/kvm/opensbi}"
WORKDIR="${WORKDIR:-$HOME/Documents/kvm/smmpt-integration}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

TESTDIR="$QEMU_ROOT/tests/tcg/riscv64/smmpt"
OUTDIR="$WORKDIR/sv39-test"
MONITOR=/tmp/qemu-smmpt-monitor

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

sleep 1

echo "=== Serial tail ==="
tail -20 "$WORKDIR/sv39-serial.log"

echo
echo "=== SmMPT status ==="
echo 'info smmpt' | socat - "UNIX-CONNECT:$MONITOR"

echo
echo "=== SmMPT MMU log ==="
rg -n \
    'SmMPT|PTE fetch|A/D update|denied' \
    "$WORKDIR/sv39-mmu.log" \
    | tail -50

echo
echo "=== Root page table ==="
printf 'xp /4gx 0x80201000\n' | \
    socat - "UNIX-CONNECT:$MONITOR"

echo 'quit' | socat - "UNIX-CONNECT:$MONITOR" >/dev/null
