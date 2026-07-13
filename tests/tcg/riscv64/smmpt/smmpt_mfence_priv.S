    .section .text.init
    .globl _start

/*
 * Verify that experimental mfence.mcpa is restricted to M-mode.
 *
 * This test starts in M-mode without OpenSBI, installs an M-mode trap
 * handler, and enters S-mode with mret. S-mode then executes mfence.mcpa.
 *
 * Expected result:
 *
 *   - machine trap cause is illegal instruction;
 *   - mepc points to mfence_site;
 *   - mtval is either zero or the instruction encoding;
 *   - the SmMPT mfence helper is not executed.
 *
 * Success marker: MFI
 * Failure marker: MFF
 */

    .equ UART_BASE, 0x10000000
    .equ CAUSE_ILLEGAL_INSTRUCTION, 2
    .equ MFENCE_MCPA_ENCODING, 0x24000073

    /* mstatus.MPP field */
    .equ MSTATUS_MPP_MASK,  0x1800
    .equ MSTATUS_MPP_S,     0x0800

_start:
    /* All traps, including S-mode illegal instructions, return to M-mode. */
    la      t0, machine_trap_handler
    csrw    mtvec, t0

    /*
     * Permit S-mode instruction fetches and data accesses. Without an
     * M-mode firmware such as OpenSBI, PMP has not yet been configured.
     */
    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x0f
    csrw    pmpcfg0, t0

    /* Set MPP=S while preserving unrelated mstatus fields. */
    csrr    t0, mstatus
    li      t1, MSTATUS_MPP_MASK
    not     t1, t1
    and     t0, t0, t1
    li      t1, MSTATUS_MPP_S
    or      t0, t0, t1
    csrw    mstatus, t0

    la      t0, supervisor_entry
    csrw    mepc, t0
    mret

supervisor_entry:
mfence_site:
    .word   MFENCE_MCPA_ENCODING

    /* Reaching here means S-mode incorrectly executed the instruction. */
    j       failure

    /*
     * mtvec.BASE must be at least 4-byte aligned. Compressed instructions
     * above may otherwise place the handler at a 2-byte-only boundary.
     */
    .balign 4
machine_trap_handler:
    csrr    t0, mcause
    li      t1, CAUSE_ILLEGAL_INSTRUCTION
    bne     t0, t1, failure

    csrr    t0, mepc
    la      t1, mfence_site
    bne     t0, t1, failure

    /*
     * For an illegal instruction, mtval may contain the instruction
     * encoding or zero. Accept either result.
     */
    csrr    t0, mtval
    beqz    t0, success

    li      t1, MFENCE_MCPA_ENCODING
    bne     t0, t1, failure

success:
    li      t0, UART_BASE
    li      t1, 'M'
    sb      t1, 0(t0)
    li      t1, 'F'
    sb      t1, 0(t0)
    li      t1, 'I'
    sb      t1, 0(t0)

success_loop:
    wfi
    j       success_loop

failure:
    li      t0, UART_BASE
    li      t1, 'M'
    sb      t1, 0(t0)
    li      t1, 'F'
    sb      t1, 0(t0)
    li      t1, 'F'
    sb      t1, 0(t0)

failure_loop:
    wfi
    j       failure_loop
