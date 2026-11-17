#include <nt.h>
#include <ntrtl.h>
#include <stddef.h>
_Static_assert(sizeof(CONTEXT) == 912, "ARM64 context");
_Static_assert(sizeof(RUNTIME_FUNCTION) == 8, "ARM64 pdata entry");
_Static_assert(sizeof(NTSTATUS) == 4, "NTSTATUS");
PTEB XpArm64TebProbe(void) { return NtCurrentTeb(); }

_Static_assert(IMAGE_FILE_MACHINE_ARM64 == 0xaa64, "ARM64 PE identifier");
_Static_assert(sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) == 8, "ARM64 image pdata");
_Static_assert(sizeof(IMAGE_NT_HEADERS) == 264, "PE32+ NT headers");
