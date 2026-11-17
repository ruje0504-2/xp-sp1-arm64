#ifndef XP_MM_ARM64_BOOT_IMAGE_H
#define XP_MM_ARM64_BOOT_IMAGE_H
#include "../../../boot/arm64/boot.h"

NTSTATUS MmArm64InitializeBootImage(PXP_ARM64_BOOT_BLOCK Block);
PKLDR_DATA_TABLE_ENTRY MiLookupDataTableEntry(PVOID AddressWithinSection, ULONG ResourceHeld);
extern ERESOURCE PsLoadedModuleResource;
extern FAST_MUTEX MmDynamicMemoryMutex;

#endif
