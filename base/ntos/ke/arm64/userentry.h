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
} KARM64_USER_ENTRY;

NTSTATUS KiArm64RunUser(MM_ARM64_USER_SPACE *Space, KARM64_USER_ENTRY *Entry);
NTSTATUS KiArm64UserService(MM_ARM64_USER_SPACE *Space, KARM64_USER_ENTRY *Entry);
#endif
