/* ARM64 bring-up subset. Windows LONG remains 32 bits. */
#ifndef XP_ARM64_INTERLOCKED_H
#define XP_ARM64_INTERLOCKED_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <excpt.h>
#include <ntdef.h>
#else
/* Fixed-width NT types for the macOS execution harness. */
typedef int32_t LONG;
typedef int64_t LONGLONG;
#endif
_Static_assert(sizeof(void *) == 8, "ARM64 pointer width");
_Static_assert(sizeof(int32_t) == 4, "Windows LONG width");
/* Naturally aligned, normal cacheable memory. Every operation is a full
 * barrier. IRQ masking and caller-supplied spin locks belong to the separate
 * ExInterlocked* contract and are not implemented by these entry points. */
LONG InterlockedIncrement(volatile LONG *target);
LONG InterlockedDecrement(volatile LONG *target);
LONG InterlockedExchange(volatile LONG *target, LONG value);
LONG InterlockedExchangeAdd(volatile LONG *target, LONG value);
LONG InterlockedCompareExchange(volatile LONG *target, LONG value, LONG comparand);
LONGLONG InterlockedIncrement64(volatile LONGLONG *target);
LONGLONG InterlockedDecrement64(volatile LONGLONG *target);
LONGLONG InterlockedExchange64(volatile LONGLONG *target, LONGLONG value);
LONGLONG InterlockedExchangeAdd64(volatile LONGLONG *target, LONGLONG value);
LONGLONG InterlockedCompareExchange64(volatile LONGLONG *target, LONGLONG value, LONGLONG comparand);
void *InterlockedExchangePointer(void *volatile *target, void *value);
void *InterlockedCompareExchangePointer(void *volatile *target, void *value, void *comparand);
#endif
