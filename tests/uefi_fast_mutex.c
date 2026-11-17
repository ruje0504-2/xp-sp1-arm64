/* Run the original AMD64 C fast-mutex paths with real NT waiters and GIC APCs. */
#include "ki.h"
#include "../base/ntos/ex/arm64/bootexec.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/mm/arm64/bootframes.h"
#include "../base/ntos/ps/arm64/bootexit.h"
#include "uefi_test.h"

static ETHREAD waiters[3];
static KAPC owner_apc;
static PKTHREAD owner_thread;
static volatile ULONG handoffs, apcs;
static ULONG unsafe_saved_irql;

static VOID observe_owner_apc(PKAPC apc, PKNORMAL_ROUTINE *routine,
                              PVOID *context, PVOID *a, PVOID *b)
{
    (VOID)routine; (VOID)context; (VOID)a; (VOID)b;
    CHECK(apc == &owner_apc && KeGetCurrentThread() == owner_thread &&
          KeGetCurrentIrql() == APC_LEVEL && ExpEnvironmentLock.Owner != owner_thread);
    ++apcs;
}

static VOID contend(PVOID argument)
{
    PETHREAD thread = argument;
    ULONG index = (ULONG)(thread - waiters);
    CHECK(index < 3 && KeGetCurrentThread() == &thread->Tcb);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    KIRQL entry_irql = PASSIVE_LEVEL;
    if (index == 1) {
        KIRQL previous;
        KeRaiseIrql(APC_LEVEL, &previous);
        CHECK(previous == PASSIVE_LEVEL);
        entry_irql = APC_LEVEL;
    }
    if (index == 2) KeEnterCriticalRegion();
    PFAST_MUTEX mutex = &ExpEnvironmentLock;
    LONG pending = mutex->Count;
    CHECK(!ExTryToAcquireFastMutex(mutex));
    CHECK(KeGetCurrentIrql() == entry_irql && mutex->Count == pending && mutex->Owner == owner_thread);

    if (index < 2) {
        ExAcquireFastMutex(mutex);
        CHECK(KeGetCurrentIrql() == APC_LEVEL && mutex->OldIrql == entry_irql);
    } else {
        ExAcquireFastMutexUnsafe(mutex);
        CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && mutex->OldIrql == unsafe_saved_irql);
    }
    CHECK(mutex->Owner == &thread->Tcb && handoffs == index);
    CHECK(mutex->Count == (index == 0 ? -1 : 0));
    ++handoffs;
    if (index < 2) {
        ExReleaseFastMutex(mutex);
        CHECK(KeGetCurrentIrql() == entry_irql);
        if (entry_irql != PASSIVE_LEVEL) KeLowerIrql(PASSIVE_LEVEL);
    } else {
        ExReleaseFastMutexUnsafe(mutex);
        CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && mutex->OldIrql == unsafe_saved_irql);
        KeLeaveCriticalRegion();
    }
    PsArm64ExitBootThread(thread, STATUS_SUCCESS);
}

static VOID start_waiter(PKSTART_ROUTINE routine, PVOID argument)
{
    routine(argument);
    CHECK(FALSE);
}

static VOID queue_waiter(ULONG index, PKPROCESS process, LONG expected)
{
    PETHREAD thread = &waiters[index];
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = KeInitializeThread(&thread->Tcb, NULL, start_waiter, contend, thread,
                                         NULL, NULL, process);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(status == STATUS_SUCCESS);
    KeSetPriorityThread(&thread->Tcb, 18 + index);
    KeReadyThread(&thread->Tcb);
    CHECK(thread->Tcb.State == Waiting && thread->Tcb.WaitReason == WrExecutive);
    CHECK(thread->Tcb.WaitBlockList->Object == &ExpEnvironmentLock.Event &&
          ExpEnvironmentLock.Count == expected && ExpEnvironmentLock.Owner == owner_thread);
}

VOID XpCheckFastMutex(PKPROCESS process)
{
    PFAST_MUTEX mutex = &ExpEnvironmentLock;
    owner_thread = KeGetCurrentThread();
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && mutex->Count == 1 &&
          !mutex->Owner && !mutex->Contention);
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG stacks = process->StackCount;
    ULONG reaped = ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed;
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);

    CHECK(ExTryToAcquireFastMutex(mutex));
    CHECK(KeGetCurrentIrql() == APC_LEVEL && mutex->Owner == owner_thread && !mutex->Count);
    ExReleaseFastMutex(mutex);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && mutex->Count == 1 && !mutex->Owner);

    ExAcquireFastMutex(mutex);
    CHECK(KeGetCurrentIrql() == APC_LEVEL && mutex->OldIrql == PASSIVE_LEVEL);
    KeInitializeApc(&owner_apc, owner_thread, OriginalApcEnvironment, observe_owner_apc,
                    NULL, NULL, KernelMode, NULL);
    CHECK(KeInsertQueueApc(&owner_apc, NULL, NULL, 0));
    CHECK(!apcs && owner_thread->ApcState.KernelApcPending);
    queue_waiter(0, process, -1);
    queue_waiter(1, process, -2);
    CHECK(!apcs && owner_thread->ApcState.KernelApcPending && mutex->Contention == 2);
    ExReleaseFastMutex(mutex);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    for (ULONG i = 0; i < 2; ++i)
        CHECK(KeWaitForSingleObject(&waiters[i].Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(apcs == 1 && !owner_thread->ApcState.KernelApcPending);
    CHECK(handoffs == 2 && mutex->Count == 1 && !mutex->Owner && mutex->Contention == 2);

    unsafe_saved_irql = mutex->OldIrql;
    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(mutex);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && mutex->OldIrql == unsafe_saved_irql);
    queue_waiter(2, process, -1);
    CHECK(mutex->Contention == 3);
    ExReleaseFastMutexUnsafe(mutex);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    KeLeaveCriticalRegion();
    CHECK(KeWaitForSingleObject(&waiters[2].Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(handoffs == 3 && mutex->Count == 1 && !mutex->Owner &&
          !mutex->Event.Header.SignalState && IsListEmpty(&mutex->Event.Header.WaitListHead));

    KEVENT park;
    LARGE_INTEGER grace;
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    grace.QuadPart = -1000000;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &grace) == STATUS_TIMEOUT);
    KiArm64DisableTimerDispatch();
    CHECK(process->StackCount == stacks && !PsReaperList);
    ULONG batches = ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed - reaped;
    CHECK(batches > 0 && batches <= 3);
    for (ULONG i = 0; i < sizeof(waiters) / sizeof(*waiters); ++i) {
        CHECK(waiters[i].Tcb.State == Terminated && waiters[i].ExitStatus == STATUS_SUCCESS);
        CHECK(!waiters[i].Tcb.InitialStack && !waiters[i].Tcb.StackBase && !waiters[i].ReaperLink);
    }
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Fast mutex evidence handoffs="); XpTestHex(handoffs);
    XpTestReport(" contention="); XpTestHex(mutex->Contention);
    XpTestReport(" deferred_apc="); XpTestHex(apcs);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP fast mutex contention, FIFO handoff, IRQL restore and APC deferral\n");
}
