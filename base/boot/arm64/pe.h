#ifndef XP_ARM64_BOOT_PE_H
#define XP_ARM64_BOOT_PE_H
#include "boot.h"

XP_EFI_STATUS BlArm64LoadNativePe(XP_EFI_BOOT_SERVICES_PREFIX *Services,
    const UCHAR *File, ULONG64 FileSize, PXP_ARM64_BOOT_IMAGE Result);
XP_EFI_STATUS BlArm64ReadKernel(PVOID Image, XP_EFI_BOOT_SERVICES_PREFIX *Services,
                                PXP_ARM64_BOOT_IMAGE Kernel, XP_ARM64_BOOT_VOLUME *Volume);
extern WCHAR BlArm64KernelFilePath[];

#endif
