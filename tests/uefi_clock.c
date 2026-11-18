#include "ki.h"
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/ke/arm64/clock.h"
#include "../base/ntos/hal/arm64/gicv2.h"
#include "uefi_test.h"

VOID XpCheckClockContracts(VOID)
{
    XP_EFI_TIME rtc={2000,1,1,0,0,0,0,0,0,0,0};
    LARGE_INTEGER value;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_SUCCESS);
    CHECK(value.QuadPart==125911584000000000LL);
    rtc.Hour=12;rtc.TimeZone=480;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_SUCCESS);
    CHECK(value.QuadPart==125912304000000000LL); /* PST noon == UTC 20:00 */
    rtc.Hour=13;rtc.TimeZone=420;rtc.Daylight=3;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_SUCCESS);
    CHECK(value.QuadPart==125912304000000000LL); /* offset already accounts for DST */
    rtc.Hour=8;rtc.TimeZone=-480;rtc.Daylight=0;rtc.Nanosecond=123456789;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_SUCCESS);
    CHECK(value.QuadPart==125911584001234567LL);
    rtc.TimeZone=2047;
    value.QuadPart=42;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_NOT_SUPPORTED && value.QuadPart==42);
    CHECK(BlArm64ConvertRtc(&rtc,1,&value)==STATUS_SUCCESS);
    rtc.Daylight=1;value.QuadPart=42;
    CHECK(BlArm64ConvertRtc(&rtc,1,&value)==STATUS_NOT_SUPPORTED && value.QuadPart==42);
    rtc.Daylight=0;rtc.TimeZone=0;rtc.Nanosecond=0;rtc.Month=2;rtc.Day=29;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_SUCCESS); /* leap century */
    rtc.Year=1900;value.QuadPart=42;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_INVALID_PARAMETER && value.QuadPart==42);
    rtc.Year=2000;rtc.Month=13;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_INVALID_PARAMETER);
    rtc.Month=1;rtc.Day=1;rtc.TimeZone=1441;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_INVALID_PARAMETER);
    rtc.TimeZone=0;rtc.Nanosecond=1000000000;
    CHECK(BlArm64ConvertRtc(&rtc,0,&value)==STATUS_INVALID_PARAMETER);
    ULONG64 converted=42;
    CHECK(KiArm64CounterTo100ns(62500001,62500000,&converted)==STATUS_SUCCESS && converted==10000000);
    CHECK(KiArm64CounterTo100ns(98305,98304,&converted)==STATUS_SUCCESS && converted==10000101);
    CHECK(KiArm64CounterTo100ns(0xffffffffULL,0xffffffffULL,&converted)==STATUS_SUCCESS && converted==10000000);
    converted=42;
    CHECK(KiArm64CounterTo100ns(~0ULL,1,&converted)==STATUS_INTEGER_OVERFLOW && converted==42);
    CHECK(KiArm64CounterTo100ns(1,0,&converted)==STATUS_INVALID_PARAMETER && converted==42);
    value.QuadPart=-7;
    CHECK(RtlExtendedIntegerMultiply(value,-3).QuadPart==21);
    value.QuadPart=0x7fffffffffffffffLL;
    CHECK(RtlExtendedIntegerMultiply(value,2).QuadPart==-2);
    XpTestReport("PASS: original XP calendar conversion, UEFI timezone/rejection and counter scaling boundaries\n");
    CHECK(KeQueryTimeIncrement() == 10000 && KeMinimumIncrement == 10000 &&
          KeTimeIncrement == 10000 && KeTimeAdjustment == 10000 && KiTickOffset == 10000 &&
          (ULONG64)KiTimeIncrementReciprocal.QuadPart == 0xd1b71758e219652cULL &&
          KiTimeIncrementShiftCount == 13 && SharedUserData->TickCountMultiplier == 0x01000000 &&
          !SharedUserData->TickCountLow);
    const ULONG64 last_boundary = (~0ULL / 10000 - TIMER_TABLE_SIZE) * 10000;
    KTIMER timer;
    KeInitializeTimer(&timer);
    for (ULONG bucket = 0; bucket < TIMER_TABLE_SIZE; ++bucket) {
        const ULONG64 low = 10000ULL * (bucket + 1), high = last_boundary + 10000ULL * bucket;
        const ULONG64 samples[] = {low - 1, low, low + 1, high - 1, high, high + 1};
        for (ULONG i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
            ULONG64 time = samples[i];
            LARGE_INTEGER current = {.QuadPart = (LONG64)(time - 1)};
            LARGE_INTEGER interval = {.QuadPart = -1};
            CHECK(KiArm64InterruptTicks(time) == time / 10000 &&
                  KiComputeTimerTableIndex(interval, current, &timer) == (time / 10000) % TIMER_TABLE_SIZE &&
                  timer.DueTime.QuadPart == time);
        }
    }
    CHECK(KiArm64InterruptTicks(0) == 0 && KiArm64InterruptTicks(~0ULL) == ~0ULL / 10000);
    XpTestReport("Clock setup evidence increment="); XpTestHex(KeQueryTimeIncrement());
    XpTestReport(" reciprocal="); XpTestHex(KiTimeIncrementReciprocal.QuadPart);
    XpTestReport(" shift="); XpTestHex(KiTimeIncrementShiftCount);
    XpTestReport(" multiplier="); XpTestHex(SharedUserData->TickCountMultiplier);
    XpTestReport(" samples="); XpTestHex(6 * TIMER_TABLE_SIZE + 2);
    XpTestReport("\nPASS: original XP clock setup initializes native reciprocal timer buckets\n");

}

VOID XpCheckPublishedClock(PXP_ARM64_BOOT_BLOCK block)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2\nisb" : "=r"(daif) :: "memory");
    KI_ARM64_CLOCK_STATE state;
    HAL_ARM64_IRQ_STATS irq;
    KiArm64GetBootClockState(&state);HalArm64GetIrqStats(&irq);
    CHECK(state.Initialized && state.Frequency==block->Time.Frequency);
    CHECK(state.BootCounter==block->Time.CounterBefore && state.SampleCounter==block->Time.CounterAfter);
    CHECK(state.UtcAtSample==(ULONG64)block->Time.Utc.QuadPart);
    CHECK((ULONG_PTR)SharedUserData==KI_USER_SHARED_DATA);
    CHECK(state.SharedPagePhysical==(block->SharedPagePfn<<PAGE_SHIFT));
    CHECK(((state.SharedPagePar>>56)&255)==255 && ((state.SharedPagePar>>7)&3)==3);
    CHECK(state.SharedPageLoaderType==LoaderMemoryData);
    CHECK(state.Publications==irq.Interrupts+1);
    if (irq.Interrupts) CHECK(state.LastCounter==irq.LastCounter);
    CHECK(KeQueryInterruptTime()==state.InterruptTime);
    LARGE_INTEGER time;
    KeQuerySystemTime(&time);CHECK((ULONG64)time.QuadPart==state.SystemTime);
    CHECK(SharedUserData->InterruptTime.High1Time==SharedUserData->InterruptTime.High2Time);
    CHECK(SharedUserData->SystemTime.High1Time==SharedUserData->SystemTime.High2Time);
    CHECK(state.InterruptTime==(state.LastCounter-state.BootCounter)*10000000/state.Frequency);
    CHECK(state.SystemTime==state.UtcAtSample+(state.LastCounter-state.SampleCounter)*10000000/state.Frequency);
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)SharedUserData)==state.SharedPagePhysical);
    CHECK(MmArm64QueryPhysicalAddress(state.SharedPagePhysical)==~0ULL);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
