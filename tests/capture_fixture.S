/* Seed registers around a real ARM64 call; restore all ABI nonvolatiles. */
#ifdef __APPLE__
#define SYMBOL(n) _##n
#else
#define SYMBOL(n) n
#endif
.text
.p2align 2
.globl SYMBOL(XpCaptureFixture)
.globl SYMBOL(XpCaptureResume)
SYMBOL(XpCaptureFixture):
    sub sp, sp, #320
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp q8, q9, [sp, #96]
    stp q10, q11, [sp, #128]
    stp q12, q13, [sp, #160]
    stp q14, q15, [sp, #192]
    mrs x1, nzcv
    str x1, [sp, #224]
    mov x1, #4097
    mov x2, #4098
    mov x3, #4099
    mov x4, #4100
    mov x5, #4101
    mov x6, #4102
    mov x7, #4103
    mov x8, #4104
    mov x9, #4105
    mov x10, #4106
    mov x11, #4107
    mov x12, #4108
    mov x13, #4109
    mov x14, #4110
    mov x15, #4111
    mov x16, #4112
    mov x17, #4113
    mov x19, #4115
    mov x20, #4116
    mov x21, #4117
    mov x22, #4118
    mov x23, #4119
    mov x24, #4120
    mov x25, #4121
    mov x26, #4122
    mov x27, #4123
    mov x28, #4124
    mov x29, #4125
    movi v0.16b, #0
    movi v1.16b, #1
    movi v2.16b, #2
    movi v3.16b, #3
    movi v4.16b, #4
    movi v5.16b, #5
    movi v6.16b, #6
    movi v7.16b, #7
    movi v8.16b, #8
    movi v9.16b, #9
    movi v10.16b, #10
    movi v11.16b, #11
    movi v12.16b, #12
    movi v13.16b, #13
    movi v14.16b, #14
    movi v15.16b, #15
    movi v16.16b, #16
    movi v17.16b, #17
    movi v18.16b, #18
    movi v19.16b, #19
    movi v20.16b, #20
    movi v21.16b, #21
    movi v22.16b, #22
    movi v23.16b, #23
    movi v24.16b, #24
    movi v25.16b, #25
    movi v26.16b, #26
    movi v27.16b, #27
    movi v28.16b, #28
    movi v29.16b, #29
    movi v30.16b, #30
    movi v31.16b, #31
    cmp xzr, xzr
    bl SYMBOL(RtlCaptureContext)
SYMBOL(XpCaptureResume):
    mov x0, sp
    ldp q8, q9, [sp, #96]
    ldp q10, q11, [sp, #128]
    ldp q12, q13, [sp, #160]
    ldp q14, q15, [sp, #192]
    ldr x1, [sp, #224]
    msr nzcv, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #320
    ret
