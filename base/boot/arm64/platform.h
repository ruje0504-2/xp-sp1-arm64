#ifndef XP_ARM64_PLATFORM_H
#define XP_ARM64_PLATFORM_H
#include <ntos.h>
#include "uefi.h"
typedef struct {
    ULONG64 GicDistributor, GicCpuInterface, Mpidr;
    ULONG TimerIrq, TimerFlags, GicVersion, CpuInterfaceNumber;
    ULONG MadtRevision, GtdtRevision;
} HAL_ARM64_BOOT_PLATFORM;
NTSTATUS BlArm64DiscoverAcpiPlatform(XP_EFI_SYSTEM_TABLE *System, HAL_ARM64_BOOT_PLATFORM *Platform);
#endif
