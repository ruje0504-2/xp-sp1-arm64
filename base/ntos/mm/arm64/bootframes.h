#ifndef XP_ARM64_BOOT_FRAMES_H
#define XP_ARM64_BOOT_FRAMES_H
#include "../../../boot/arm64/boot.h"

typedef struct {
    PFN_NUMBER Present, Free, Allocated, Inaccessible;
} MM_ARM64_BOOT_PFN_COUNTS;

NTSTATUS MmArm64PrepareBootFrames(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS MmArm64InitializeBootFrames(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS MmArm64PromotePhysicalMemoryDescriptor(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS MmArm64TakeBootFrame(PVOID Owner, PPFN_NUMBER Frame);
VOID MmArm64RecordBootMapping(PVOID Owner, PFN_NUMBER Frame, volatile ULONG64 *Pte,
                              BOOLEAN Stack);
NTSTATUS MmArm64ReturnBootFrame(PVOID Owner, PFN_NUMBER Frame);
BOOLEAN MmArm64BootFrameOwnedBy(PFN_NUMBER Frame, PVOID Owner);
MM_ARM64_BOOT_PFN_COUNTS MmArm64BootFrameCounts(VOID);

#endif
