/* Execute in TTBR1 throughout. Keep SP and all caller memory untouched
 * until TTBR0 and its translation context have been restored. */
.text
.p2align 2
.globl XpProbeUserSpace
XpProbeUserSpace:
    mrs x5, daif
    msr daifset, #15
    mrs x6, ttbr0_el1
    mrs x7, par_el1
    dsb ishst
    msr ttbr0_el1, x0
    isb
    tlbi vmalle1is
    dsb ish
    isb
    at s1e0r, x1
    isb
    mrs x8, par_el1
    at s1e0w, x1
    isb
    mrs x9, par_el1
    mov w10, wzr
    tbnz x8, #0, 1f
    ldtr w10, [x1]
    cbz w2, 1f
    tbnz x9, #0, 1f
    sttr w2, [x1]
1:
    dsb ish
    msr ttbr0_el1, x6
    isb
    tlbi vmalle1is
    dsb ish
    isb
    msr par_el1, x7
    stp x8, x9, [x3]
    str w10, [x3, #16]
    msr daif, x5
    isb
    ret
