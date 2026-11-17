/* ARM64 compiler funclet call with the actual parent frame state. */
#include "kernel_offsets.inc"
.text
.p2align 4
.globl RtlArm64CallFunclet
RtlArm64CallFunclet:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mrs x9, cpacr_el1
    ubfx x9, x9, #20, #1
    str w9, [sp, #168]
    cbz x9, 1f
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [sp, #160]
    ldr d8, [x2, #(CTX_V+128)]
    ldr d9, [x2, #(CTX_V+144)]
    ldr d10, [x2, #(CTX_V+160)]
    ldr d11, [x2, #(CTX_V+176)]
    ldr d12, [x2, #(CTX_V+192)]
    ldr d13, [x2, #(CTX_V+208)]
    ldr d14, [x2, #(CTX_V+224)]
    ldr d15, [x2, #(CTX_V+240)]
    ldr w9, [x2, #CTX_FPCR]
    ldr w10, [x2, #CTX_FPSR]
    msr fpcr, x9
    msr fpsr, x10
1:  mov x16, x0
    mov x0, x1
    ldr x1, [x2, #CTX_FP]
    ldp x19, x20, [x2, #(CTX_X+152)]
    ldp x21, x22, [x2, #(CTX_X+168)]
    ldp x23, x24, [x2, #(CTX_X+184)]
    ldp x25, x26, [x2, #(CTX_X+200)]
    ldp x27, x28, [x2, #(CTX_X+216)]
    ldr x29, [x2, #CTX_FP]
    blr x16
    ldr w9, [sp, #168]
    cbz w9, 2f
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp w9, w10, [sp, #160]
    msr fpcr, x9
    msr fpsr, x10
2:
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #176
    ret
/* Nonreturning exception target restore. C validates EL1h, masked control,
 * resident target/stack, PCR identity and FP availability before entry. */
.p2align 4
.globl RtlArm64ResumeException
RtlArm64ResumeException:
    mov x16, x0
    ldr x17, [x16, #CTX_PC]
    msr elr_el1, x17
    ldr w17, [x16, #CTX_CPSR]
    msr spsr_el1, x17
    ldr w17, [x16, #CTX_FLAGS]
    tbz w17, #2, 3f
    ldr w17, [x16, #CTX_FPCR]
    msr fpcr, x17
    ldr w17, [x16, #CTX_FPSR]
    msr fpsr, x17
    ldp q0, q1, [x16, #(CTX_V+0)]
    ldp q2, q3, [x16, #(CTX_V+32)]
    ldp q4, q5, [x16, #(CTX_V+64)]
    ldp q6, q7, [x16, #(CTX_V+96)]
    ldp q8, q9, [x16, #(CTX_V+128)]
    ldp q10, q11, [x16, #(CTX_V+160)]
    ldp q12, q13, [x16, #(CTX_V+192)]
    ldp q14, q15, [x16, #(CTX_V+224)]
    ldp q16, q17, [x16, #(CTX_V+256)]
    ldp q18, q19, [x16, #(CTX_V+288)]
    ldp q20, q21, [x16, #(CTX_V+320)]
    ldp q22, q23, [x16, #(CTX_V+352)]
    ldp q24, q25, [x16, #(CTX_V+384)]
    ldp q26, q27, [x16, #(CTX_V+416)]
    ldp q28, q29, [x16, #(CTX_V+448)]
    ldp q30, q31, [x16, #(CTX_V+480)]
3:
    ldp x0, x1, [x16, #(CTX_X+0)]
    ldp x2, x3, [x16, #(CTX_X+16)]
    ldp x4, x5, [x16, #(CTX_X+32)]
    ldp x6, x7, [x16, #(CTX_X+48)]
    ldp x8, x9, [x16, #(CTX_X+64)]
    ldp x10, x11, [x16, #(CTX_X+80)]
    ldp x12, x13, [x16, #(CTX_X+96)]
    ldp x14, x15, [x16, #(CTX_X+112)]
    ldp x18, x19, [x16, #(CTX_X+144)]
    ldp x20, x21, [x16, #(CTX_X+160)]
    ldp x22, x23, [x16, #(CTX_X+176)]
    ldp x24, x25, [x16, #(CTX_X+192)]
    ldp x26, x27, [x16, #(CTX_X+208)]
    ldp x28, x29, [x16, #(CTX_X+224)]
    ldr x30, [x16, #(CTX_X+240)]
    ldr x17, [x16, #CTX_SP]
    mov sp, x17
    ldp x16, x17, [x16, #(CTX_X+128)]
    eret
