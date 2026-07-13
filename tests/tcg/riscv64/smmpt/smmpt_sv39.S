    .section .text.init
    .globl _start

/*
 * OpenSBI enters here in S-mode at physical address 0x80200000.
 *
 * The test creates an Sv39 root table with two 1-GiB identity mappings:
 *
 *   VA 0x00000000-0x3fffffff -> PA 0x00000000-0x3fffffff
 *   VA 0x80000000-0xbfffffff -> PA 0x80000000-0xbfffffff
 *
 * A/D bits are deliberately clear. The first translated accesses should
 * force QEMU to update them, exercising SmMPT checks on PTE A/D writes.
 */

_start:
    /* Install an S-mode trap vector before enabling translation. */
    la      t0, trap_handler
    csrw    stvec, t0

    /* Clear one 4-KiB root page table. */
    la      t0, root_pt
    li      t1, 512

1:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    addi    t1, t1, -1
    bnez    t1, 1b

    /*
     * Sv39 root entry 0:
     * 1-GiB leaf mapping VA 0x00000000 -> PA 0x00000000.
     *
     * PTE flags:
     * V=1, R=1, W=1, X=1
     * A=0, D=0 intentionally
     */
    la      t0, root_pt
    li      t1, 0x0f
    sd      t1, 0(t0)

    /*
     * Sv39 root entry 2:
     * 1-GiB leaf mapping VA 0x80000000 -> PA 0x80000000.
     *
     * PPN = 0x80000000 >> 12 = 0x80000
     * PTE = (PPN << 10) | V | R | W | X
     */
    li      t1, 0x80000
    slli    t1, t1, 10
    ori     t1, t1, 0x0f
    sd      t1, 16(t0)

    /* satp.MODE=8 (Sv39), ASID=0, PPN=root_pt>>12. */
    la      t0, root_pt
    srli    t0, t0, 12

    li      t1, 8
    slli    t1, t1, 60
    or      t0, t0, t1

    sfence.vma
    csrw    satp, t0
    sfence.vma

    /*
     * Execution continues through the identity mapping.
     * The first instruction fetch should perform:
     *
     *   PTE fetch SmMPT check
     *   optional A-bit PTE update SmMPT check
     *   final PA SmMPT check
     */

translated_entry:
    /* Force a translated load and store. */
    la      t0, test_value
    ld      t1, 0(t0)
    addi    t1, t1, 1
    sd      t1, 0(t0)

    /* Print 'S' using the identity-mapped UART. */
    li      t0, 0x10000000
    li      t1, 'S'
    sb      t1, 0(t0)

success_loop:
    wfi
    j       success_loop

trap_handler:
    /* Print 'T' if an S-mode trap occurs. */
    li      t0, 0x10000000
    li      t1, 'T'
    sb      t1, 0(t0)

trap_loop:
    wfi
    j       trap_loop

    .section .data
    .align 3

test_value:
    .dword 0x123456789abcdef0

    .section .bss
    .align 12

root_pt:
    .space 4096
