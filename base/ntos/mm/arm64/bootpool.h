#ifndef XP_ARM64_BOOTPOOL_H
#define XP_ARM64_BOOTPOOL_H
#include "bootmap.h"
#define MM_ARM64_KERNEL_DEMAND_ZERO_PTE (1ULL << 2)

NTSTATUS MmArm64InitializeNonPagedPool(PMM_ARM64_BOOT_TABLES Tables);
BOOLEAN MmArm64IsBootNonPagedPoolAddress(PVOID Address);
BOOLEAN MmArm64IsBootPagedPoolAddress(PVOID Address);
NTSTATUS MmArm64PinBootPoolPage(ULONG_PTR Address, BOOLEAN Write, PPFN_NUMBER Frame);
ULONG64 MmArm64BootPoolDescriptor(PFN_NUMBER Frame);
NTSTATUS MmArm64InitializePagedPool(PMM_ARM64_BOOT_TABLES Tables);
BOOLEAN MmArm64ResolvePagedPoolFault(PKTRAP_FRAME Frame);
/* PFN allocator and paged pool share this reservation under the UP DAIF lock. */
extern PFN_COUNT MiArm64BootDemandZeroPages;
#endif
