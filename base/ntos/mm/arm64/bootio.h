#ifndef XP_ARM64_BOOT_IO_H
#define XP_ARM64_BOOT_IO_H
#include "../../../boot/arm64/boot.h"

NTSTATUS MmArm64InitializeBootIo(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS MmArm64MapBootGic(const HAL_ARM64_BOOT_PLATFORM *Platform,
                           HAL_ARM64_BOOT_PLATFORM *Mapped);
VOID MmArm64UnmapBootGic(const HAL_ARM64_BOOT_PLATFORM *Mapped);
NTSTATUS MmArm64TryUnmapIoSpace(PVOID Address, SIZE_T Bytes);
#endif
