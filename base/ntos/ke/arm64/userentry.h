#ifndef XP_ARM64_USERENTRY_H
#define XP_ARM64_USERENTRY_H
#include "../../mm/arm64/userspace.h"

/* Kernel-owned state for one native user execution. The transition record
 * must be resident in TTBR1. Frame.Cpsr accepts only user condition flags. */
typedef struct DECLSPEC_ALIGN(16) {
    KTRAP_FRAME Frame;
    ULONG64 KernelSp;
    ULONG64 KernelRoot;
    ULONG64 UserThreadPointer;
    ULONG64 VectorKind;
    ULONG_PTR ApcDispatcher;
    ULONG_PTR ExceptionDispatcher;
    ULONG_PTR CallbackDispatcher;
    ULONG CallbackDepth;
    BOOLEAN ExitRequested;
    NTSTATUS ExitStatus;
    MM_ARM64_USER_SPACE *Space;
    NTSTATUS ApcStatus;
} KARM64_USER_ENTRY;

NTSTATUS KiArm64RunUser(MM_ARM64_USER_SPACE *Space, KARM64_USER_ENTRY *Entry);
NTSTATUS KiArm64RunUserThread(KARM64_USER_ENTRY *Entry);
NTSTATUS KiArm64DeliverPendingUserApc(KARM64_USER_ENTRY *Entry);
NTSTATUS KiArm64UserService(MM_ARM64_USER_SPACE *Space, KARM64_USER_ENTRY *Entry);
NTSTATUS KiArm64DispatchUserException(MM_ARM64_USER_SPACE *Space, KARM64_USER_ENTRY *Entry, ULONG_PTR Dispatcher);
/* Eight register arguments followed by up to three eight-byte stack slots.
 * The native descriptor's Number entries specify arguments, not bytes. */
typedef NTSTATUS (*PKARM64_WIN32_SERVICE)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                         ULONG_PTR, ULONG_PTR, ULONG_PTR);
#endif
