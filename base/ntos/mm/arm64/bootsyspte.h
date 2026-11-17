#ifndef XP_ARM64_SYSTEM_PTE_H
#define XP_ARM64_SYSTEM_PTE_H
#include "bootframes_private.h"
#include "xp-syspte-types.h"

extern PMMPTE MmSystemPteBase;
extern PMMPTE MmSystemPtesStart[MaximumPtePoolTypes], MmSystemPtesEnd[MaximumPtePoolTypes];
extern ULONG MmTotalSystemPtes;
extern ULONG MmTotalFreeSystemPtes[MaximumPtePoolTypes];
extern ULONG MmPteFailures[MaximumPtePoolTypes];

VOID MiInitializeSystemPtes(PMMPTE Start, ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool);
PMMPTE MiReserveSystemPtes(ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool);
PMMPTE MiReserveAlignedSystemPtes(ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool, ULONG Alignment);
VOID MiReleaseSystemPtes(PMMPTE Start, ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool);
ULONG MiCountFreeSystemPtes(MMSYSTEM_PTE_POOL_TYPE Pool);
NTSTATUS MmArm64TryReleaseSystemPtes(PMMPTE Start, ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool);
PVOID MmArm64SystemPteVa(PMMPTE Pte);
VOID MmArm64SyncSystemPtes(PMMPTE Start, ULONG Pages);

/* Selected XP free-cluster operations. The ARM64 wrapper owns the lock,
 * checks each lease and handles hardware translation maintenance. */
VOID MiArm64InitializeSystemPteList(PMMPTE Start, ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool);
PMMPTE MiArm64FindSystemPtes(ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool, ULONG Alignment);
VOID MiArm64CoalesceSystemPtes(PMMPTE Start, ULONG Pages, MMSYSTEM_PTE_POOL_TYPE Pool);
#endif
