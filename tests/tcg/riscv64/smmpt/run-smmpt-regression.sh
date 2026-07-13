#!/usr/bin/env bash
set -euo pipefail

QEMU_ROOT="${QEMU_ROOT:-$(git rev-parse --show-toplevel)}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

TESTDIR="$QEMU_ROOT/tests/tcg/riscv64/smmpt"

run_test()
{
    local name="$1"
    local script="$2"

    echo
    echo "============================================================"
    echo "Running: $name"
    echo "============================================================"

    CROSS_COMPILE="$CROSS_COMPILE" "$TESTDIR/$script"
}

run_test \
    "strict SmMPT Sv39 integration" \
    "run-smmpt-sv39.sh"

run_test \
    "PTAC permission revocation" \
    "run-smmpt-sv39-revoke.sh"

run_test \
    "mfence.mcpa privilege boundary" \
    "run-smmpt-mfence-priv.sh"

echo
echo "============================================================"
echo "[pass] all SmMPT regression tests"
echo "============================================================"
