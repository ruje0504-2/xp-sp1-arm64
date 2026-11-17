#include "time.h"

NTSTATUS BlArm64ConvertRtc(const XP_EFI_TIME *rtc, ULONG policy, PLARGE_INTEGER utc)
{
    if (!rtc || !utc || policy>XP_RTC_UNSPECIFIED_UTC ||
        rtc->Year<1900 || rtc->Year>9999 || rtc->Nanosecond>=1000000000 ||
        (rtc->Daylight&~3u)) return STATUS_INVALID_PARAMETER;
    if (rtc->TimeZone==2047) {
        /* No guessed DST offset when the timezone itself is unknown. */
        if (policy!=XP_RTC_UNSPECIFIED_UTC || rtc->Daylight)
            return STATUS_NOT_SUPPORTED;
    } else if (rtc->TimeZone < -1440 || rtc->TimeZone > 1440) {
        return STATUS_INVALID_PARAMETER;
    }
    TIME_FIELDS fields;
    fields.Year=rtc->Year;fields.Month=rtc->Month;fields.Day=rtc->Day;
    fields.Hour=rtc->Hour;fields.Minute=rtc->Minute;fields.Second=rtc->Second;
    fields.Milliseconds=0;fields.Weekday=0;
    LARGE_INTEGER result;
    if (!RtlTimeFieldsToTime(&fields,&result)) return STATUS_INVALID_PARAMETER;
    result.QuadPart+=rtc->Nanosecond/100;
    /* UEFI 2.10: LocalTime = UTC - TimeZone. The offset includes DST. */
    if (rtc->TimeZone!=2047)
        result.QuadPart+=(LONG64)rtc->TimeZone*600000000LL;
    *utc=result;
    return STATUS_SUCCESS;
}

NTSTATUS BlArm64CaptureTime(XP_EFI_SYSTEM_TABLE *system, XP_ARM64_BOOT_TIME *time)
{
    if (!system || !time || time->Policy>XP_RTC_UNSPECIFIED_UTC)
        return STATUS_INVALID_PARAMETER;
    XP_EFI_RUNTIME_SERVICES_PREFIX *services=system->RuntimeServices;
    time->Status=STATUS_NOT_SUPPORTED;
    time->FirmwareStatus=XP_EFI_UNSUPPORTED;
    if (!services || services->Hdr.Signature!=0x56524553544e5552ULL ||
        services->Hdr.HeaderSize<sizeof(*services) || !services->GetTime)
        return time->Status;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(time->Frequency));
    if (!time->Frequency || time->Frequency>0xffffffffULL)
        return time->Status;
    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(time->CounterBefore) :: "memory");
    time->FirmwareStatus=services->GetTime(&time->Rtc,&time->Capabilities);
    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(time->CounterAfter) :: "memory");
    if (time->FirmwareStatus) return time->Status=STATUS_UNSUCCESSFUL;
    if (time->CounterAfter<time->CounterBefore || !time->Capabilities.Resolution)
        return time->Status=STATUS_INVALID_PARAMETER;
    return time->Status=BlArm64ConvertRtc(&time->Rtc,time->Policy,&time->Utc);
}
