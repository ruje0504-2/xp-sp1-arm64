/* Native machine-dependent work called by XP KeInitSystem in Phase 1. */
#include "ki.h"
#include "../../hal/arm64/gicv2.h"
#include "../../mm/arm64/bootio_private.h"
#include <ntimage.h>

static ULONG cache_line_bytes(ULONG64 selector)
{
    ULONG64 ccsidr;
    __asm__ volatile("msr csselr_el1, %1\nisb\nmrs %0, ccsidr_el1"
                     : "=r"(ccsidr) : "r"(selector) : "memory");
    return 1u << ((ccsidr & 7) + 4);
}

static ULONG largest_cache_line(VOID)
{
    ULONG64 daif, selector, clidr;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15\n"
                     "mrs %1, csselr_el1\nmrs %2, clidr_el1"
                     : "=r"(daif), "=r"(selector), "=r"(clidr) :: "memory");
    ULONG largest = 0;
    for (ULONG level = 0; level < 7; ++level) {
        ULONG type = (ULONG)(clidr >> (3 * level)) & 7;
        if (!type) break;
        if (type > 4) {
            largest = 0;
            break;
        }
        if (type != 1) {
            ULONG data = cache_line_bytes((ULONG64)level << 1);
            if (data > largest) largest = data;
        }
        if (type == 1 || type == 3) {
            ULONG instruction = cache_line_bytes(((ULONG64)level << 1) | 1);
            if (instruction > largest) largest = instruction;
        }
    }
    __asm__ volatile("msr csselr_el1, %0\nisb\nmsr daif, %1\nisb"
                     :: "r"(selector), "r"(daif) : "memory");
    return largest;
}

BOOLEAN KiInitMachineDependent(VOID)
{
    if (!KeLoaderBlock || KeNumberProcessors != 1 || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !KeGetCurrentThread() || KeGetCurrentPrcb() != (PKPRCB)KeLoaderBlock->Prcb ||
        KeProcessorArchitecture != PROCESSOR_ARCHITECTURE_UNKNOWN)
        return FALSE;

    /* Keep firmware's live MAIR encoding. The original MM override table
     * must agree with the HAL's checked WB aliases before publishing it. */
    HAL_PLATFORM_INFORMATION platform;
    ULONG returned;
    if (!NT_SUCCESS(HalArm64QueryBootPlatformInformation(HalPlatformInformation,
            sizeof(platform), &platform, &returned)) || returned != sizeof(platform) ||
        !(platform.PlatformFlags & HAL_PLATFORM_DISABLE_UC_MAIN_MEMORY) ||
        MiPlatformCacheAttributes[MmNonCached] != MiCached ||
        MiPlatformCacheAttributes[MmMaximumCacheType + MmNonCached] != MiNonCached)
        return FALSE;

    HAL_ARM64_IRQ_STATS irq;
    LARGE_INTEGER frequency;
    HalArm64GetIrqStats(&irq);
    HalArm64QueryPerformanceCounter(&frequency);
    if (!irq.Initialized || !irq.TimerActive || !frequency.QuadPart ||
        (ULONG64)frequency.QuadPart != irq.Frequency)
        return FALSE;

    ULONG alignment = largest_cache_line();
    if (!alignment) return FALSE;
    KeLargestCacheLine = alignment;
    SharedUserData->ImageNumberLow = IMAGE_FILE_MACHINE_ARM64;
    SharedUserData->ImageNumberHigh = IMAGE_FILE_MACHINE_ARM64;
    KeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
    return TRUE;
}
