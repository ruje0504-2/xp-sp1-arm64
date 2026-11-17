/* Original XP ERESOURCE with two real threads, an event and a semaphore. */
#include "ki.h"
#include "../base/ntos/ex/arm64/bootexec.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/mm/arm64/bootframes.h"
#include "../base/ntos/ps/arm64/bootexit.h"
#include "uefi_test.h"

extern LIST_ENTRY ExpSystemResourcesList;
extern ERESOURCE HandleTableListLock;
extern LARGE_INTEGER ExpTimeout;
static ERESOURCE resource;
static ETHREAD contender;
static PKTHREAD controller;
static KEVENT acquired, convert, released, park;
static KTIMER handoff;
static KDPC handoff_dpc;
static ULONG shared_handoffs, exclusive_handoffs;

static VOID grant_conversion(PKDPC dpc, PVOID context, PVOID a, PVOID b)
{
    (VOID)dpc; (VOID)context; (VOID)a; (VOID)b;
    /* Require a real dispatcher wait, not an already-signaled semaphore. */
    if (controller->State != Waiting || resource.NumberOfSharedWaiters != 1 ||
        controller->WaitBlockList->Object != resource.SharedWaiters)
        return;
    CHECK(KeIsExecutingDpc() && KeGetCurrentIrql() == DISPATCH_LEVEL);
    ++shared_handoffs;
    KeCancelTimer(&handoff);
    CHECK(KeSetEvent(&convert, 0, FALSE) == 0);
}

static VOID contend(PVOID argument)
{
    CHECK(argument == &contender && KeGetCurrentThread() == &contender.Tcb);
    KeEnterCriticalRegion();
    CHECK(!ExAcquireResourceExclusiveLite(&resource, FALSE));
    CHECK(ExAcquireResourceExclusiveLite(&resource, TRUE));
    CHECK(ExIsResourceAcquiredExclusiveLite(&resource));
    ++exclusive_handoffs;
    CHECK(KeSetEvent(&acquired, 0, FALSE) == 0);
    CHECK(KeWaitForSingleObject(&convert, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(ExGetSharedWaiterCount(&resource) == 1 && ExIsResourceAcquiredExclusiveLite(&resource));
    ExConvertExclusiveToSharedLite(&resource);
    CHECK(ExIsResourceAcquiredSharedLite(&resource) == 1 && !ExIsResourceAcquiredExclusiveLite(&resource));
    CHECK(KeWaitForSingleObject(&released, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    ExReleaseResourceLite(&resource);
    KeLeaveCriticalRegion();
    PsArm64ExitBootThread(&contender, STATUS_SUCCESS);
}

static VOID start_contender(PKSTART_ROUTINE routine, PVOID argument)
{
    routine(argument);
    CHECK(FALSE);
}

VOID XpCheckExecutiveResource(PKPROCESS process)
{
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && ExpTimeout.QuadPart == -40000000);
    CHECK(ExpEnvironmentLock.Count == 1 && !ExpEnvironmentLock.Owner &&
          !ExpEnvironmentLock.Contention && !ExpEnvironmentLock.Event.Header.SignalState &&
          ExpEnvironmentLock.Event.Header.Type == SynchronizationEvent);
    PLIST_ENTRY resident_tail = ExpSystemResourcesList.Blink;
    CHECK(ExpSystemResourcesList.Flink == &ExpKeyManipLock.SystemResourcesList &&
          ExpKeyManipLock.SystemResourcesList.Flink == &HandleTableListLock.SystemResourcesList &&
          HandleTableListLock.SystemResourcesList.Blink == &ExpKeyManipLock.SystemResourcesList &&
          !HandleTableListLock.ActiveCount && !HandleTableListLock.SpinLock &&
          !ExpKeyManipLock.ActiveCount && !ExpKeyManipLock.SpinLock);

    CHECK(ExInitializeResourceLite(&resource) == STATUS_SUCCESS);
    CHECK(ExpSystemResourcesList.Blink == &resource.SystemResourcesList);
    KeInitializeEvent(&acquired, SynchronizationEvent, FALSE);
    KeInitializeEvent(&convert, SynchronizationEvent, FALSE);
    KeInitializeEvent(&released, SynchronizationEvent, FALSE);
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    KeInitializeTimer(&handoff);
    KeInitializeDpc(&handoff_dpc, grant_conversion, NULL);
    controller = KeGetCurrentThread();
    ULONG original_stack_count = process->StackCount;
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG reaped_before = ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed;
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);

    KeEnterCriticalRegion();
    CHECK(ExAcquireResourceSharedLite(&resource, TRUE));
    CHECK(ExAcquireResourceSharedLite(&resource, TRUE));
    CHECK(ExIsResourceAcquiredSharedLite(&resource) == 2 && resource.ActiveCount == 1);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = KeInitializeThread(&contender.Tcb, NULL, start_contender, contend,
                                         &contender, NULL, NULL, process);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(status == STATUS_SUCCESS);
    KeSetPriorityThread(&contender.Tcb, 18);
    KeReadyThread(&contender.Tcb);
    CHECK(contender.Tcb.State == Waiting && ExGetExclusiveWaiterCount(&resource) == 1);
    CHECK(resource.ExclusiveWaiters && contender.Tcb.WaitBlockList->Object == resource.ExclusiveWaiters);
    ExReleaseResourceLite(&resource);
    CHECK(ExIsResourceAcquiredSharedLite(&resource) == 1 && ExGetExclusiveWaiterCount(&resource) == 1);
    ExReleaseResourceLite(&resource);
    CHECK(KeWaitForSingleObject(&acquired, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(exclusive_handoffs == 1 && !ExGetExclusiveWaiterCount(&resource) && resource.ActiveCount == 1);

    LARGE_INTEGER due;
    due.QuadPart = -100000; /* Check for the real waiter every 10 ms. */
    CHECK(!KeSetTimerEx(&handoff, due, 10, &handoff_dpc));
    CHECK(ExAcquireResourceSharedLite(&resource, TRUE));
    CHECK(shared_handoffs == 1 && !handoff.Header.Inserted && !ExGetSharedWaiterCount(&resource));
    CHECK(resource.SharedWaiters && resource.ActiveCount == 2 &&
          ExIsResourceAcquiredSharedLite(&resource) == 1 && !ExIsResourceAcquiredExclusiveLite(&resource));
    ExReleaseResourceLite(&resource);
    KeLeaveCriticalRegion();
    CHECK(resource.ActiveCount == 1 && resource.ContentionCount == 2);
    CHECK(KeSetEvent(&released, 0, FALSE) == 0);
    CHECK(KeWaitForSingleObject(&contender.Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(contender.ExitStatus == STATUS_SUCCESS && !resource.ActiveCount);
    due.QuadPart = -1000000;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &due) == STATUS_TIMEOUT);
    CHECK(!contender.Tcb.InitialStack && !PsReaperList && process->StackCount == original_stack_count);
    CHECK(ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed == reaped_before + 1);
    CHECK(ExDeleteResourceLite(&resource) == STATUS_SUCCESS);
    CHECK(ExpSystemResourcesList.Flink == &ExpKeyManipLock.SystemResourcesList &&
          ExpSystemResourcesList.Blink == resident_tail);
    KiArm64DisableTimerDispatch();
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(after.Free == before.Free && after.Allocated == before.Allocated);

    XpTestReport("Executive resource evidence exclusive="); XpTestHex(exclusive_handoffs);
    XpTestReport(" shared="); XpTestHex(shared_handoffs);
    XpTestReport(" contention="); XpTestHex(resource.ContentionCount);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP ERESOURCE initialization, blocking exclusive/event and shared/semaphore handoffs\n");
}
