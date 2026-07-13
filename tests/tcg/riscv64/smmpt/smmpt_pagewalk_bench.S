    .section .text.init
    .globl _start

/*
 * Controlled SmMPT page-walk benchmark.
 *
 * Host/guest synchronization:
 *
 *   guest: BENCH_READY\n
 *   host:  smmpt-reset-stats
 *   host:  sends 'G' through UART
 *   guest: performs BENCH_ITERS targeted TLB invalidations and loads
 *   guest: BENCH_DONE\n
 *
 * Each measured iteration invalidates only TARGET_VA and then loads it.
 * The benchmark code and UART translations therefore remain cached.
 *
 * TARGET_VA uses a two-level Sv39 translation:
 *
 *   root_pt[1] -> target_l1_pt
 *   target_l1_pt[0] -> TARGET_PA as a 2-MiB leaf
 *
 * Therefore, each target page walk performs two PTE fetches:
 *
 *   root PTE page
 *   L1 PTE page
 *
 * Strict policy performs a full MPT lookup for both PTE fetches on every
 * iteration. Provenance policy should fill PTAC on the first walk and reuse
 * both PTE-page authorizations on subsequent walks.
 */

#ifndef BENCH_ITERS
#define BENCH_ITERS 100
#endif

    .equ UART_BASE,       0x10000000
    .equ UART_LSR,        5
    .equ UART_LSR_DR,     1

    .equ TARGET_VA,       0x40000000
    .equ TARGET_PA,       0x80400000

_start:
    la      t0, trap_handler
    csrw    stvec, t0

    /*
     * Clear root_pt and target_l1_pt.
     * They are adjacent, page-aligned 4-KiB pages.
     */
    la      t0, root_pt
    li      t1, 1024

clear_page_tables:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    addi    t1, t1, -1
    bnez    t1, clear_page_tables

    /*
     * root_pt[0]:
     * VA 0x00000000 -> PA 0x00000000, 1-GiB RWX leaf.
     * A and D are preset.
     */
    la      t0, root_pt
    li      t1, 0xcf
    sd      t1, 0(t0)

    /*
     * root_pt[2]:
     * VA 0x80000000 -> PA 0x80000000, 1-GiB RWX leaf.
     * Keeps benchmark code, data, page tables and UART-access code mapped.
     */
    li      t1, 0x80000
    slli    t1, t1, 10
    ori     t1, t1, 0xcf
    sd      t1, 16(t0)

    /*
     * root_pt[1] points to target_l1_pt.
     */
    la      t2, target_l1_pt
    srli    t1, t2, 12
    slli    t1, t1, 10
    ori     t1, t1, 0x1
    sd      t1, 8(t0)

    /*
     * target_l1_pt[0]:
     * TARGET_VA -> TARGET_PA as a 2-MiB RW leaf.
     * A and D are preset.
     */
    li      t1, TARGET_PA
    srli    t1, t1, 12
    slli    t1, t1, 10
    ori     t1, t1, 0xc7
    sd      t1, 0(t2)

    /*
     * Enable Sv39.
     */
    la      t0, root_pt
    srli    t0, t0, 12
    li      t1, 8
    slli    t1, t1, 60
    or      t0, t0, t1

    sfence.vma
    csrw    satp, t0
    sfence.vma

translated_entry:
    /*
     * Warm the complete target translation before the measurement window.
     * This fills PTAC for both root_pt and target_l1_pt under provenance.
     */
    li      t0, TARGET_VA
    ld      t1, 0(t0)
    sfence.vma t0, zero

    /*
     * Warm the exact translations used by the synchronization loop:
     * benchmark code, marker string and UART.
     */
    la      a0, ready_marker
    call    uart_puts

wait_for_go:
    li      t0, UART_BASE
    lbu     t1, UART_LSR(t0)
    andi    t1, t1, UART_LSR_DR
    beqz    t1, wait_for_go

    lbu     t1, 0(t0)
    li      t2, 'G'
    bne     t1, t2, wait_for_go

    /*
     * Measurement loop.
     *
     * sfence.vma TARGET_VA invalidates only the translated target mapping.
     * It does not deliberately flush code or UART translations.
     */
    li      s0, BENCH_ITERS
    li      s1, TARGET_VA
    li      s2, 0

    /*
     * BENCH_ITERS=0 is used to measure the fixed synchronization and
     * completion-path overhead of the harness.
     */
    beqz    s0, benchmark_complete

benchmark_loop:
    sfence.vma s1, zero
    ld      t0, 0(s1)
    xor     s2, s2, t0

    addi    s0, s0, -1
    bnez    s0, benchmark_loop

benchmark_complete:
    /*
     * Prevent the loads from being treated as dead benchmark work.
     */
    la      t0, benchmark_sink
    sd      s2, 0(t0)

    la      a0, done_marker
    call    uart_puts

done_loop:
    wfi
    j       done_loop

trap_handler:
    la      a0, fail_marker
    call    uart_puts

failure_loop:
    wfi
    j       failure_loop

/*
 * a0: address of zero-terminated string
 */
uart_puts:
    li      t0, UART_BASE

uart_puts_loop:
    lbu     t1, 0(a0)
    beqz    t1, uart_puts_done
    sb      t1, 0(t0)
    addi    a0, a0, 1
    j       uart_puts_loop

uart_puts_done:
    ret

    .section .rodata
ready_marker:
    .asciz  "BENCH_READY\n"

done_marker:
    .asciz  "BENCH_DONE\n"

fail_marker:
    .asciz  "BENCH_FAIL\n"

    .section .data
    .align 3

benchmark_sink:
    .dword  0

    .section .bss
    .align 12

root_pt:
    .space 4096

    .section .target_pt, "aw", @nobits
    .align 12

target_l1_pt:
    .space 4096
