/* Execute entirely in TTBR1, with all exceptions masked. Suspend TTBR0
 * walks and retire global as well as ASID translations on both transitions.
 * No stack or low data access until the bootstrap root is restored. */
.macro change_root root
    msr tcr_el1, x6
    isb
    dsb ishst
    tlbi vmalle1is
    dsb ish
    isb
    msr ttbr0_el1, \root
    isb
    msr tcr_el1, x5
    isb
.endm

.text
.p2align 2
.globl XpQueryUserTranslation
XpQueryUserTranslation:
    mrs x4, ttbr0_el1
    mrs x5, tcr_el1
    orr x6, x5, #0x80
    mrs x7, par_el1
    change_root x0
    cbnz w2, 1f
    at s1e0r, x1
    b 2f
1:  at s1e0w, x1
2:  isb
    mrs x0, par_el1
    msr par_el1, x7
    change_root x4
    ret
