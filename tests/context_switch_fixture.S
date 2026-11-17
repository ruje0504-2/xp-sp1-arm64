.text
.p2align 4
.globl XpContextRegisterFixture
XpContextRegisterFixture:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    add x29, sp, #80
    str x2, [sp, #168]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [sp, #160]
    mov x19, #20499
    mov x20, #20500
    mov x21, #20501
    mov x22, #20502
    mov x23, #20503
    mov x24, #20504
    mov x25, #20505
    mov x26, #20506
    mov x27, #20507
    mov x28, #20508
    mov x9, #24584
    fmov d8, x9
    mov x9, #24585
    fmov d9, x9
    mov x9, #24586
    fmov d10, x9
    mov x9, #24587
    fmov d11, x9
    mov x9, #24588
    fmov d12, x9
    mov x9, #24589
    fmov d13, x9
    mov x9, #24590
    fmov d14, x9
    mov x9, #24591
    fmov d15, x9
    mov x9, #0x400000
    msr fpcr, x9
    mov x9, #0x8000000
    msr fpsr, x9
    bl KiArm64SwitchKernelContext
    ldr x9, [sp, #168]
    str w0, [x9, #152]
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    stp d8, d9, [x9, #80]
    stp d10, d11, [x9, #96]
    stp d12, d13, [x9, #112]
    stp d14, d15, [x9, #128]
    mrs x10, fpcr
    mrs x11, fpsr
    stp w10, w11, [x9, #144]
    ldp w9, w10, [sp, #160]
    msr fpcr, x9
    msr fpsr, x10
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #176
    ret

.p2align 2
.globl XpReadFpcr
XpReadFpcr:
    mrs x0, fpcr
    ret
.globl XpReadFpsr
XpReadFpsr:
    mrs x0, fpsr
    ret
.globl XpWriteFpState
XpWriteFpState:
    msr fpcr, x0
    msr fpsr, x1
    ret
