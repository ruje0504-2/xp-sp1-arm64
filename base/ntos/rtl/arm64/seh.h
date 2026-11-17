#ifndef XP_ARM64_SEH_H
#define XP_ARM64_SEH_H
#include "unwind.h"
typedef LONG (*PEXCEPTION_FILTER)(PEXCEPTION_POINTERS, PVOID);
typedef VOID (*PTERMINATION_HANDLER)(BOOLEAN, PVOID);
typedef struct _SCOPE_TABLE {
    ULONG Count;
    struct { ULONG BeginAddress, EndAddress, HandlerAddress, JumpTarget; } ScopeRecord[1];
} SCOPE_TABLE, *PSCOPE_TABLE;
/* Port-owned dispatcher-to-language-handler contract. No runtime object from
 * another Windows version is inspected or assumed to have this layout. */
typedef struct _DISPATCHER_CONTEXT {
    ULONG64 ControlPc, ImageBase, EstablisherFrame, TargetIp;
    PCONTEXT ContextRecord;
    PVOID HandlerData, HistoryTable;
    PCONTEXT NonVolatileContext;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;
EXCEPTION_DISPOSITION __C_specific_handler(PEXCEPTION_RECORD, PVOID, PCONTEXT, PDISPATCHER_CONTEXT);
LONG RtlArm64CallFunclet(ULONG_PTR Routine, ULONG_PTR Argument, PCONTEXT Parent);
__declspec(noreturn) VOID RtlArm64ResumeException(PCONTEXT Context);
__declspec(noreturn) VOID RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD Record,
                                    PVOID ReturnValue, PCONTEXT Context, PVOID History);
BOOLEAN RtlArm64DispatchScopeException(const RTL_ARM64_UNWIND_IMAGE *Image,
    PKTRAP_FRAME Frame, ULONG_PTR StackLow, ULONG_PTR StackHigh);
NTSTATUS RtlArm64SehLastStatus(VOID);
#endif
