/* QEMU control: preserve caller ABI, seed registers, take BRK, record return. */
.text
.p2align 2
.globl XpTrapFixture
.globl XpExpectedBrk
XpTrapFixture:
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
    mov x30, #4126
    movi v0.16b, #16
    movi v1.16b, #17
    movi v2.16b, #18
    movi v3.16b, #19
    movi v4.16b, #20
    movi v5.16b, #21
    movi v6.16b, #22
    movi v7.16b, #23
    movi v8.16b, #24
    movi v9.16b, #25
    movi v10.16b, #26
    movi v11.16b, #27
    movi v12.16b, #28
    movi v13.16b, #29
    movi v14.16b, #30
    movi v15.16b, #31
    movi v16.16b, #32
    movi v17.16b, #33
    movi v18.16b, #34
    movi v19.16b, #35
    movi v20.16b, #36
    movi v21.16b, #37
    movi v22.16b, #38
    movi v23.16b, #39
    movi v24.16b, #40
    movi v25.16b, #41
    movi v26.16b, #42
    movi v27.16b, #43
    movi v28.16b, #44
    movi v29.16b, #45
    movi v30.16b, #46
    movi v31.16b, #47
    cmp xzr, xzr
XpExpectedBrk:
    brk #0x1234
    stp x0, x1, [x0, #8]
    stp x2, x3, [x0, #24]
    stp x4, x5, [x0, #40]
    stp x6, x7, [x0, #56]
    stp x8, x9, [x0, #72]
    stp x10, x11, [x0, #88]
    stp x12, x13, [x0, #104]
    stp x14, x15, [x0, #120]
    stp x16, x17, [x0, #136]
    stp x18, x19, [x0, #152]
    stp x20, x21, [x0, #168]
    stp x22, x23, [x0, #184]
    stp x24, x25, [x0, #200]
    stp x26, x27, [x0, #216]
    stp x28, x29, [x0, #232]
    str x30, [x0, #248]
    mrs x9, nzcv
    str w9, [x0, #4]
    stp q0, q1, [x0, #272]
    stp q2, q3, [x0, #304]
    stp q4, q5, [x0, #336]
    stp q6, q7, [x0, #368]
    stp q8, q9, [x0, #400]
    stp q10, q11, [x0, #432]
    stp q12, q13, [x0, #464]
    stp q14, q15, [x0, #496]
    stp q16, q17, [x0, #528]
    stp q18, q19, [x0, #560]
    stp q20, q21, [x0, #592]
    stp q22, q23, [x0, #624]
    stp q24, q25, [x0, #656]
    stp q26, q27, [x0, #688]
    stp q28, q29, [x0, #720]
    stp q30, q31, [x0, #752]
    ldp q8, q9, [sp, #96]
    ldp q10, q11, [sp, #128]
    ldp q12, q13, [sp, #160]
    ldp q14, q15, [sp, #192]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #320
    ret
