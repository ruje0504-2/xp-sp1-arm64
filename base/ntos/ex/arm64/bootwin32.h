#ifndef XP_ARM64_BOOT_WIN32_H
#define XP_ARM64_BOOT_WIN32_H
#include <ntos.h>

extern POBJECT_TYPE ExWindowStationObjectType, ExDesktopObjectType;

/* Console objects are created by trusted kernel callers at PASSIVE_LEVEL. */
NTSTATUS ExpArm64InitializeWin32Console(VOID);
NTSTATUS ExpArm64CreateWindowStation(PCUNICODE_STRING Name, PHANDLE Handle);
NTSTATUS ExpArm64CreateDesktop(HANDLE Station, PCUNICODE_STRING Name, PHANDLE Handle);
NTSTATUS ExpArm64CloseUserHandle(HANDLE Handle, POBJECT_TYPE Type);
VOID ExpArm64ReleaseWin32Bindings(PETHREAD Thread, BOOLEAN LastThread);
VOID ExpArm64ReleaseThreadUserObjects(PKTHREAD Thread);
VOID ExpArm64ReleaseGdiObjects(PEPROCESS Process);
ULONG ExpArm64CountGdiObjects(PEPROCESS Process);
ULONG ExpArm64CountUserObjects(PEPROCESS Process);
VOID ExpArm64RouteBootInput(BOOLEAN Keyboard, USHORT Type, USHORT Code, LONG Value);
#endif
