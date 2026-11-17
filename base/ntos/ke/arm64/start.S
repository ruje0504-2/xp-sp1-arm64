/* Non-returning early handoff on the separately allocated kernel stack.
 * Full NT initialization remains beyond the explicit continuation. */
.text
.p2align 4
.globl KiArm64EnterBoot
KiArm64EnterBoot:
    msr daifset, #15
    mov sp, x1
    mov x19, x0
    mov x29, xzr
    mov x30, xzr
    bl KiArm64InitializeBootStructures
    mov w1, w0
    mov x0, x19
    bl KiArm64BootContinuation
1:  wfe
    b 1b
