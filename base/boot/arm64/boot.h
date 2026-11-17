/* Early port-owned wrapper around the actual XP NT loader definitions.
 * No compatibility with private Windows ARM64 loader layouts is assumed. */
#ifndef XP_ARM64_BOOT_H
#define XP_ARM64_BOOT_H
#include <ntos.h>
#include "uefi.h"
#include "platform.h"
#include "time.h"
#include "bootimage.h"
#include "../../ntos/mm/arm64/bootmap.h"
#define XP_ARM64_BOOT_VERSION 15
#define XP_ARM64_NATIVE_LOAD_OPTIONS "NOGUIBOOT"
#define XP_ARM64_BOOT_MAP_CAPACITY 65536
#define XP_ARM64_BOOT_DESCRIPTOR_CAPACITY 1024
#define XP_ARM64_DEVICE_PATH_CAPACITY 256
#define XP_ARM64_ARC_NAME_CAPACITY (2 * XP_ARM64_DEVICE_PATH_CAPACITY + sizeof("uefi()"))
#define XP_ARM64_BOOT_DIRECTORY_CAPACITY 128
typedef struct _XP_ARM64_BOOT_VOLUME {
    ULONG DevicePathBytes;
    UCHAR DevicePath[XP_ARM64_DEVICE_PATH_CAPACITY];
    CHAR ArcName[XP_ARM64_ARC_NAME_CAPACITY];
    CHAR NtDirectory[XP_ARM64_BOOT_DIRECTORY_CAPACITY];
} XP_ARM64_BOOT_VOLUME;
#define XP_DISPLAY_RGBX 0u
#define XP_DISPLAY_BGRX 1u
typedef struct {
    ULONG64 PhysicalBase, Bytes;
    ULONG Width, Height, Stride, Format;
    ULONG_PTR VirtualBase;
} XP_ARM64_BOOT_DISPLAY;

typedef struct _XP_ARM64_BOOT_BLOCK {
    LOADER_PARAMETER_BLOCK Nt;
    ULONG Size, Version;
    BOOLEAN ExitAttempted, BootServicesExited;
    ULONG ExitAttempts;
    NTSTATUS InitializationStatus;
    NTSTATUS PlatformStatus;
    NTSTATUS HalStatus;
    HAL_ARM64_BOOT_PLATFORM Platform;
    XP_ARM64_BOOT_TIME Time;
    XP_ARM64_BOOT_DISPLAY Display;
    ULONG64 StackBase;
    ULONG64 StackAllocationBase;
    MM_ARM64_BOOT_TABLES Tables;
    ULONG_PTR StackSize;
    ULONG_PTR MapSize, MapKey, DescriptorSize;
    ULONG DescriptorVersion, DescriptorCount;
    ULONG64 FreePages, ReservedPages;
    PFN_NUMBER SharedPagePfn;
    ULONG BootPfnPages;
    ULONG BootPfnMetadataPages;
    ULONG_PTR FirmwareStack;
    XP_ARM64_BOOT_IMAGE KernelImage;
    KLDR_DATA_TABLE_ENTRY KernelModule;
    XP_ARM64_BOOT_VOLUME Volume;
    KPCR Pcr;
    /* Original wait-priority code accesses EPROCESS.Vm through Pcb. Reserve
     * the real enclosing object; a standalone KPROCESS is insufficient. */
    EPROCESS InitialProcess;
    KNODE InitialNode;
    KSPIN_LOCK SharedLocks[16];
    DECLSPEC_ALIGN(8) UCHAR Map[XP_ARM64_BOOT_MAP_CAPACITY];
    MEMORY_ALLOCATION_DESCRIPTOR Descriptors[XP_ARM64_BOOT_DESCRIPTOR_CAPACITY];
} XP_ARM64_BOOT_BLOCK, *PXP_ARM64_BOOT_BLOCK;
VOID BlArm64CaptureDisplay(XP_EFI_BOOT_SERVICES_PREFIX *Services, XP_ARM64_BOOT_DISPLAY *Display);
BOOLEAN BlArm64DisplayReserved(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS BlArm64ConvertMemoryMap(PXP_ARM64_BOOT_BLOCK Block);
ULONG BlArm64EncodeArcDevicePath(const UCHAR *Path, ULONG Available, PCHAR ArcName);
NTSTATUS MmArm64ReserveBootPages(PXP_ARM64_BOOT_BLOCK Block, ULONG Count,
                                  PPFN_NUMBER FirstPage);
/* Block must be zero initialized except the RTC policy. Never reuse it after
 * an ExitBootServices attempt. A pre-exit allocation failure may be retried. */
XP_EFI_STATUS BlArm64Boot(PVOID Image, XP_EFI_SYSTEM_TABLE *System,
                          PXP_ARM64_BOOT_BLOCK Block);
__declspec(noreturn) VOID KiArm64EnterBoot(PXP_ARM64_BOOT_BLOCK Block, ULONG_PTR StackTop);
NTSTATUS KiArm64InitializeBootStructures(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS KiArm64InitializeBootIdle(PXP_ARM64_BOOT_BLOCK Block);
NTSTATUS KiArm64StartPhase1(PXP_ARM64_BOOT_BLOCK Block, PHANDLE ThreadHandle);
BOOLEAN KiArm64DispatchBootTrap(PKTRAP_FRAME Frame, ULONG VectorType);
BOOLEAN KiArm64BootTrap(PKTRAP_FRAME Frame, ULONG VectorType);
/* Explicit unfinished boundary: current test supplies a verification
 * continuation, never a substitute KiInitializeKernel or scheduler. */
__declspec(noreturn) VOID KiArm64BootContinuation(PXP_ARM64_BOOT_BLOCK Block, NTSTATUS Status);
#endif
