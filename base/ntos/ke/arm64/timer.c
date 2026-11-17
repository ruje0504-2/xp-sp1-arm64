/* Counter-derived NT_UP tick publication, timer expiry and sampled runtime. */
#include "ki.h"
#include "scheduler.h"
#include "clock.h"
#include "../../hal/arm64/gicv2.h"
static BOOLEAN enabled, initialized;
static ULONG scan_hand;
static KI_ARM64_TIMER_DISPATCH_STATS stats;
static VOID publish_ticks(VOID)
{
    ULONG64 ticks=KeQueryInterruptTime()/10000;
    __atomic_store_n(&KeTickCount.QuadPart,(LONG64)ticks,__ATOMIC_RELEASE);
    stats.Ticks=ticks;
}
/* Follow the original IA64/AMD64 KeUpdateRunTime EL1 classification.
 * Charge elapsed 1ms tick boundaries to the interrupted context. Reset
 * the baseline at timer start so suspended clock service is not CPU time.
 * This sampled accounting cannot subdivide an IRQ-masked interval. */
static VOID account_runtime(PKTRAP_FRAME frame, ULONG64 elapsed)
{
    PKPRCB prcb=KeGetCurrentPrcb();
    PKTHREAD thread=prcb->CurrentThread;
    ULONG ticks=(ULONG)elapsed;

    stats.RuntimeTicks+=elapsed;
    prcb->KernelTime+=ticks;
    if (frame->PreviousIrql>DISPATCH_LEVEL) {
        prcb->InterruptTime+=ticks;
    } else if (frame->PreviousIrql==DISPATCH_LEVEL && prcb->DpcRoutineActive) {
        prcb->DpcTime+=ticks;
    } else {
        thread->KernelTime+=ticks;
        thread->ApcState.Process->KernelTime+=ticks;
    }

    if (thread==prcb->IdleThread) return;
    LONG quantum=thread->Quantum;
    /* Saturate before converting back to SCHAR, even after a long delay. */
    if (quantum<=0 || elapsed>=((ULONG)quantum+CLOCK_QUANTUM_DECREMENT-1)/CLOCK_QUANTUM_DECREMENT) {
        thread->Quantum=0;
        if (!prcb->QuantumEnd) {
            prcb->QuantumEnd=1;
            KiRequestSoftwareInterrupt(DISPATCH_LEVEL);
        }
    } else {
        thread->Quantum=(SCHAR)(quantum-(LONG)elapsed*CLOCK_QUANTUM_DECREMENT);
    }
}
NTSTATUS KiArm64EnableTimerDispatch(VOID)
{
    HAL_ARM64_IRQ_STATS hal;
    HalArm64GetIrqStats(&hal);
    if (enabled || hal.TimerActive || KeNumberProcessors!=1 ||
        KeGetCurrentIrql()!=PASSIVE_LEVEL || !KeGetCurrentThread())
        return STATUS_INVALID_DEVICE_STATE;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    KIRQL old;
    KeRaiseIrql(CLOCK_LEVEL,&old);
    NTSTATUS status=KiArm64UpdateBootClock(HalArm64ReadCounter());
    if (NT_SUCCESS(status)) {
        publish_ticks();
        if (!initialized) {
            stats.StartTicks=stats.Ticks;
            /* No timer bucket has ever been consumed before the first start.
             * Thereafter retain the earliest unscanned hand across stops. */
            scan_hand=0;
            initialized=TRUE;
        }
        enabled=TRUE;
        status=HalArm64StartBootTimer(1000);
        if (!NT_SUCCESS(status)) enabled=FALSE;
    }
    KeLowerIrql(old);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (NT_SUCCESS(status)) _enable();
    return status;
}
VOID KiArm64ClockTimerInterrupt(PKTRAP_FRAME frame)
{
    if (!enabled) return;
    if (KeGetCurrentIrql()!=CLOCK_LEVEL)
        KeBugCheckEx((ULONG)STATUS_INVALID_DEVICE_STATE,0x21,KeGetCurrentIrql(),0,0);
    ++stats.Interrupts;
    ULONG64 previous=stats.Ticks;
    publish_ticks();
    if (stats.Ticks>previous) account_runtime(frame,stats.Ticks-previous);
    /* A queued DPC keeps its earliest hand. Advancing only on a new insert
     * makes delayed/coalesced interrupts and table wraps scan conservatively. */
    if (KeInsertQueueDpc(&KiTimerExpireDpc,ULongToPtr(scan_hand),NULL)) {
        scan_hand=(ULONG)stats.Ticks;
        ++stats.QueueInsertions;
    }
}
VOID KiArm64DisableTimerDispatch(VOID)
{
    if (!enabled || KeGetCurrentIrql()!=PASSIVE_LEVEL)
        KeBugCheckEx((ULONG)STATUS_INVALID_DEVICE_STATE,0x22,KeGetCurrentIrql(),0,0);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
    HalArm64StopBootTimer();
    enabled=FALSE;
    /* Queued expiration work remains owned by the dispatch SGI. */
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
VOID KiArm64GetTimerDispatchStats(KI_ARM64_TIMER_DISPATCH_STATS *out)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    _disable(); *out=stats;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
__declspec(noreturn) VOID KiArm64IdleLoop(VOID)
{
    for (;;) {
        if (KeGetCurrentThread()!=KeGetCurrentPrcb()->IdleThread || KeGetCurrentIrql()!=PASSIVE_LEVEL)
            KeBugCheckEx((ULONG)STATUS_INVALID_DEVICE_STATE,0x24,0,0,0);
        _disable();
        /* WFI wakes for a pending eligible PPI or SGI even with PSTATE.I set. */
        __asm__ volatile("dsb sy\nwfi" ::: "memory");
        _enable();
    }
}
