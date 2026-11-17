#ifndef XP_BOOT_ARM64_TIME_H
#define XP_BOOT_ARM64_TIME_H
#include <ntos.h>
#include "uefi.h"
/* Only a platform with independently known RTC semantics may choose UTC.
 * Default rejects an unspecified timezone. No firmware settings are changed. */
#define XP_RTC_UNSPECIFIED_REJECT 0
#define XP_RTC_UNSPECIFIED_UTC 1
typedef struct {
    XP_EFI_TIME Rtc;
    XP_EFI_TIME_CAPABILITIES Capabilities;
    XP_EFI_STATUS FirmwareStatus;
    NTSTATUS Status;
    ULONG Policy;
    ULONG64 CounterBefore, CounterAfter, Frequency;
    LARGE_INTEGER Utc;
} XP_ARM64_BOOT_TIME;
NTSTATUS BlArm64ConvertRtc(const XP_EFI_TIME *Rtc, ULONG Policy, PLARGE_INTEGER Utc);
NTSTATUS BlArm64CaptureTime(XP_EFI_SYSTEM_TABLE *System, XP_ARM64_BOOT_TIME *Time);
#endif
