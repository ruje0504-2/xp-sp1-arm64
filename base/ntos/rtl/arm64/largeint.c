#include <ntos.h>
/* Full low 64-bit two's-complement product, without signed-C overflow UB. */
LARGE_INTEGER NTAPI RtlExtendedIntegerMultiply(LARGE_INTEGER value, LONG multiplier)
{
    LARGE_INTEGER result;
    result.QuadPart=(LONG64)((ULONG64)value.QuadPart*(ULONG64)(LONG64)multiplier);
    return result;
}
