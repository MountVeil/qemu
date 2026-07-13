#!/usr/bin/env bash
set -euo pipefail

QEMU_ROOT="${QEMU_ROOT:-$(git rev-parse --show-toplevel)}"
OPENSBI_ROOT="${OPENSBI_ROOT:-$HOME/Documents/kvm/opensbi}"
WORKDIR="${WORKDIR:-$HOME/Documents/kvm/smmpt-integration}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"
ITERATIONS="${ITERATIONS:-100}"

TESTDIR="$QEMU_ROOT/tests/tcg/riscv64/smmpt"
OUTDIR="$WORKDIR/pagewalk-bench"
CSV="$OUTDIR/pagewalk-results.csv"

mkdir -p "$OUTDIR"

"${CROSS_COMPILE}gcc" \
    -march=rv64imac_zicsr_zifencei \
    -mabi=lp64 \
    -mcmodel=medany \
    -nostdlib \
    -nostartfiles \
    -ffreestanding \
    -DBENCH_ITERS="$ITERATIONS" \
    -Wl,-T,"$TESTDIR/smmpt_pagewalk_bench.ld" \
    -Wl,--build-id=none \
    -o "$OUTDIR/smmpt_pagewalk_bench.elf" \
    "$TESTDIR/smmpt_pagewalk_bench.S"

printf '%s\n' \
'policy,iterations,tlb_fills,lookup_requests,entry_reads,final_checks,pte_fetch_checks,ad_update_checks,policy_skips,ptac_lookups,ptac_hits,ptac_misses,ptac_fills,pte_full_lookups,pte_reuses' \
> "$CSV"

run_policy()
{
    local policy_id="$1"
    local policy_name="$2"

    local monitor="/tmp/qemu-smmpt-pagewalk-${policy_name}-monitor"
    local serial="/tmp/qemu-smmpt-pagewalk-${policy_name}-serial"
    local info_file="$OUTDIR/${policy_name}-info.txt"
    local transcript="$OUTDIR/${policy_name}-serial.log"

    rm -f "$monitor" "$serial" "$info_file" "$transcript"

    "$QEMU_ROOT/build/qemu-system-riscv64" \
        -machine virt \
        -cpu "rv64,smmpt-policy=$policy_id" \
        -m 512M \
        -smp 1 \
        -bios "$OPENSBI_ROOT/build/platform/generic/firmware/fw_jump.bin" \
        -kernel "$OUTDIR/smmpt_pagewalk_bench.elf" \
        -dtb "$WORKDIR/virt-smmpt.dtb" \
        -display none \
        -chardev "socket,id=benchserial,path=$serial,server=on,wait=off" \
        -serial chardev:benchserial \
        -monitor "unix:$monitor,server=on,wait=off" \
        -daemonize

    cleanup_policy()
    {
        if [ -S "$monitor" ]; then
            echo quit |
                socat - "UNIX-CONNECT:$monitor" >/dev/null 2>&1 || true
        fi
        rm -f "$monitor" "$serial"
    }

    trap cleanup_policy RETURN

    python3 - \
        "$serial" \
        "$monitor" \
        "$info_file" \
        "$transcript" <<'PYCLIENT'
import socket
import sys
import time
from pathlib import Path

serial_path = sys.argv[1]
monitor_path = sys.argv[2]
info_path = Path(sys.argv[3])
transcript_path = Path(sys.argv[4])

READY = b"BENCH_READY\n"
DONE = b"BENCH_DONE\n"
FAIL = b"BENCH_FAIL\n"


def connect_unix(path: str, timeout: float = 10.0) -> socket.socket:
    deadline = time.monotonic() + timeout
    last_error = None

    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            sock.settimeout(0.2)
            return sock
        except OSError as exc:
            last_error = exc
            sock.close()
            time.sleep(0.05)

    raise RuntimeError(f"unable to connect to {path}: {last_error}")


def hmp_command(command: str) -> str:
    sock = connect_unix(monitor_path)
    data = bytearray()

    try:
        deadline = time.monotonic() + 5.0

        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue

            if not chunk:
                break

            data.extend(chunk)
            if b"(qemu)" in data:
                break

        sock.sendall(command.encode("ascii") + b"\n")

        response = bytearray()
        deadline = time.monotonic() + 5.0

        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue

            if not chunk:
                break

            response.extend(chunk)
            if b"(qemu)" in response:
                break

        return response.decode("utf-8", errors="replace").replace("\r", "")
    finally:
        sock.close()


serial_sock = connect_unix(serial_path)
serial_data = bytearray()

try:
    deadline = time.monotonic() + 15.0

    while READY not in serial_data:
        if time.monotonic() >= deadline:
            raise RuntimeError("BENCH_READY marker was not observed")

        try:
            chunk = serial_sock.recv(4096)
        except socket.timeout:
            continue

        if not chunk:
            raise RuntimeError("serial connection closed before BENCH_READY")

        serial_data.extend(chunk)

        if FAIL in serial_data:
            raise RuntimeError("guest reported BENCH_FAIL before measurement")

    stop_response = hmp_command("stop")

    reset_response = hmp_command("smmpt-reset-stats")
    if "SmMPT statistics reset for selected CPU" not in reset_response:
        raise RuntimeError(
            "smmpt-reset-stats did not report success:\n" + reset_response
        )

    # Queue the start byte while the vCPU is stopped. The guest consumes it
    # immediately after cont, avoiding wait-loop activity in the measurement.
    serial_sock.sendall(b"G")

    cont_response = hmp_command("cont")

    deadline = time.monotonic() + 15.0

    while DONE not in serial_data:
        if time.monotonic() >= deadline:
            raise RuntimeError("BENCH_DONE marker was not observed")

        try:
            chunk = serial_sock.recv(4096)
        except socket.timeout:
            continue

        if not chunk:
            raise RuntimeError("serial connection closed before BENCH_DONE")

        serial_data.extend(chunk)

        if FAIL in serial_data:
            raise RuntimeError("guest reported BENCH_FAIL")

    info = hmp_command("info smmpt")
    info_path.write_text(info)
    transcript_path.write_bytes(serial_data)

finally:
    serial_sock.close()
PYCLIENT

    cat "$info_file"

    get_counter()
    {
        local name="$1"

        awk -F: -v key="$name" '
            {
                field = $1
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", field)

                if (field == key) {
                    value = $2
                    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
                    print value
                    exit
                }
            }
        ' "$info_file"
    }

    local tlb_fills
    local lookup_requests
    local entry_reads
    local final_checks
    local pte_fetch_checks
    local ad_update_checks
    local policy_skips
    local ptac_lookups
    local ptac_hits
    local ptac_misses
    local ptac_fills
    local pte_full_lookups
    local pte_reuses

    tlb_fills="$(get_counter tlb-fills)"
    lookup_requests="$(get_counter lookup-requests)"
    entry_reads="$(get_counter entry-reads)"
    final_checks="$(get_counter final-checks)"
    pte_fetch_checks="$(get_counter pte-fetch-checks)"
    ad_update_checks="$(get_counter ad-update-checks)"
    policy_skips="$(get_counter policy-skips)"
    ptac_lookups="$(get_counter lookups)"
    ptac_hits="$(get_counter hits)"
    ptac_misses="$(get_counter misses)"
    ptac_fills="$(get_counter fills)"
    pte_full_lookups="$(get_counter pte-full-lookups)"
    pte_reuses="$(get_counter pte-reuses)"

    for value in \
        "$tlb_fills" \
        "$lookup_requests" \
        "$entry_reads" \
        "$final_checks" \
        "$pte_fetch_checks" \
        "$ad_update_checks" \
        "$policy_skips" \
        "$ptac_lookups" \
        "$ptac_hits" \
        "$ptac_misses" \
        "$ptac_fills" \
        "$pte_full_lookups" \
        "$pte_reuses"
    do
        case "$value" in
            ''|*[!0-9]*)
                echo "[fail] unable to parse SmMPT counters for $policy_name"
                exit 1
                ;;
        esac
    done

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$policy_name" \
        "$ITERATIONS" \
        "$tlb_fills" \
        "$lookup_requests" \
        "$entry_reads" \
        "$final_checks" \
        "$pte_fetch_checks" \
        "$ad_update_checks" \
        "$policy_skips" \
        "$ptac_lookups" \
        "$ptac_hits" \
        "$ptac_misses" \
        "$ptac_fills" \
        "$pte_full_lookups" \
        "$pte_reuses" \
        >> "$CSV"

    cleanup_policy
    trap - RETURN
}

run_policy 0 disabled
run_policy 1 strict
run_policy 2 provenance

echo
echo "=== SmMPT page-walk results ==="
column -s, -t "$CSV" 2>/dev/null || cat "$CSV"

echo
echo "[pass] SmMPT page-walk benchmark"
echo "CSV: $CSV"
