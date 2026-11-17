#ifndef XP_ARM64_BOOT_IO_PRIVATE_H
#define XP_ARM64_BOOT_IO_PRIVATE_H
#include "bootio.h"
#include "bootsyspte.h"
#include "xp-io-tracker.h"

extern ULONG MmTrackPtes;
extern SYSPTES_HEADER MiPteHeader;
extern KSPIN_LOCK MiPteTrackerLock;
extern MI_PFN_CACHE_ATTRIBUTE MiPlatformCacheAttributes[2 * MmMaximumCacheType];
VOID MiInitializeIoTrackers(VOID);
VOID MiInitializeCacheOverrides(VOID);
VOID MiInsertPteTracker(PPTE_TRACKER Tracker, PMDL Mdl, PFN_NUMBER Pages,
                       PVOID Caller, PVOID Parent);
VOID MiArm64RemoveIoPteTracker(PVOID PteAddress, PFN_NUMBER Pages);
PVOID MmArm64IoPte(PVOID Address);
PVOID MmArm64IoVa(PVOID PteAddress);
NTSTATUS HalArm64QueryBootPlatformInformation(HAL_QUERY_INFORMATION_CLASS Class, ULONG Length,
                                               PVOID Output, PULONG Returned);
#endif
