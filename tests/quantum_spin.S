/* No calls or waits while the register patterns are live. Return 1 on
 * external progress, or 0 on a clobber or expiration of the real deadline. */
.text
.p2align 2
.globl XpSpinQuantum
XpSpinQuantum:
    sub sp, sp, #32
    str x19, [sp]
    str d8, [sp, #8]
    mrs x7, fpcr
    mrs x8, fpsr
    stp w7, w8, [sp, #16]
    lsl x5, x4, #22
    msr fpcr, x5
    msr fpsr, x4
    mov x19, x4
    lsl x9, x4, #32
    dup v0.2d, x9
    dup v8.2d, x4
    mov x6, sp
    mov w10, #0
1:
    cmp x19, x4
    b.ne 3f
    mov x7, sp
    cmp x7, x6
    b.ne 3f
    umov x7, v0.d[0]
    umov x8, v0.d[1]
    cmp x7, x9
    ccmp x8, x9, #0, eq
    b.ne 3f
    umov x7, v8.d[0]
    umov x8, v8.d[1]
    cmp x7, x4
    ccmp x8, x4, #0, eq
    b.ne 3f
    mrs x7, fpcr
    mrs x8, fpsr
    cmp x7, x5
    ccmp x8, x4, #0, eq
    b.ne 3f
    add w10, w10, #1
    str w10, [x0]
    ldar w7, [x1]
    cmp w7, w2
    b.hs 2f
    isb
    mrs x7, cntpct_el0
    cmp x7, x3
    b.lo 1b
3:
    mov w0, #0
    b 4f
2:
    mov w0, #1
4:
    ldp w7, w8, [sp, #16]
    msr fpcr, x7
    msr fpsr, x8
    ldr d8, [sp, #8]
    ldr x19, [sp]
    add sp, sp, #32
    ret

.p2align 4
.globl XpSpinProfile
.globl XpProfileSampleBegin
.globl XpProfileSampleEnd
XpSpinProfile:
XpProfileSampleBegin:
    isb
    mrs x1, cntpct_el0
    cmp x1, x0
    b.lo XpProfileSampleBegin
XpProfileSampleEnd:
    ret
