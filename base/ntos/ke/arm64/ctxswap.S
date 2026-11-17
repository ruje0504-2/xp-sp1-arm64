/* Voluntary, DAIF-masked kernel call boundary. No user/interrupt state.
 * Save Windows ARM64 callee-preserved GPRs and low D8-D15; x18 stays PCR.
 * Full NT scheduler integration and switch-frame unwinding remain pending. */
#include "kernel_offsets.inc"
.text
.p2align 4
.globl KiArm64SwapContextRaw
KiArm64SwapContextRaw:
    sub sp, sp, #SW_SIZE
    stp x19, x20, [sp, #SW_X19]
    stp x21, x22, [sp, #SW_X19+16]
    stp x23, x24, [sp, #SW_X19+32]
    stp x25, x26, [sp, #SW_X19+48]
    stp x27, x28, [sp, #SW_X19+64]
    stp x29, x30, [sp, #SW_FP]
    mrs x9, cpacr_el1
    ubfx x9, x9, #20, #1
    str w9, [sp, #SW_HAS_FP]
    str wzr, [sp, #SW_RESERVED]
    cbz x9, 1f
    stp d8, d9, [sp, #SW_D8]
    stp d10, d11, [sp, #SW_D8+16]
    stp d12, d13, [sp, #SW_D8+32]
    stp d14, d15, [sp, #SW_D8+48]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [sp, #SW_FPCR]
1:  mov x9, sp
    str x9, [x0, #THREAD_KSTACK]
    ldr x9, [x1, #THREAD_KSTACK]
    ldr x10, [x18, #PCR_PRCB]
    str x1, [x10, #PRCB_THREAD]
    mov sp, x9
    ldr w9, [sp, #SW_HAS_FP]
    cbz w9, 2f
    ldp d8, d9, [sp, #SW_D8]
    ldp d10, d11, [sp, #SW_D8+16]
    ldp d12, d13, [sp, #SW_D8+32]
    ldp d14, d15, [sp, #SW_D8+48]
    ldp w9, w10, [sp, #SW_FPCR]
    msr fpcr, x9
    msr fpsr, x10
2:  ldp x19, x20, [sp, #SW_X19]
    ldp x21, x22, [sp, #SW_X19+16]
    ldp x23, x24, [sp, #SW_X19+32]
    ldp x25, x26, [sp, #SW_X19+48]
    ldp x27, x28, [sp, #SW_X19+64]
    ldp x29, x30, [sp, #SW_FP]
    add sp, sp, #SW_SIZE
    ret

.p2align 4
.globl KiArm64KernelEntry
KiArm64KernelEntry:
    mov x0, x20
    mov x1, x21
    blr x19
    /* A real NT system start routine must terminate or hand control away.
     * Never manufacture completion if it unexpectedly returns. */
    brk #0x7ff
3:  wfe
    b 3b

/* Fresh NT thread begins at dispatcher level, then runs the shared APC/start
 * prologue. Suspended call-boundary contexts resume through their saved LR. */
.p2align 4
.globl KiArm64NtKernelEntry
KiArm64NtKernelEntry:
    mov x0, x19
    mov x1, x20
    mov x2, x21
    bl KiArm64StartThread
    brk #0x7ff
4:  wfe
    b 4b
