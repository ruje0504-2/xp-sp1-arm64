/* A real failing load and a declared continuation; no guessed C register
 * allocation and no empty success implementation. The SEH filter chooses
 * whether to continue by editing its documented exception context. */
.text
.p2align 4
.globl XpSehResumeRead
.seh_proc XpSehResumeRead
XpSehResumeRead:
    sub sp, sp, #16
    .seh_stackalloc 16
    .seh_endprologue
.globl XpSehResumeFault
XpSehResumeFault:
    ldr w0, [x0]
.globl XpSehResumeTarget
XpSehResumeTarget:
    .seh_startepilogue
    add sp, sp, #16
    .seh_stackalloc 16
    .seh_endepilogue
    ret
.seh_endproc
