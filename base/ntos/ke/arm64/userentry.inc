/* Lower-EL entry uses SP_EL1 as a TTBR1 transition record. No accesses
 * to the interrupted kernel stack or PCR until kernel TTBR0 is active. */
.macro user_change_root root
    mrs x11, tcr_el1
    orr x12, x11, #0x80
    msr tcr_el1, x12
    isb
    dsb ishst
    tlbi vmalle1is
    dsb ish
    isb
    msr ttbr0_el1, \root
    isb
    msr tcr_el1, x11
    isb
.endm

.globl KiArm64EnterUserRaw
KiArm64EnterUserRaw:
    sub sp, sp, #256
    stp x18, x19, [sp, #0]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    mrs x9, cpacr_el1
    str x9, [sp, #104]
    orr x10, x9, #(3 << 20)
    msr cpacr_el1, x10
    isb
    mrs x9, sp_el0
    mrs x10, tpidr_el0
    stp x9, x10, [sp, #112]
    mrs x9, tpidrro_el0
    mrs x10, fpcr
    stp x9, x10, [sp, #128]
    mrs x9, fpsr
    str x9, [sp, #144]
    stp d8, d9, [sp, #160]
    stp d10, d11, [sp, #176]
    stp d12, d13, [sp, #192]
    stp d14, d15, [sp, #208]
    mov x9, sp
    mrs x10, ttbr0_el1
    str x9, [x1, #TF_SIZE]
    str x10, [x1, #(TF_SIZE+8)]
    mov sp, x1
    user_change_root x0
    ldr x9, [sp, #TF_SP]
    msr sp_el0, x9
    ldr x9, [sp, #TF_PC]
    msr elr_el1, x9
    ldr x9, [sp, #TF_CPSR]
    msr spsr_el1, x9
    ldr x9, [sp, #(TF_SIZE+16)]
    msr tpidr_el0, x9
    msr tpidrro_el0, xzr
    ldr w9, [sp, #TF_FPCR]
    msr fpcr, x9
    ldr w9, [sp, #TF_FPSR]
    msr fpsr, x9
    ldp q0, q1, [sp, #(TF_V+0)]
    ldp q2, q3, [sp, #(TF_V+32)]
    ldp q4, q5, [sp, #(TF_V+64)]
    ldp q6, q7, [sp, #(TF_V+96)]
    ldp q8, q9, [sp, #(TF_V+128)]
    ldp q10, q11, [sp, #(TF_V+160)]
    ldp q12, q13, [sp, #(TF_V+192)]
    ldp q14, q15, [sp, #(TF_V+224)]
    ldp q16, q17, [sp, #(TF_V+256)]
    ldp q18, q19, [sp, #(TF_V+288)]
    ldp q20, q21, [sp, #(TF_V+320)]
    ldp q22, q23, [sp, #(TF_V+352)]
    ldp q24, q25, [sp, #(TF_V+384)]
    ldp q26, q27, [sp, #(TF_V+416)]
    ldp q28, q29, [sp, #(TF_V+448)]
    ldp q30, q31, [sp, #(TF_V+480)]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    eret

KiArm64UserSyncEntry:
    stp x0, x1, [sp]
    mov x0, #0
    b KiArm64LeaveUser
KiArm64UserIrqEntry:
    stp x0, x1, [sp]
    mov x0, #1
    b KiArm64LeaveUser
KiArm64UserFiqEntry:
    stp x0, x1, [sp]
    mov x0, #2
    b KiArm64LeaveUser
KiArm64UserSErrorEntry:
    stp x0, x1, [sp]
    mov x0, #3
    b KiArm64LeaveUser
KiArm64LeaveUser:
    str x0, [sp, #(TF_SIZE+24)]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x9, sp_el0
    str x9, [sp, #(TF_SP)]
    mrs x9, elr_el1
    str x9, [sp, #(TF_PC)]
    mrs x9, spsr_el1
    str x9, [sp, #(TF_CPSR)]
    mrs x9, esr_el1
    str x9, [sp, #(TF_ESR)]
    mrs x9, far_el1
    str x9, [sp, #(TF_FAR)]
    mrs x9, tpidr_el0
    str x9, [sp, #(TF_SIZE+16)]
    stp q0, q1, [sp, #(TF_V+0)]
    stp q2, q3, [sp, #(TF_V+32)]
    stp q4, q5, [sp, #(TF_V+64)]
    stp q6, q7, [sp, #(TF_V+96)]
    stp q8, q9, [sp, #(TF_V+128)]
    stp q10, q11, [sp, #(TF_V+160)]
    stp q12, q13, [sp, #(TF_V+192)]
    stp q14, q15, [sp, #(TF_V+224)]
    stp q16, q17, [sp, #(TF_V+256)]
    stp q18, q19, [sp, #(TF_V+288)]
    stp q20, q21, [sp, #(TF_V+320)]
    stp q22, q23, [sp, #(TF_V+352)]
    stp q24, q25, [sp, #(TF_V+384)]
    stp q26, q27, [sp, #(TF_V+416)]
    stp q28, q29, [sp, #(TF_V+448)]
    stp q30, q31, [sp, #(TF_V+480)]
    mrs x9, fpcr
    str w9, [sp, #TF_FPCR]
    mrs x9, fpsr
    str w9, [sp, #TF_FPSR]
    ldr x10, [sp, #(TF_SIZE+8)]
    user_change_root x10
    ldr x9, [sp, #TF_SIZE]
    mov sp, x9
    ldp x9, x10, [sp, #112]
    msr sp_el0, x9
    msr tpidr_el0, x10
    ldp x9, x10, [sp, #128]
    msr tpidrro_el0, x9
    msr fpcr, x10
    ldr x9, [sp, #144]
    msr fpsr, x9
    ldp d8, d9, [sp, #160]
    ldp d10, d11, [sp, #176]
    ldp d12, d13, [sp, #192]
    ldp d14, d15, [sp, #208]
    ldr x9, [sp, #104]
    msr cpacr_el1, x9
    isb
    ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #256
    ret
