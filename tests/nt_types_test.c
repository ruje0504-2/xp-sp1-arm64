#include <excpt.h>
#include <ntdef.h>
#include <stddef.h>
#include "../base/ntos/ex/arm64/interlocked.h"
#include <ntarm64.h>
#include <ntxcapi.h>
_Static_assert(sizeof(LONG) == 4, "NT LONG");
_Static_assert(sizeof(ULONG) == 4, "NT ULONG");
_Static_assert(sizeof(WCHAR) == 2, "NT WCHAR");
_Static_assert(sizeof(NTSTATUS) == 4, "NTSTATUS");
_Static_assert(sizeof(ULONG_PTR) == 8, "NT ULONG_PTR");
_Static_assert(sizeof(POINTER_64_INT) == 8, "Explicit pointer integer");
_Static_assert(sizeof(SIZE_T) == 8, "NT SIZE_T");
_Static_assert(sizeof(LARGE_INTEGER) == 8, "LARGE_INTEGER");
_Static_assert(sizeof(UNICODE_STRING) == 16, "UNICODE_STRING");
_Static_assert(offsetof(UNICODE_STRING, Buffer) == 8, "Unicode pointer");
_Static_assert(sizeof(LIST_ENTRY) == 16, "LIST_ENTRY");
_Static_assert(MEMORY_ALLOCATION_ALIGNMENT == 16, "Pool alignment");
ULONG_PTR XpArm64TypeProbe(PVOID pointer) { return (ULONG_PTR)pointer; }

_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "NT64 exception record");
_Static_assert(offsetof(EXCEPTION_RECORD, ExceptionInformation) == 32, "Exception arguments");
LONG XpArm64CallProbe(PLONG target, PCONTEXT context) {
    RtlCaptureContext(context);
    return InterlockedIncrement(target);
}
#include "context_layout.inc"
