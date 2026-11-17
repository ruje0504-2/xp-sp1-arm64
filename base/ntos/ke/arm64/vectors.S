/* ARMv8.0 EL1 exception vectors. Generated offsets come from NT kernel types.
 * Before VBAR is installed, SP_EL1, TPIDR_EL1 and x18 must designate valid
 * kernel stack/PCR state. The C dispatch path is general-register-only.
 * The early handler is an explicit boundary to the future NT dispatcher.
 * Unsupported/unhandled exceptions remain stopped with interrupts masked. */
#include "kernel_offsets.inc"

.text
.p2align 11
.globl KiArm64VectorTable
KiArm64VectorTable:
.macro VECTOR target
    b \target
    .space 124
.endm
    VECTOR KiArm64SyncEntry
    VECTOR KiArm64IrqEntry
    VECTOR KiArm64FiqEntry
    VECTOR KiArm64SErrorEntry
    VECTOR KiArm64SyncEntry
    VECTOR KiArm64IrqEntry
    VECTOR KiArm64FiqEntry
    VECTOR KiArm64SErrorEntry
    VECTOR KiArm64SyncEntry
    VECTOR KiArm64IrqEntry
    VECTOR KiArm64FiqEntry
    VECTOR KiArm64SErrorEntry
    VECTOR KiArm64Fatal
    VECTOR KiArm64Fatal
    VECTOR KiArm64Fatal
    VECTOR KiArm64Fatal

/* Save all integer registers before using a scratch register or changing x18.
 * Taking an exception selects SP_EL1. SPSR identifies the interrupted stack. */
.macro SAVE_INTEGER kind
    sub sp, sp, #TF_SIZE
    stp x0, x1, [sp, #0]
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
    mov w1, #\kind
    b KiArm64SaveSystemState
.endm
KiArm64SyncEntry:
    SAVE_INTEGER 0
KiArm64IrqEntry:
    SAVE_INTEGER 1
KiArm64FiqEntry:
    SAVE_INTEGER 2
KiArm64SErrorEntry:
    SAVE_INTEGER 3

KiArm64SaveSystemState:
    mrs x9, spsr_el1
    str x9, [sp, #TF_CPSR]
    add x10, sp, #TF_SIZE
    tbnz x9, #0, 1f
    mrs x10, sp_el0
1:
    str x10, [sp, #TF_SP]
    mrs x10, elr_el1
    str x10, [sp, #TF_PC]
    mrs x10, esr_el1
    str x10, [sp, #TF_ESR]
    mrs x10, far_el1
    str x10, [sp, #TF_FAR]
    mrs x18, tpidr_el1
    cbz x18, KiArm64Fatal
    ldrb w10, [x18, #PCR_IRQL]
    strb w10, [sp, #TF_IRQL]
    and x9, x9, #0xf
    cmp x9, #0
    cset w10, eq
    strb w10, [sp, #TF_MODE]
    mov w10, #1
    strb w10, [sp, #TF_ACTIVE]
    str wzr, [sp, #TF_PADDING]

    /* FPEN bit20 permits EL1 FP/SIMD access. A trapped/disabled FP state
     * remains invalid in this frame and is never read or restored. */
    mrs x9, cpacr_el1
    tbz x9, #20, KiArm64NoFpState
    stp q0, q1, [sp, #TF_V]
    stp q2, q3, [sp, #(TF_V + 32)]
    stp q4, q5, [sp, #(TF_V + 64)]
    stp q6, q7, [sp, #(TF_V + 96)]
    stp q8, q9, [sp, #(TF_V + 128)]
    stp q10, q11, [sp, #(TF_V + 160)]
    stp q12, q13, [sp, #(TF_V + 192)]
    stp q14, q15, [sp, #(TF_V + 224)]
    stp q16, q17, [sp, #(TF_V + 256)]
    stp q18, q19, [sp, #(TF_V + 288)]
    stp q20, q21, [sp, #(TF_V + 320)]
    stp q22, q23, [sp, #(TF_V + 352)]
    stp q24, q25, [sp, #(TF_V + 384)]
    stp q26, q27, [sp, #(TF_V + 416)]
    stp q28, q29, [sp, #(TF_V + 448)]
    stp q30, q31, [sp, #(TF_V + 480)]
    mrs x9, fpcr
    str w9, [sp, #TF_FPCR]
    mrs x9, fpsr
    str w9, [sp, #TF_FPSR]
    mov w9, #1
    strb w9, [sp, #TF_FP_SAVED]
    b KiArm64CallHandler
KiArm64NoFpState:
    strb wzr, [sp, #TF_FP_SAVED]
    str wzr, [sp, #TF_FPCR]
    str wzr, [sp, #TF_FPSR]
    add x9, sp, #TF_V
    mov x10, #32
2:
    stp xzr, xzr, [x9], #16
    subs x10, x10, #1
    b.ne 2b

KiArm64CallHandler:
    mov x0, sp
    bl KiArm64TrapDispatch
    /* A DPC can enable nested higher-priority IRQs. Protect restored system
     * registers and the interrupted SIMD/GPR state until ERET. */
    msr daifset, #15
    cbz w0, KiArm64Fatal
    ldrb w9, [sp, #TF_FP_SAVED]
    cbz w9, KiArm64RestoreInteger
    mrs x9, cpacr_el1
    tbz x9, #20, KiArm64Fatal
    ldr w9, [sp, #TF_FPCR]
    msr fpcr, x9
    ldr w9, [sp, #TF_FPSR]
    msr fpsr, x9
    ldp q0, q1, [sp, #TF_V]
    ldp q2, q3, [sp, #(TF_V + 32)]
    ldp q4, q5, [sp, #(TF_V + 64)]
    ldp q6, q7, [sp, #(TF_V + 96)]
    ldp q8, q9, [sp, #(TF_V + 128)]
    ldp q10, q11, [sp, #(TF_V + 160)]
    ldp q12, q13, [sp, #(TF_V + 192)]
    ldp q14, q15, [sp, #(TF_V + 224)]
    ldp q16, q17, [sp, #(TF_V + 256)]
    ldp q18, q19, [sp, #(TF_V + 288)]
    ldp q20, q21, [sp, #(TF_V + 320)]
    ldp q22, q23, [sp, #(TF_V + 352)]
    ldp q24, q25, [sp, #(TF_V + 384)]
    ldp q26, q27, [sp, #(TF_V + 416)]
    ldp q28, q29, [sp, #(TF_V + 448)]
    ldp q30, q31, [sp, #(TF_V + 480)]

KiArm64RestoreInteger:
    ldr x9, [sp, #TF_CPSR]
    msr spsr_el1, x9
    tbnz x9, #0, 3f
    ldr x10, [sp, #TF_SP]
    msr sp_el0, x10
3:
    ldr x9, [sp, #TF_PC]
    msr elr_el1, x9
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
    add sp, sp, #TF_SIZE
    eret

KiArm64Fatal:
    msr daifset, #15
4:
    wfe
    b 4b
