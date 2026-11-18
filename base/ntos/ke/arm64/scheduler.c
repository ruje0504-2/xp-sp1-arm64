/* ARM64 NT_UP dispatcher bridges. Generic selection, waits and APC queues
 * and quantum-end decisions use the original XP algorithms. */
#include "ki.h"
#include "scheduler.h"
#include "clock.h"
#include "../../ps/arm64/bootprocess.h"
KAFFINITY KiIdleSummary;
ULONG KiReadySummary;
KSPIN_LOCK KiProfileLock;
/* Original psinit.c declares this zero-initialized policy value. Until the
 * executive sets a foreground policy, the kernel-only process has no boost. */
ULONG PsPrioritySeperation;
volatile ULONG64 KiArm64SchedulerSwitches;
volatile ULONG64 KiArm64QuantumSwitches;
volatile ULONG_PTR KiArm64BugCheckData[5];
VOID KiRetireDpcList(PKPRCB Prcb);
static BOOLEAN initialized;
static PKPROCESS idle_process;

__declspec(noreturn) VOID KeBugCheckEx(ULONG code, ULONG_PTR a, ULONG_PTR b,
                                      ULONG_PTR c, ULONG_PTR d)
{
    __asm__ volatile("msr daifset, #15" ::: "memory");
    KiArm64BugCheckData[0]=code; KiArm64BugCheckData[1]=a; KiArm64BugCheckData[2]=b;
    KiArm64BugCheckData[3]=c; KiArm64BugCheckData[4]=d;
    __asm__ volatile("dsb sy\nbrk #0x7ef" ::: "memory");
    for (;;) __asm__ volatile("wfe");
}
__declspec(noreturn) VOID KeBugCheck(ULONG code)
{
    KeBugCheckEx(code, 0, 0, 0, 0);
}
static VOID require(BOOLEAN condition, ULONG tag)
{
    if (!condition) KeBugCheckEx((ULONG)STATUS_INVALID_DEVICE_STATE, tag,
        (ULONG_PTR)KeGetCurrentThread(), KeGetCurrentIrql(), 0);
}
VOID KeInitializeSpinLock(PKSPIN_LOCK lock)
{
    *lock = 0;
}
VOID KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK lock)
{
    require(KeNumberProcessors==1 && KeGetCurrentIrql()>=DISPATCH_LEVEL && lock, 13);
    KSPIN_LOCK expected=0;
    require(__atomic_compare_exchange_n(lock,&expected,1,FALSE,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED),14);
}
VOID KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK lock)
{
    require(lock && KeGetCurrentIrql()>=DISPATCH_LEVEL &&
        __atomic_load_n(lock,__ATOMIC_RELAXED)==1,15);
    __atomic_store_n(lock,0,__ATOMIC_RELEASE);
}
KIRQL KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK lock)
{
    KIRQL previous;
    KeRaiseIrql(DISPATCH_LEVEL, &previous);
    KeAcquireSpinLockAtDpcLevel(lock);
    return previous;
}
VOID KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL previous)
{
    KeReleaseSpinLockFromDpcLevel(lock);
    KeLowerIrql(previous);
}
static VOID acquire(PKSPIN_LOCK lock, PKLOCK_QUEUE_HANDLE handle, KIRQL level)
{
    require(KeNumberProcessors==1 && lock && handle && !((ULONG_PTR)lock & 7), 1);
    KeRaiseIrql(level, &handle->OldIrql);
    handle->LockQueue.Next = NULL;
    handle->LockQueue.Lock = lock;
    KSPIN_LOCK expected = 0;
    /* UP at DISPATCH_LEVEL cannot wait for a preempted owner. Reentry is a
     * kernel contract violation, not a reason to report a lock acquired. */
    require(__atomic_compare_exchange_n(lock, &expected, (KSPIN_LOCK)handle,
                                        FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED), 2);
}
VOID KeAcquireInStackQueuedSpinLock(PKSPIN_LOCK lock, PKLOCK_QUEUE_HANDLE handle)
{
    acquire(lock, handle, DISPATCH_LEVEL);
}
VOID KeAcquireInStackQueuedSpinLockRaiseToSynch(PKSPIN_LOCK lock, PKLOCK_QUEUE_HANDLE handle)
{
    acquire(lock, handle, SYNCH_LEVEL);
}
VOID KeReleaseInStackQueuedSpinLockFromDpcLevel(PKLOCK_QUEUE_HANDLE handle)
{
    require(handle && handle->LockQueue.Lock && KeGetCurrentIrql()>=DISPATCH_LEVEL, 3);
    PKSPIN_LOCK lock=handle->LockQueue.Lock;
    require(__atomic_load_n(lock, __ATOMIC_RELAXED)==(KSPIN_LOCK)handle, 4);
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    handle->LockQueue.Lock=NULL;
}
VOID KeReleaseInStackQueuedSpinLock(PKLOCK_QUEUE_HANDLE handle)
{
    KeReleaseInStackQueuedSpinLockFromDpcLevel(handle);
    KeLowerIrql(handle->OldIrql);
}
NTSTATUS KiArm64InitializeDispatcher(PKPROCESS process)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (initialized || !process || KeNumberProcessors!=1 || KeGetCurrentIrql()!=HIGH_LEVEL ||
        (daif & 0x3c0)!=0x3c0 || KeGetCurrentThread()!=NULL)
        return STATUS_INVALID_DEVICE_STATE;
    KeLowerIrql(APC_LEVEL);
    KiInitSystem();
    KIRQL previous;
    KeRaiseIrql(HIGH_LEVEL,&previous);
    require(previous==APC_LEVEL,18);
    KiProcessorBlock[0]=KeGetCurrentPrcb();
    PKPRCB prcb=KeGetCurrentPrcb();
    InitializeListHead(&prcb->DpcListHead);
    KeInitializeSpinLock(&prcb->DpcLock);
    /* Early policy: request dispatch for every queued DPC. No fabricated
     * request-rate estimate or adaptive threshold accounting. */
    prcb->MaximumDpcQueueDepth=1;
    prcb->MinimumDpcRate=0;
    KiIdleSummary=0; KiReadySummary=0;
    idle_process=process;
    initialized=TRUE;
    return STATUS_SUCCESS;
}
static BOOLEAN is_boot_process(PKPROCESS process)
{
    return process && (process == idle_process ||
        (PsInitialSystemProcess && process == &PsInitialSystemProcess->Pcb));
}

BOOLEAN KiSwapContext(PKTHREAD target)
{
    PKTHREAD old=KeGetCurrentThread();
    require(initialized && old && target && old!=target && KeGetCurrentIrql()==SYNCH_LEVEL, 6);
    PKPROCESS outgoing = old->ApcState.Process;
    PKPROCESS incoming = target->ApcState.Process;
    require(!KeIsExecutingDpc() && target->Header.Type==ThreadObject &&
        (is_boot_process(outgoing) || PsArm64UserProcessSpace((PEPROCESS)outgoing)) &&
        (is_boot_process(incoming) || PsArm64UserProcessSpace((PEPROCESS)incoming)) &&
        old->ApcStateIndex==OriginalApcEnvironment && target->ApcStateIndex==OriginalApcEnvironment &&
        (target->PreviousMode==KernelMode || (target->PreviousMode==UserMode && target->Teb && target->TrapFrame &&
                                             !is_boot_process(incoming))) &&
        target->KernelStackResident &&
        (target->State==Standby || target->State==Ready), 7);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");

    /* Kernel continuations use the bootstrap TTBR0. EL0 installs a private root. As in
     * the XP process switch, transfer the active bit before resuming the
     * target, with CLOCK IRQ masked until the new stack owns the CPU. */
    if (incoming != outgoing) {
        ULONG_PTR lower, upper;
        __asm__ volatile("mrs %0, ttbr0_el1\nmrs %1, ttbr1_el1" : "=r"(lower), "=r"(upper));
        lower &= 0x0000fffffffff000ULL;
        upper &= 0x0000fffffffff000ULL;
        KAFFINITY cpu = KeGetCurrentPrcb()->SetMember;
        require(idle_process->DirectoryTableBase[0] == lower &&
            outgoing->DirectoryTableBase[1] == upper && incoming->DirectoryTableBase[1] == upper &&
            outgoing->ActiveProcessors == cpu && !incoming->ActiveProcessors &&
            (incoming->Affinity & cpu), 19);
        outgoing->ActiveProcessors &= ~cpu;
        incoming->ActiveProcessors |= cpu;
    }
    target->State=Running;
    ++target->ContextSwitches; ++KiArm64SchedulerSwitches;
    NTSTATUS status=KiArm64SwitchKernelContext(old, target);
    require(NT_SUCCESS(status), (ULONG)status);
    require(KeGetCurrentThread()==old && old->State==Running && KeGetCurrentIrql()==SYNCH_LEVEL, 8);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (old->ApcState.KernelApcPending) {
        if (old->WaitIrql<APC_LEVEL) return TRUE;
        KiRequestSoftwareInterrupt(APC_LEVEL);
    }
    return FALSE;
}
VOID KiUnlockDispatcherDatabase(KIRQL old_irql)
{
    require(initialized && KeGetCurrentIrql()==SYNCH_LEVEL, 9);
    PKPRCB prcb=KeGetCurrentPrcb();
    if (prcb->NextThread && old_irql<DISPATCH_LEVEL) {
        PKTHREAD old=prcb->CurrentThread, next=prcb->NextThread;
        require(old && old->State==Running, 10);
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
        prcb->NextThread=NULL;
        old->WaitIrql=old_irql;
        /* Match the original AMD64 ordering: ready the outgoing thread while
         * selection sees the incoming one as current, then perform the swap.
         * Mask CLOCK so it can only charge the thread on the actual stack. */
        prcb->CurrentThread=next;
        if (old==prcb->IdleThread) {
            old->State=Standby;
            KiIdleSummary=0;
        } else {
            KiReadyThread(old);
        }
        prcb->CurrentThread=old;
        BOOLEAN apc_pending=KiSwapContext(next);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        if (apc_pending) {
            if (daif & (1u<<7)) {
                /* IRQ frame returns through ERET before this APC can run. */
                KiRequestSoftwareInterrupt(APC_LEVEL);
            } else {
                KeLowerIrql(APC_LEVEL);
                KiDeliverApc(KernelMode, NULL, NULL);
                old_irql=PASSIVE_LEVEL;
            }
        }
    } else if (prcb->NextThread && !prcb->DpcRoutineActive) {
        KiRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }
    KeLowerIrql(old_irql);
}
VOID KiArm64DispatchInterrupt(KIRQL interrupted)
{
    PKPRCB prcb=KeGetCurrentPrcb();
    require(initialized && prcb->CurrentThread && interrupted<DISPATCH_LEVEL &&
        KeGetCurrentIrql()==DISPATCH_LEVEL && !KeIsExecutingDpc(),17);
    prcb->DpcInterruptRequested=FALSE;
    if (prcb->DpcQueueDepth || prcb->TimerHand)
        KiRetireDpcList(prcb);
    require(KeGetCurrentIrql()==DISPATCH_LEVEL && !KeIsExecutingDpc(),16);
    if (prcb->QuantumEnd) {
        prcb->QuantumEnd=0;
        if (KiQuantumEnd())
            ++KiArm64QuantumSwitches;
    }
    /* NT_UP holds the dispatcher lock by staying at SYNCH_LEVEL. After
     * selection, the common path can suspend this retired IRQ frame. */
    KiUnlockDispatcherDatabase(interrupted);
}
__declspec(noreturn) VOID KiArm64StartThread(PKSYSTEM_ROUTINE system, PKSTART_ROUTINE start,
                                            PVOID parameter)
{
    require(initialized && KeGetCurrentThread()->State==Running &&
        KeGetCurrentThread()->Header.Type==ThreadObject && KeGetCurrentIrql()==SYNCH_LEVEL, 11);
    KeLowerIrql(APC_LEVEL);
    if (KeGetCurrentThread()->ApcState.KernelApcPending) KiDeliverApc(KernelMode,NULL,NULL);
    KeLowerIrql(PASSIVE_LEVEL);
    _enable();
    system(start,parameter);
    KeBugCheckEx((ULONG)STATUS_THREAD_IS_TERMINATING,12,(ULONG_PTR)KeGetCurrentThread(),0,0);
}
ULONG KiComputeTimerTableIndex(LARGE_INTEGER interval, LARGE_INTEGER current, PKTIMER timer)
{
    ULONG64 due = (ULONG64)current.QuadPart - (ULONG64)interval.QuadPart;
    timer->DueTime.QuadPart = due;
    return (ULONG)(KiArm64InterruptTicks(due) & (TIMER_TABLE_SIZE - 1));
}
