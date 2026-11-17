/* Minimal public UEFI 2.10 ABI (sections 4, 7.2, 7.4.6).
 * Unused service slots retain their specified positions, not callability. */
#ifndef XP_BOOT_UEFI_H
#define XP_BOOT_UEFI_H
#include <stddef.h>
typedef ULONG_PTR XP_EFI_STATUS;
#define XP_EFI_ERROR(n) (0x8000000000000000ULL | (n))
#define XP_EFI_LOAD_ERROR XP_EFI_ERROR(1)
#define XP_EFI_INVALID_PARAMETER XP_EFI_ERROR(2)
#define XP_EFI_UNSUPPORTED XP_EFI_ERROR(3)
#define XP_EFI_BUFFER_TOO_SMALL XP_EFI_ERROR(5)
#define XP_EFI_OUT_OF_RESOURCES XP_EFI_ERROR(9)
#define XP_EFI_ABORTED XP_EFI_ERROR(21)
#define XP_EFI_LOADER_CODE 1u
#define XP_EFI_LOADER_DATA 2u
#define XP_EFI_MEMORY_RUNTIME (1ULL << 63)
typedef struct {
    ULONG64 Signature;
    ULONG Revision, HeaderSize, Crc32, Reserved;
} XP_EFI_TABLE_HEADER;
typedef struct {
    ULONG Type;
    ULONG64 PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} XP_EFI_MEMORY_DESCRIPTOR;
typedef XP_EFI_STATUS (*XP_EFI_ALLOCATE_PAGES)(ULONG, ULONG, ULONG_PTR, PULONG64);
typedef XP_EFI_STATUS (*XP_EFI_FREE_PAGES)(ULONG64, ULONG_PTR);
typedef XP_EFI_STATUS (*XP_EFI_GET_MEMORY_MAP)(PULONG_PTR, PVOID, PULONG_PTR,
                                             PULONG_PTR, PULONG);
typedef XP_EFI_STATUS (*XP_EFI_EXIT_BOOT_SERVICES)(PVOID, ULONG_PTR);
typedef XP_EFI_STATUS (*XP_EFI_HANDLE_PROTOCOL)(PVOID, const GUID *, PVOID *);
typedef struct {
    USHORT Year;
    UCHAR Month, Day, Hour, Minute, Second, Pad1;
    ULONG Nanosecond;
    SHORT TimeZone;
    UCHAR Daylight, Pad2;
} XP_EFI_TIME;
typedef struct {
    ULONG Resolution, Accuracy;
    BOOLEAN SetsToZero;
} XP_EFI_TIME_CAPABILITIES;
typedef XP_EFI_STATUS (*XP_EFI_GET_TIME)(XP_EFI_TIME *, XP_EFI_TIME_CAPABILITIES *);
typedef struct {
    XP_EFI_TABLE_HEADER Hdr;
    XP_EFI_GET_TIME GetTime;
} XP_EFI_RUNTIME_SERVICES_PREFIX;
typedef struct {
    XP_EFI_TABLE_HEADER Hdr;
    PVOID RaiseTpl, RestoreTpl;
    XP_EFI_ALLOCATE_PAGES AllocatePages;
    XP_EFI_FREE_PAGES FreePages;
    XP_EFI_GET_MEMORY_MAP GetMemoryMap;
    PVOID AllocatePool, FreePool;
    PVOID CreateEvent, SetTimer, WaitForEvent, SignalEvent, CloseEvent, CheckEvent;
    PVOID InstallProtocol, ReinstallProtocol, UninstallProtocol;
    XP_EFI_HANDLE_PROTOCOL HandleProtocol;
    PVOID Reserved, RegisterProtocolNotify, LocateHandle, LocateDevicePath;
    PVOID InstallConfigurationTable;
    PVOID LoadImage, StartImage, Exit, UnloadImage;
    XP_EFI_EXIT_BOOT_SERVICES ExitBootServices;
} XP_EFI_BOOT_SERVICES_PREFIX;
typedef struct {
    XP_EFI_TABLE_HEADER Hdr;
    PWCHAR FirmwareVendor;
    ULONG FirmwareRevision;
    PVOID ConsoleInHandle, ConIn, ConsoleOutHandle, ConOut;
    PVOID StandardErrorHandle, StdErr, RuntimeServices;
    XP_EFI_BOOT_SERVICES_PREFIX *BootServices;
    ULONG_PTR NumberOfTableEntries;
    PVOID ConfigurationTable;
} XP_EFI_SYSTEM_TABLE;
_Static_assert(sizeof(XP_EFI_TABLE_HEADER) == 24, "UEFI header");
_Static_assert(sizeof(XP_EFI_MEMORY_DESCRIPTOR) == 40, "UEFI memory prefix");
_Static_assert(sizeof(XP_EFI_TIME) == 16 && offsetof(XP_EFI_TIME, TimeZone) == 12, "UEFI time");
_Static_assert(sizeof(XP_EFI_TIME_CAPABILITIES) == 12, "UEFI clock capabilities");
_Static_assert(offsetof(XP_EFI_RUNTIME_SERVICES_PREFIX, GetTime) == 24, "UEFI GetTime");
_Static_assert(offsetof(XP_EFI_BOOT_SERVICES_PREFIX, AllocatePages) == 40, "UEFI allocation");
_Static_assert(offsetof(XP_EFI_BOOT_SERVICES_PREFIX, GetMemoryMap) == 56, "UEFI map");
_Static_assert(offsetof(XP_EFI_BOOT_SERVICES_PREFIX, HandleProtocol) == 152, "UEFI protocol");
_Static_assert(offsetof(XP_EFI_BOOT_SERVICES_PREFIX, ExitBootServices) == 232, "UEFI exit");
_Static_assert(offsetof(XP_EFI_SYSTEM_TABLE, BootServices) == 96, "UEFI services");
#endif
