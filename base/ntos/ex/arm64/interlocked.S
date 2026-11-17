/* ARMv8.0 leaf routines, suitable for Cortex-A57. No LSE required.
 * x0 = target, w1/x1 = value, w2/x2 = comparand.
 * Only volatile registers are changed. No stack or unwind frame is needed.
 * Full barriers apply to successful updates and failed comparisons. */
#ifdef __APPLE__
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif
.text

.p2align 2
.globl SYMBOL(InterlockedIncrement)
SYMBOL(InterlockedIncrement):
    dmb ish
1:
    ldaxr w3, [x0]
    add w5, w3, #1
    stlxr w4, w5, [x0]
    cbnz w4, 1b
    dmb ish
    mov w0, w5
    ret

.p2align 2
.globl SYMBOL(InterlockedDecrement)
SYMBOL(InterlockedDecrement):
    dmb ish
1:
    ldaxr w3, [x0]
    sub w5, w3, #1
    stlxr w4, w5, [x0]
    cbnz w4, 1b
    dmb ish
    mov w0, w5
    ret

.p2align 2
.globl SYMBOL(InterlockedExchange)
SYMBOL(InterlockedExchange):
    dmb ish
1:
    ldaxr w3, [x0]
    stlxr w4, w1, [x0]
    cbnz w4, 1b
    dmb ish
    mov w0, w3
    ret

.p2align 2
.globl SYMBOL(InterlockedExchangeAdd)
SYMBOL(InterlockedExchangeAdd):
    dmb ish
1:
    ldaxr w3, [x0]
    add w5, w3, w1
    stlxr w4, w5, [x0]
    cbnz w4, 1b
    dmb ish
    mov w0, w3
    ret

.p2align 2
.globl SYMBOL(InterlockedCompareExchange)
SYMBOL(InterlockedCompareExchange):
    dmb ish
1:
    ldaxr w3, [x0]
    cmp w3, w2
    b.ne 2f
    stlxr w4, w1, [x0]
    cbnz w4, 1b
    b 3f
2:
    clrex
3:
    dmb ish
    mov w0, w3
    ret

.p2align 2
.globl SYMBOL(InterlockedIncrement64)
SYMBOL(InterlockedIncrement64):
    dmb ish
1:
    ldaxr x3, [x0]
    add x5, x3, #1
    stlxr w4, x5, [x0]
    cbnz w4, 1b
    dmb ish
    mov x0, x5
    ret

.p2align 2
.globl SYMBOL(InterlockedDecrement64)
SYMBOL(InterlockedDecrement64):
    dmb ish
1:
    ldaxr x3, [x0]
    sub x5, x3, #1
    stlxr w4, x5, [x0]
    cbnz w4, 1b
    dmb ish
    mov x0, x5
    ret

.p2align 2
.globl SYMBOL(InterlockedExchange64)
SYMBOL(InterlockedExchange64):
    dmb ish
1:
    ldaxr x3, [x0]
    stlxr w4, x1, [x0]
    cbnz w4, 1b
    dmb ish
    mov x0, x3
    ret

.p2align 2
.globl SYMBOL(InterlockedExchangeAdd64)
SYMBOL(InterlockedExchangeAdd64):
    dmb ish
1:
    ldaxr x3, [x0]
    add x5, x3, x1
    stlxr w4, x5, [x0]
    cbnz w4, 1b
    dmb ish
    mov x0, x3
    ret

.p2align 2
.globl SYMBOL(InterlockedCompareExchange64)
SYMBOL(InterlockedCompareExchange64):
    dmb ish
1:
    ldaxr x3, [x0]
    cmp x3, x2
    b.ne 2f
    stlxr w4, x1, [x0]
    cbnz w4, 1b
    b 3f
2:
    clrex
3:
    dmb ish
    mov x0, x3
    ret

.p2align 2
.globl SYMBOL(InterlockedExchangePointer)
SYMBOL(InterlockedExchangePointer):
    b SYMBOL(InterlockedExchange64)

.p2align 2
.globl SYMBOL(InterlockedCompareExchangePointer)
SYMBOL(InterlockedCompareExchangePointer):
    b SYMBOL(InterlockedCompareExchange64)
