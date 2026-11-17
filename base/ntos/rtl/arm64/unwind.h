#ifndef XP_ARM64_UNWIND_H
#define XP_ARM64_UNWIND_H
#include <ntos.h>
typedef struct _RTL_ARM64_UNWIND_IMAGE {
    ULONG_PTR Base;
    ULONG Size;
    PRUNTIME_FUNCTION Functions;
    ULONG Count;
} RTL_ARM64_UNWIND_IMAGE;
typedef struct _RTL_ARM64_UNWIND_RESULT {
    ULONG64 EstablisherFrame;
    ULONG64 ControlPc, FunctionBegin, FunctionEnd, FrameSp;
    PEXCEPTION_ROUTINE Handler;
    PVOID HandlerData;
    KNONVOLATILE_CONTEXT_POINTERS Pointers;
} RTL_ARM64_UNWIND_RESULT;
/* Image/stack ranges must be resident and supplied by their actual owner.
 * This API does not probe arbitrary kernel/user pointers or register modules. */
NTSTATUS RtlArm64InitializeUnwindImage(PVOID Base, ULONG Size, RTL_ARM64_UNWIND_IMAGE *Image);
NTSTATUS RtlArm64UnwindOne(const RTL_ARM64_UNWIND_IMAGE *Image, PCONTEXT Context,
    ULONG_PTR StackLow, ULONG_PTR StackHigh, RTL_ARM64_UNWIND_RESULT *Result);
#endif
