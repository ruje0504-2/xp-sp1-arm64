/* The firmware control instruments one already-authorized exit attempt
 * before the independent native kernel runs its own controls. */
#include "../base/boot/arm64/boot.h"
#include "uefi_test.h"

static XP_ARM64_BOOT_BLOCK boot;
static XP_EFI_EXIT_BOOT_SERVICES real_exit;
static XP_EFI_ALLOCATE_PAGES real_allocate;
static XP_EFI_FREE_PAGES real_free;
static ULONG_PTR outstanding_pages;
static XP_EFI_PROTOCOL_SERVICES service_copy;
static XP_EFI_SYSTEM_TABLE system_copy;
static ULONG exit_calls;

static XP_EFI_STATUS track_allocate(ULONG type, ULONG memory_type, ULONG_PTR pages, PULONG64 address)
{
    XP_EFI_STATUS status=real_allocate(type,memory_type,pages,address);
    if (!status) outstanding_pages+=pages;
    return status;
}
static XP_EFI_STATUS track_free(ULONG64 address, ULONG_PTR pages)
{
    XP_EFI_STATUS status=real_free(address,pages);
    if (!status) {
        CHECK(pages<=outstanding_pages);
        outstanding_pages-=pages;
    }
    return status;
}

static XP_EFI_STATUS stale_key_once(PVOID image, ULONG_PTR key)
{
    if (++exit_calls==1) {
        CHECK(boot.Volume.DevicePathBytes > 4 && boot.Nt.ArcBootDeviceName == boot.Volume.ArcName &&
              boot.Nt.NtBootPathName == boot.Volume.NtDirectory && boot.Volume.NtDirectory[0] == '\\');
        XpTestReport("Firmware boot volume ARC=");
        XpTestReport(boot.Volume.ArcName);
        XpTestReport("\n");
        XP_EFI_STATUS status=real_exit(image,key^1);
        CHECK(status==XP_EFI_INVALID_PARAMETER);
        XpTestReport("XP boot: real firmware rejected a stale memory-map key\n");
        return status;
    }
    CHECK(exit_calls==boot.ExitAttempts);
    return real_exit(image,key);
}

ULONG_PTR EfiBootMain(PVOID image, XP_EFI_SYSTEM_TABLE *system)
{
    XpTestReport("XP ARM64 NT loader entered\n");
    system_copy=*system;
    service_copy.Prefix = *system->BootServices;
    if (system->BootServices->Hdr.HeaderSize >= sizeof(service_copy)) {
        service_copy.LocateProtocol = ((XP_EFI_PROTOCOL_SERVICES *)system->BootServices)->LocateProtocol;
        service_copy.Prefix.Hdr.HeaderSize = sizeof(service_copy);
    } else service_copy.Prefix.Hdr.HeaderSize = sizeof(service_copy.Prefix);
    real_allocate=service_copy.Prefix.AllocatePages;
    real_free=service_copy.Prefix.FreePages;
    service_copy.Prefix.AllocatePages=track_allocate;
    service_copy.Prefix.FreePages=track_free;
    real_exit=service_copy.Prefix.ExitBootServices;
    service_copy.Prefix.ExitBootServices=stale_key_once;
    system_copy.BootServices=&service_copy.Prefix;
    /* The build selects -rtc base=utc,clock=vm for this control. */
    boot.Time.Policy=XP_RTC_UNSPECIFIED_UTC;
    XP_EFI_STATUS status=BlArm64Boot(image,&system_copy,&boot);
    if (status==XP_EFI_LOAD_ERROR && !boot.ExitAttempted) {
        CHECK(!exit_calls && !outstanding_pages && !boot.KernelImage.Base &&
              !boot.StackAllocationBase && !boot.Tables.PoolBase && !boot.Tables.StackArena &&
              !boot.Volume.DevicePathBytes && !boot.Nt.ArcBootDeviceName && !boot.Nt.NtBootPathName);
        XpTestReport("PASS: rejected malformed native PE before firmware exit and freed all loader pages\n");
        XpTestFinish(0);
    }
    XpTestReport("Boot returned=");XpTestHex(status);
    XpTestReport(" init status=");XpTestHex((ULONG)boot.InitializationStatus);
    XpTestReport(" time status=");XpTestHex((ULONG)boot.Time.Status);
    XpTestReport(" firmware time status=");XpTestHex(boot.Time.FirmwareStatus);XpTestReport("\n");
    CHECK(FALSE);
    return XP_EFI_ABORTED;
}
