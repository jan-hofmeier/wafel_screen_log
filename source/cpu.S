@ wafel_screen_log - small ARM926 helpers (privileged ones must only be
@ called when (cpsr & 0x1f) != 0x10).

.arm
.text

@ u32 cpu_get_cpsr(void)   -- fine in any mode
.globl cpu_get_cpsr
.type cpu_get_cpsr, %function
cpu_get_cpsr:
    mrs     r0, cpsr
    bx      lr

@ u32 cpu_cp15_ctrl_read(void)            [privileged]
.globl cpu_cp15_ctrl_read
.type cpu_cp15_ctrl_read, %function
cpu_cp15_ctrl_read:
    mrc     p15, 0, r0, c1, c0, 0
    bx      lr

@ void cpu_cp15_ctrl_write(u32 v)          [privileged]
.globl cpu_cp15_ctrl_write
.type cpu_cp15_ctrl_write, %function
cpu_cp15_ctrl_write:
    mcr     p15, 0, r0, c1, c0, 0
    nop
    nop
    nop
    bx      lr

@ void cpu_dcache_clean_all(void)          [privileged]
@ test-and-clean loop + drain write buffer
.globl cpu_dcache_clean_all
.type cpu_dcache_clean_all, %function
cpu_dcache_clean_all:
1:  mrc     p15, 0, r15, c7, c10, 3
    bne     1b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4
    bx      lr

@ void cpu_dcache_invalidate_all(void)     [privileged]
.globl cpu_dcache_invalidate_all
.type cpu_dcache_invalidate_all, %function
cpu_dcache_invalidate_all:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c6, 0
    bx      lr

@ void cpu_dcache_clean_range(void *p, u32 size)   [privileged]
.globl cpu_dcache_clean_range
.type cpu_dcache_clean_range, %function
cpu_dcache_clean_range:
    cmp     r1, #0
    beq     2f
    add     r1, r0, r1
    bic     r0, r0, #0x1F
1:  mcr     p15, 0, r0, c7, c10, 1
    add     r0, r0, #0x20
    cmp     r0, r1
    blo     1b
2:  mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4
    bx      lr

@ u32 cpu_irq_disable(void)  -> old cpsr   [privileged]
.globl cpu_irq_disable
.type cpu_irq_disable, %function
cpu_irq_disable:
    mrs     r0, cpsr
    orr     r1, r0, #0xC0
    msr     cpsr_c, r1
    bx      lr

@ void cpu_irq_restore(u32 old_cpsr)       [privileged]
.globl cpu_irq_restore
.type cpu_irq_restore, %function
cpu_irq_restore:
    msr     cpsr_c, r0
    bx      lr

@ void cpu_call_on_stack(void (*fn)(void*), void *arg, void *stack_top)
@ Runs fn(arg) with sp = stack_top, then restores the old sp.
.globl cpu_call_on_stack
.type cpu_call_on_stack, %function
cpu_call_on_stack:
    push    {r4, lr}
    mov     r3, r0
    mov     r0, r1
    mov     r4, sp
    mov     sp, r2
    blx     r3
    mov     sp, r4
    pop     {r4, pc}
