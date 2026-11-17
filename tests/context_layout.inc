/* The same observations are emitted by the XP overlay and the WDK header. */
#include <stddef.h>
const unsigned XpArm64ContextLayout[] = {
    sizeof(CONTEXT), _Alignof(CONTEXT),
    offsetof(CONTEXT, ContextFlags), offsetof(CONTEXT, Cpsr),
    offsetof(CONTEXT, X0), offsetof(CONTEXT, X18),
    offsetof(CONTEXT, X28), offsetof(CONTEXT, Fp),
    offsetof(CONTEXT, Lr), offsetof(CONTEXT, Sp),
    offsetof(CONTEXT, Pc), offsetof(CONTEXT, V),
    sizeof(((CONTEXT *)0)->V), offsetof(CONTEXT, Fpcr),
    offsetof(CONTEXT, Fpsr), offsetof(CONTEXT, Bcr),
    offsetof(CONTEXT, Bvr), offsetof(CONTEXT, Wcr),
    offsetof(CONTEXT, Wvr), CONTEXT_FULL
};
