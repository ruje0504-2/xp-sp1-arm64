/* Single-processor early identity-root transition, ARMv8.0 EL1.
 * x0=new TTBR0, x1=new TCR, x2=new TTBR1. Return the SCTLR sampled with M clear.
 * C validates this entire aligned code range as identity mapped. Interrupts
 * must already be masked, page tables cleaned, all other PEs quiescent.
 * No stack, literal pool, or data memory access occurs while M is clear.
 *
 * Architecture rationale: ARM address translation / break-before-make.
 * EDK2 stable202408 ArmMmuLibReplaceEntry.S also uses an identity MMU-off
 * interval where a live block-size change could cover the executing code.
 * This implementation changes roots only; no descriptor store occurs here.
 */
.text
.p2align 7
.globl MmArm64SwitchBootRoot
MmArm64SwitchBootRoot:
    mrs x5, sctlr_el1
    bic x3, x5, #1
    dsb sy
    msr sctlr_el1, x3
    isb
    mrs x4, sctlr_el1
    msr ttbr0_el1, x0
    msr ttbr1_el1, x2
    msr tcr_el1, x1
    isb
    tlbi vmalle1is
    dsb ish
    isb
    msr sctlr_el1, x5
    isb
    mov x0, x4
    ret
.globl MmArm64SwitchBootRootEnd
MmArm64SwitchBootRootEnd:
.if (MmArm64SwitchBootRootEnd - MmArm64SwitchBootRoot) > 128
.error "MMU transition crossed its aligned code interval"
.endif
