#include "xp-user-services.h"
.text
.p2align 2
.globl XpUserProgram
.globl XpUserProgramEnd
XpUserProgram:
    sub sp, sp, #32
    mov x19, sp
    mov w20, #0x5678
    str w20, [sp, #16]
    fmov d5, x20
    ldr w9, [x2]
    mov w10, #0x2000000
2:  ldr w11, [x2]
    cmp w9, w11
    b.ne 3f
    subs w10, w10, #1
    b.ne 2b
    brk #0xdf
3:
.globl XpUserReadonlyWrite
XpUserReadonlyWrite:
    str w20, [x2]
    mov x0, sp
    add x1, sp, #8
    mov w8, #XP_SERVICE_QUERY_PERFORMANCE_COUNTER
    svc #0
    cbnz w0, 1f
    ldp x21, x22, [sp]
    cbz x21, 1f
    cbz x22, 1f
    mov x0, #0x1000
    mov x1, xzr
    mov w8, #XP_SERVICE_QUERY_PERFORMANCE_COUNTER
    svc #0
    mov w23, w0
    mov w8, #0xffff
    svc #0
    mov w24, w0
    fmov x25, d5
    ldr w26, [sp, #16]
    add sp, sp, #32
    brk #0x37
1:  brk #0xde
XpUserProgramEnd:
