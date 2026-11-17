.text
.p2align 4
.globl XpUnwindFullFixture
.seh_proc XpUnwindFullFixture
XpUnwindFullFixture:
    stp x19, x20, [x0, #160]
    .seh_nop
    stp x21, x22, [x0, #176]
    .seh_nop
    stp x23, x24, [x0, #192]
    .seh_nop
    stp x25, x26, [x0, #208]
    .seh_nop
    stp x27, x28, [x0, #224]
    .seh_nop
    stp x29, x30, [x0, #240]
    .seh_nop
    str x18, [x0, #152]
    .seh_nop
    mov x9, sp
    .seh_nop
    str x9, [x0, #256]
    .seh_nop
    str x30, [x0, #264]
    .seh_nop
    str d8, [x0, #400]
    .seh_nop
    str d9, [x0, #416]
    .seh_nop
    str d10, [x0, #432]
    .seh_nop
    str d11, [x0, #448]
    .seh_nop
    str d12, [x0, #464]
    .seh_nop
    str d13, [x0, #480]
    .seh_nop
    str d14, [x0, #496]
    .seh_nop
    str d15, [x0, #512]
    .seh_nop
    brk #0x181
    .seh_nop
    stp x19, x20, [sp, #-160]!
    .seh_save_r19r20_x 160
    brk #0x182
    .seh_nop
    stp x21, x22, [sp, #16]
    .seh_save_regp x21, 16
    stp x23, x24, [sp, #32]
    .seh_save_regp x23, 32
    stp x25, x26, [sp, #48]
    .seh_save_regp x25, 48
    stp x27, x28, [sp, #64]
    .seh_save_regp x27, 64
    stp x29, x30, [sp, #80]
    .seh_save_fplr 80
    stp d8, d9, [sp, #96]
    .seh_save_fregp d8, 96
    stp d10, d11, [sp, #112]
    .seh_save_fregp d10, 112
    stp d12, d13, [sp, #128]
    .seh_save_fregp d12, 128
    stp d14, d15, [sp, #144]
    .seh_save_fregp d14, 144
    add x29, sp, #80
    .seh_add_fp 80
    brk #0x183
    .seh_nop
    .seh_endprologue
    mov x19, #28691
    mov x20, #28692
    mov x21, #28693
    mov x22, #28694
    mov x23, #28695
    mov x24, #28696
    mov x25, #28697
    mov x26, #28698
    mov x27, #28699
    mov x28, #28700
    mov x9, #32776
    fmov d8, x9
    mov x9, #32777
    fmov d9, x9
    mov x9, #32778
    fmov d10, x9
    mov x9, #32779
    fmov d11, x9
    mov x9, #32780
    fmov d12, x9
    mov x9, #32781
    fmov d13, x9
    mov x9, #32782
    fmov d14, x9
    mov x9, #32783
    fmov d15, x9
    brk #0x184
    .seh_startepilogue
    ldp d8, d9, [sp, #96]
    .seh_save_fregp d8, 96
    ldp d10, d11, [sp, #112]
    .seh_save_fregp d10, 112
    ldp d12, d13, [sp, #128]
    .seh_save_fregp d12, 128
    ldp d14, d15, [sp, #144]
    .seh_save_fregp d14, 144
    brk #0x185
    .seh_nop
    ldp x27, x28, [sp, #64]
    .seh_save_regp x27, 64
    ldp x25, x26, [sp, #48]
    .seh_save_regp x25, 48
    ldp x23, x24, [sp, #32]
    .seh_save_regp x23, 32
    ldp x21, x22, [sp, #16]
    .seh_save_regp x21, 16
    ldp x29, x30, [sp, #80]
    .seh_save_fplr 80
    ldp x19, x20, [sp], #160
    .seh_save_r19r20_x 160
    brk #0x186
    .seh_nop
    .seh_endepilogue
    ret
.seh_endproc
.p2align 4
.globl XpUnwindPackedFixture
XpUnwindPackedFixture:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [x0, #160]
    stp x21, x22, [x0, #176]
    stp x23, x24, [x0, #192]
    stp x25, x26, [x0, #208]
    stp x27, x28, [x0, #224]
    stp x29, x30, [x0, #240]
    str x18, [x0, #152]
    mov x9, sp
    str x9, [x0, #256]
    adr x9, XpUnwindPackedReturn
    str x9, [x0, #248]
    str x9, [x0, #264]
    str d8, [x0, #400]
    str d9, [x0, #416]
    str d10, [x0, #432]
    str d11, [x0, #448]
    str d12, [x0, #464]
    str d13, [x0, #480]
    str d14, [x0, #496]
    str d15, [x0, #512]
    bl XpUnwindPackedBody
XpUnwindPackedReturn:
    ldp x29, x30, [sp], #16
    ret
.p2align 4
.globl XpUnwindPackedBody
.seh_proc XpUnwindPackedBody
XpUnwindPackedBody:
    stp x29, x30, [sp, #-32]!
    .seh_save_fplr_x 32
    mov x29, sp
    .seh_set_fp
    .seh_endprologue
    brk #0x187
    .seh_startepilogue
    ldp x29, x30, [sp], #32
    .seh_save_fplr_x 32
    .seh_endepilogue
    ret
.seh_endproc
