    .section .text.init
    .globl _start

/*
 * SmMPT PTAC revocation test
 *
 * Code and trap handling remain in the 0x80000000 identity-mapped region.
 *
 * A separate translated address is constructed:
 *
 *   VA 0x40000000
 *       root_pt[1] -> target_l1_pt
 *       target_l1_pt[0] -> PA 0x80200000 as a 2-MiB leaf
 *
 * The physical page containing target_l1_pt is first authorized and reused by
 * PTAC. OpenSBI then revokes MPT read permission for that physical page and
 * executes mfence.mcpa. A subsequent page walk must miss in PTAC and be denied.
 */

    .equ UART_BASE,            0x10000000
    .equ TARGET_VA,            0x40000000
    .equ SBI_EXT_TEST,         0x66666666
    .equ SBI_FID_PERM_REVISE,  0x1a

_start:
    /* Install trap vector while translation is still disabled. */
    la      t0, trap_handler
    csrw    stvec, t0

    /* Clear root and L1 page-table pages. */
    la      t0, root_pt
    li      t1, 1024

1:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    addi    t1, t1, -1
    bnez    t1, 1b

    /*
     * root_pt[0]:
     * VA 0x00000000 -> PA 0x00000000, 1-GiB RWX leaf, A/D preset.
     */
    la      t0, root_pt
    li      t1, 0xcf
    sd      t1, 0(t0)

    /*
     * root_pt[2]:
     * VA 0x80000000 -> PA 0x80000000, 1-GiB RWX leaf, A/D preset.
     *
     * Keeps test code, trap handler, stackless ecall path and data accessible
     * after target_l1_pt permission is revoked.
     */
    li      t1, 0x80000
    slli    t1, t1, 10
    ori     t1, t1, 0xcf
    sd      t1, 16(t0)

    /*
     * root_pt[1] points to target_l1_pt.
     * Non-leaf PTE = (physical PPN << 10) | V.
     */
    la      t2, target_l1_pt
    srli    t1, t2, 12
    slli    t1, t1, 10
    ori     t1, t1, 0x1
    sd      t1, 8(t0)

    /*
     * target_l1_pt[0]:
     * VA 0x40000000 -> PA 0x80200000 as a 2-MiB RWX leaf.
     *
     * PA 0x80200000 is 2-MiB aligned.
     * PPN = 0x80200000 >> 12 = 0x80200.
     */
    li      t1, 0x80200
    slli    t1, t1, 10
    ori     t1, t1, 0xcf
    sd      t1, 0(t2)

    /* Enable Sv39 using root_pt. */
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
     * First walk:
     * target_l1_pt authorization should miss and fill PTAC.
     */
    li      t0, TARGET_VA
    ld      t1, 0(t0)

    li      t0, UART_BASE
    li      t1, 'R'
    sb      t1, 0(t0)

    /*
     * Flush architectural translation state so the next access performs
     * another page walk. The target_l1_pt PTE-page authorization should hit
     * in PTAC.
     */
    sfence.vma

    li      t0, TARGET_VA
    ld      t1, 0(t0)

    li      t0, UART_BASE
    li      t1, 'H'
    sb      t1, 0(t0)

    /*
     * Ask OpenSBI to revoke all SmMPT permissions for the physical page
     * containing target_l1_pt.
     *
     * a0 = physical address whose MPT tuple is changed
     * a1 = 0, and OpenSBI computes new_perm = a1 >> 3 = PERMS_NOACCESS
     * a6 = function ID
     * a7 = extension ID
     */
    la      a0, target_l1_pt
    li      a1, 0
    li      a6, SBI_FID_PERM_REVISE
    li      a7, SBI_EXT_TEST
    ecall

    /* SBI error is returned in a0. */
    bnez    a0, ecall_failed

    /*
     * Force a third page walk. mfence.mcpa should already have invalidated
     * PTAC. Reading target_l1_pt must now perform a full MPT lookup and fail.
     */
    sfence.vma

    li      t0, TARGET_VA
    ld      t1, 0(t0)

    /* Reaching here means stale authorization incorrectly survived. */
    li      t0, UART_BASE
    li      t1, 'F'
    sb      t1, 0(t0)
    j       failure_loop

ecall_failed:
    li      t0, UART_BASE
    li      t1, 'E'
    sb      t1, 0(t0)
    j       failure_loop

trap_handler:
    /*
     * A trap after R and H is expected only after the revoked page-table page
     * is fetched again. SmMPT denied counters distinguish this from an
     * unrelated trap.
     */
    li      t0, UART_BASE
    li      t1, 'T'
    sb      t1, 0(t0)

trap_loop:
    wfi
    j       trap_loop

failure_loop:
    wfi
    j       failure_loop

    .section .bss
    .align 12

root_pt:
    .space 4096

    /*
     * Keep the revocable page-table page in a separate level-2 MPT
     * permission tuple from code, trap_handler, and root_pt.
     */
    .section .target_pt, "aw", @nobits
    .align 12

target_l1_pt:
    .space 4096
