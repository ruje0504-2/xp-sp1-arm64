#ifndef XP_ARM64_BOOT_PROCESS_H
#define XP_ARM64_BOOT_PROCESS_H
#include <ntos.h>
#include "../../mm/arm64/userspace.h"

extern PEPROCESS PsIdleProcess;
extern HANDLE PspInitialSystemProcessHandle;
extern PHANDLE_TABLE PspCidTable;
extern PACCESS_TOKEN PspBootAccessToken;
typedef struct {
    ULONG_PTR Pc, StackBase, StackLimit, Teb, Argument;
} XP_ARM64_USER_START;
NTSTATUS PsArm64CreateUserThread(HANDLE ProcessHandle, const XP_ARM64_USER_START *Start,
                                 PHANDLE ThreadHandle, PCLIENT_ID ClientId);
NTSTATUS PsArm64InitializeProcessImage(PEPROCESS Process, const VOID *File, SIZE_T Size,
                                       ULONG_PTR PebAddress, PULONG_PTR EntryPoint);
NTSTATUS PsArm64SetUserExceptionDispatcher(PEPROCESS Process, ULONG_PTR Address);
ULONG_PTR PsArm64GetUserExceptionDispatcher(PEPROCESS Process);
NTSTATUS PsArm64SetUserApcDispatcher(PEPROCESS Process, ULONG_PTR Address);
ULONG_PTR PsArm64GetUserApcDispatcher(PEPROCESS Process);
NTSTATUS PsArm64SetUserCallbackDispatcher(PEPROCESS Process, ULONG_PTR Address);
ULONG_PTR PsArm64GetUserCallbackDispatcher(PEPROCESS Process);
NTSTATUS PsArm64EnableWin32Services(PEPROCESS Process);
BOOLEAN PsArm64Win32ServicesEnabled(PEPROCESS Process);
VOID PsArm64ReleaseUserTeb(PETHREAD Thread);
NTSTATUS PsArm64CreateUserProcess(HANDLE Parent, PHANDLE Process);
MM_ARM64_USER_SPACE *PsArm64UserProcessSpace(PEPROCESS Process);
VOID PsArm64ReleaseUserProcessSpace(PEPROCESS Process);
BOOLEAN PsArm64InitializePhase0Objects(VOID);
VOID PspArm64InitializeNotifications(VOID);
VOID PspArm64NotifyProcess(PEPROCESS Process, BOOLEAN Create);
VOID PspArm64NotifyThread(PETHREAD Thread, BOOLEAN Create);
NTSTATUS PsArm64InitializeSystemProcess(VOID);
NTSTATUS PspInitializeProcessSecurity(PEPROCESS Parent, PEPROCESS Child);
NTSTATUS PsLookupProcessThreadByCid(PCLIENT_ID Cid, PEPROCESS *Process, PETHREAD *Thread);

#endif
