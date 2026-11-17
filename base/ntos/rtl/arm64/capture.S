/* ARMv8.0 EL0 continuation capture. x0 points to 16-byte aligned CONTEXT.
 * Save every incoming register before using temporary registers.
 * x18 is read and preserved. No privileged status/debug registers accessed. */
#ifdef __APPLE__
#define SYMBOL(n) _##n
#else
#define SYMBOL(n) n
#endif
.text
.p2align 2
.globl SYMBOL(RtlCaptureContext)
SYMBOL(RtlCaptureContext):
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
    mov x1, sp
    stp x1, x30, [x0, #256]
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
    mrs x1, nzcv
    str w1, [x0, #4]
    mrs x1, fpcr
    mrs x2, fpsr
    str w1, [x0, #784]
    str w2, [x0, #788]
    mov w1, #7
    movk w1, #0x40, lsl #16
    str w1, [x0]
    add x1, x0, #792
    stp xzr, xzr, [x1, #0]
    stp xzr, xzr, [x1, #16]
    stp xzr, xzr, [x1, #32]
    stp xzr, xzr, [x1, #48]
    stp xzr, xzr, [x1, #64]
    stp xzr, xzr, [x1, #80]
    stp xzr, xzr, [x1, #96]
    str xzr, [x1, #112]
    ret
