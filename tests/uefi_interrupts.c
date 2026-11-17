#include "../base/boot/arm64/boot.h"
#include "../base/ntos/hal/arm64/gicv2.h"
#include "../base/ntos/ke/arm64/clock.h"
#include "uefi_test.h"
VOID XpCheckPublishedClock(PXP_ARM64_BOOT_BLOCK Block);
static BOOLEAN enabled;
BOOLEAN XpHandleHardwareInterrupt(PKTRAP_FRAME frame, ULONG kind)
{
    if (kind==0 && ((frame->Esr>>26)&63)==0x3c && (frame->Esr&65535)==0x7fd) {
        HAL_ARM64_IRQ_STATS stats;HalArm64GetIrqStats(&stats);
        XpTestReport("HAL failure=");XpTestHex(stats.LastFailure);XpTestReport("\n");
        XpTestFailure(__LINE__);
    }
    if (kind!=1) return FALSE;
    CHECK(enabled || KeGetCurrentThread()!=NULL);
    CHECK(frame->ExceptionActive==0 && KeGetPcr()->LastVectorType==1 &&
          !KeGetPcr()->LastTrapEsrValid);
    CHECK(HalArm64HandleBootIrq(frame));
    return TRUE;
}
static VOID wait_counter(ULONG64 interval)
{
    ULONG64 until=HalArm64ReadCounter()+interval;
    while ((LONG64)(HalArm64ReadCounter()-until)<0) __asm__ volatile("yield");
}
static VOID wait_interrupts(ULONG64 count, ULONG64 frequency)
{
    ULONG64 limit=HalArm64ReadCounter()+frequency/2;
    HAL_ARM64_IRQ_STATS stats;
    ULONG64 last_time=KeQueryInterruptTime();
    for (;;) {
        ULONG64 current_time=KeQueryInterruptTime();
        CHECK(current_time>=last_time);last_time=current_time;
        HalArm64GetIrqStats(&stats);
        if (stats.Interrupts>=count) return;
        CHECK((LONG64)(HalArm64ReadCounter()-limit)<0);
        __asm__ volatile("yield");
    }
}
VOID XpCheckHardwareInterrupts(PXP_ARM64_BOOT_BLOCK block)
{
    CHECK(block->PlatformStatus==STATUS_SUCCESS);
    CHECK(block->HalStatus==STATUS_SUCCESS);
    CHECK(KeGetCurrentIrql()==HIGH_LEVEL && HalArm64ReadPriorityMask()==0);
    XpCheckPublishedClock(block);
    KI_ARM64_CLOCK_STATE clock;
    KiArm64GetBootClockState(&clock);
    CHECK(KiArm64UpdateBootClock(clock.LastCounter-1)==STATUS_INVALID_PARAMETER);
    CHECK(KeQueryInterruptTime()==clock.InterruptTime);
    ULONG64 saved_daif, started=HalArm64ReadCounter(), traps=block->Pcr.TrapCount;
    __asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
    HAL_ARM64_IRQ_STATS stats;
    HalArm64GetIrqStats(&stats);
    CHECK(stats.Initialized && stats.Frequency>=1000);
    ULONG64 frequency=stats.Frequency;
    enabled=TRUE;
    CHECK(HalArm64StartBootTimer(1000)==STATUS_SUCCESS);
    KeLowerIrql(PASSIVE_LEVEL);
    CHECK(KiArm64UpdateBootClock(HalArm64ReadCounter())==STATUS_INVALID_DEVICE_STATE);
    CHECK(HalArm64ReadPriorityMask()==0xf0);
    __asm__ volatile("msr daifclr, #2\nisb" ::: "memory");
    wait_interrupts(5,frequency);

    KIRQL old=KeRaiseIrqlToDpcLevel();
    CHECK(old==PASSIVE_LEVEL && HalArm64ReadPriorityMask()==0xd0);
    HalArm64GetIrqStats(&stats);
    wait_interrupts(stats.Interrupts+3,frequency);
    HalArm64GetIrqStats(&stats);
    CHECK(stats.LastInterruptedIrql==DISPATCH_LEVEL);
    KeLowerIrql(old);

    KeRaiseIrql(CLOCK_LEVEL,&old);
    CHECK(old==PASSIVE_LEVEL && HalArm64ReadPriorityMask()==0x20);
    HalArm64GetIrqStats(&stats);
    ULONG64 masked_count=stats.Interrupts;
    ULONG64 masked_time=KeQueryInterruptTime();
    wait_counter(frequency/250);
    HalArm64GetIrqStats(&stats);
    CHECK(stats.Interrupts==masked_count && (HalArm64ReadTimerControl()&4));
    CHECK(KeQueryInterruptTime()==masked_time);
    KeLowerIrql(old);
    wait_interrupts(masked_count+1,frequency);
    CHECK(KeQueryInterruptTime()>=masked_time+39000);
    XpCheckPublishedClock(block);

    __asm__ volatile("msr daifset, #2\nisb" ::: "memory");
    HalArm64StopBootTimer();
    HalArm64GetIrqStats(&stats);
    ULONG64 stopped=stats.Interrupts;
    ULONG64 stopped_time=KeQueryInterruptTime();
    CHECK(!(HalArm64ReadTimerControl()&1));
    CHECK(!HalArm64TimerPendingOrActive());
    __asm__ volatile("msr daifclr, #2\nisb" ::: "memory");
    wait_counter(frequency/250);
    HalArm64GetIrqStats(&stats);
    CHECK(stats.Interrupts==stopped && !stats.TimerActive && !stats.LastFailure);
    CHECK(KeQueryInterruptTime()==stopped_time);
    /* Restart must account for real elapsed counter time, not interrupt count. */
    CHECK(HalArm64StartBootTimer(1000)==STATUS_SUCCESS);
    wait_interrupts(stopped+2,frequency);
    CHECK(KeQueryInterruptTime()>=stopped_time+39000);
    __asm__ volatile("msr daifset, #2\nisb" ::: "memory");
    HalArm64StopBootTimer();
    CHECK(!HalArm64TimerPendingOrActive());
    HalArm64GetIrqStats(&stats);
    XpCheckPublishedClock(block);
    KiArm64GetBootClockState(&clock);
    CHECK(stats.LastIntId==block->Platform.TimerIrq);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(saved_daif) : "memory");
    KeRaiseIrql(HIGH_LEVEL,&old);
    enabled=FALSE;
    CHECK(old==PASSIVE_LEVEL && HalArm64ReadPriorityMask()==0);
    CHECK(block->Pcr.TrapCount-traps==stats.Interrupts+stats.Spurious);
    CHECK(KeTickCount.QuadPart==0); /* NT time accounting is not connected yet. */
    XpTestReport("IRQ evidence count=");XpTestHex(stats.Interrupts);
    XpTestReport(" frequency=");XpTestHex(frequency);
    XpTestReport(" elapsed_counter=");XpTestHex(HalArm64ReadCounter()-started);
    XpTestReport(" missed_periods=");XpTestHex(stats.MissedPeriods);XpTestReport("\n");
    XpTestReport("Clock evidence interrupt=");XpTestHex(clock.InterruptTime);
    XpTestReport(" system=");XpTestHex(clock.SystemTime);
    XpTestReport(" publications=");XpTestHex(clock.Publications);XpTestReport("\n");
    XpTestReport("Clock backing physical=");XpTestHex(clock.SharedPagePhysical);
    XpTestReport(" par=");XpTestHex(clock.SharedPagePar);
    XpTestReport(" loader_type=");XpTestHex(clock.SharedPageLoaderType);XpTestReport("\n");
    XpTestReport("PASS: resident shared time, actual IRQ publication, masked delay and timer restart elapsed-time catch-up\n");
    XpTestReport("PASS: ACPI-discovered GICv2 physical timer IRQ, hardware IRQL masking/deferred delivery and interrupt retirement\n");
}
