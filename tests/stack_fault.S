.text
.p2align 2
.globl XpReadBootStack
XpReadBootStack:
.globl XpBootStackReadFault
XpBootStackReadFault:
    ldr x9, [x0]
    ret
