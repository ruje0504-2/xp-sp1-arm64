#ifndef XP_ARM64_PLATFORM_H
#define XP_ARM64_PLATFORM_H
#include <ntos.h>
#include "uefi.h"
typedef struct {
    ULONG64 GicDistributor, GicCpuInterface, Mpidr;
    ULONG TimerIrq, TimerFlags, GicVersion, CpuInterfaceNumber;
    ULONG MadtRevision, GtdtRevision;
} HAL_ARM64_BOOT_PLATFORM;
#define HAL_ARM64_VIRTIO_MMIO_LIMIT 32

typedef struct {
    ULONG64 Physical;
    ULONG Bytes, Irq, Trigger;
} HAL_ARM64_VIRTIO_MMIO;

typedef struct {
    ULONG Count;
    HAL_ARM64_VIRTIO_MMIO Devices[HAL_ARM64_VIRTIO_MMIO_LIMIT];
} HAL_ARM64_VIRTIO_BUS;

NTSTATUS BlArm64DiscoverVirtioMmio(XP_EFI_SYSTEM_TABLE *System, const HAL_ARM64_BOOT_PLATFORM *Platform,
                                   HAL_ARM64_VIRTIO_BUS *Bus);
NTSTATUS BlArm64DiscoverAcpiPlatform(XP_EFI_SYSTEM_TABLE *System, HAL_ARM64_BOOT_PLATFORM *Platform);
#endif
