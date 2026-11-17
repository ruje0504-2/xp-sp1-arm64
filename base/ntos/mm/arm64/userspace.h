#ifndef XP_ARM64_USERSPACE_H
#define XP_ARM64_USERSPACE_H
#include "bootframes.h"

#define MM_ARM64_USER_SHARED_VA 0x7ffe0000UL

/* Resident private TTBR0. All calls require the boot TTBR0, NT_UP and masked
 * DAIF. Zero-initialize once. Its address is the PFN owner until destruction. */
typedef struct {
    PXP_ARM64_BOOT_BLOCK Boot;
    ULONG64 Root;
} MM_ARM64_USER_SPACE;

NTSTATUS MmArm64CreateUserSpace(MM_ARM64_USER_SPACE *Space, PXP_ARM64_BOOT_BLOCK Boot);
VOID MmArm64DestroyUserSpace(MM_ARM64_USER_SPACE *Space);
NTSTATUS MmArm64MapUserPages(MM_ARM64_USER_SPACE *Space, ULONG_PTR Base, SIZE_T Bytes,
                             ULONG Protection);
NTSTATUS MmArm64ProtectUserPages(MM_ARM64_USER_SPACE *Space, ULONG_PTR Base,
                                 SIZE_T Bytes, ULONG Protection);
NTSTATUS MmArm64UnmapUserPages(MM_ARM64_USER_SPACE *Space, ULONG_PTR Base, SIZE_T Bytes);
NTSTATUS MmArm64ProbeUserRead(MM_ARM64_USER_SPACE *Space, ULONG_PTR Address, SIZE_T Bytes);
/* Copy through the PFN direct view, checking the whole range first. */
NTSTATUS MmArm64CopyUserBuffer(MM_ARM64_USER_SPACE *Space, ULONG_PTR Address,
                               PVOID Buffer, SIZE_T Bytes, BOOLEAN ToUser);
BOOLEAN MmArm64UserCodeAddress(MM_ARM64_USER_SPACE *Space, ULONG_PTR Pc);
BOOLEAN MmArm64InitializeSharedUserPte(VOID);
#endif
