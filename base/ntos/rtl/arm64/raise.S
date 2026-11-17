/* Port-owned EL1 software raise: actual vector entry, caller's status in w0.
 * Only this exact trap PC is accepted. Unsupported/unhandled raises fail closed.
 * This leaf still has explicit unwind metadata; no assumed leaf fallback. */
.text
.p2align 2
.globl ExRaiseStatus
.seh_proc ExRaiseStatus
ExRaiseStatus:
    .seh_endprologue
.globl RtlArm64RaiseStatusTrap
RtlArm64RaiseStatusTrap:
    brk #0x7f0
    brk #0x7fe
1:  wfe
    b 1b
.seh_endproc
