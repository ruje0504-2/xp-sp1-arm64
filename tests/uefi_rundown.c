/* Two actual early ETHREAD exits and post-switch stack reaps. */
#include "ki.h"
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/hal/arm64/gicv2.h"
#include "../base/ntos/ps/arm64/bootexit.h"
#include "uefi_test.h"
#include "../base/ntos/mm/arm64/bootframes.h"

enum { ExitCount=2 };
static ETHREAD exiting[ExitCount];
static KEVENT timeout_event;
static KAPC exit_apcs[ExitCount];
static ULONG volatile apc_calls, starts;

VOID XpCheckStackFault(ULONG_PTR Address);

static VOID before_exit_apc(PKAPC apc, PKNORMAL_ROUTINE *routine, PVOID *context,
                            PVOID *arg1, PVOID *arg2)
{
    UNREFERENCED_PARAMETER(routine);
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arg1);
    UNREFERENCED_PARAMETER(arg2);
    CHECK(apc->Thread==KeGetCurrentThread() && KeGetCurrentIrql()==APC_LEVEL);
    ++apc_calls;
}
static VOID exiting_thread(PVOID parameter)
{
    PETHREAD thread=parameter;
    PKTHREAD tcb=&thread->Tcb;
    ULONG index=(ULONG)(thread-exiting);
    CHECK(index<ExitCount && tcb==KeGetCurrentThread());
    CHECK(KeGetCurrentIrql()==PASSIVE_LEVEL && !IsListEmpty(&tcb->Header.WaitListHead));
    ++starts;
    /* This APC is genuine pending work. The thread observes its delivery
     * before exiting. A later enqueue after exit must be rejected. */
    KeInitializeApc(&exit_apcs[index],tcb,OriginalApcEnvironment,before_exit_apc,
                    NULL,NULL,KernelMode,NULL);
    CHECK(KeInsertQueueApc(&exit_apcs[index],NULL,NULL,0));
    HAL_ARM64_IRQ_STATS hal;
    HalArm64GetIrqStats(&hal);
    ULONG64 limit=HalArm64ReadCounter()+hal.Frequency;
    while (apc_calls!=index+1) {
        CHECK(HalArm64ReadCounter()<limit);
        __asm__ volatile("yield" ::: "memory");
    }
    PsArm64ExitBootThread(thread,STATUS_CANCELLED);
}
static VOID start_exit(PKSTART_ROUTINE start, PVOID parameter)
{
    start(parameter);
    CHECK(FALSE);
}
static VOID check_one_exit(PMM_ARM64_BOOT_TABLES tables, PKPROCESS process,
                           PETHREAD thread)
{
    ULONG before_count=process->StackCount;
    MM_ARM64_BOOT_PFN_COUNTS before=MmArm64BootFrameCounts();
    ULONG before_work=ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    CHECK(KeInitializeThread(&thread->Tcb,NULL,start_exit,exiting_thread,
                             thread,NULL,NULL,process)==STATUS_SUCCESS);
    MM_ARM64_BOOT_STACK stack;
    PMM_ARM64_BOOT_TABLES owner_tables;
    CHECK(MmArm64LookupKernelStack(thread->Tcb.StackBase,&owner_tables,&stack)==STATUS_SUCCESS);
    MM_ARM64_BOOT_STACK_SLOT *slot=&tables->Stacks[stack.Index];
    CHECK(owner_tables==tables && slot->Owner==&thread->Tcb);
    PFN_NUMBER frames[MM_BOOT_STACK_DATA_PAGES];
    for (ULONG i=0;i<MM_BOOT_STACK_DATA_PAGES;++i) {
        frames[i]=slot->Frames[i];
        CHECK(MmArm64BootFrameOwnedBy(frames[i],slot));
    }
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");

    CHECK(process->StackCount==before_count+1);
    KeSetBasePriorityThread(&thread->Tcb,12-process->BasePriority);
    KeReadyThread(&thread->Tcb);
    CHECK(thread->Tcb.State==Ready && thread->Tcb.ContextSwitches==0);
    LARGE_INTEGER deadline;
    deadline.QuadPart=-1000000;
    CHECK(KeWaitForSingleObject(&thread->Tcb,Executive,KernelMode,FALSE,&deadline)==STATUS_SUCCESS);
    CHECK(thread->Tcb.State==Terminated && thread->Tcb.Header.SignalState==1);
    CHECK(thread->ExitStatus==STATUS_CANCELLED && thread->ExitTime.QuadPart>0);
    CHECK(!thread->Tcb.ApcQueueable && !thread->Tcb.ApcState.KernelApcInProgress);
    CHECK(!KeInsertQueueApc(&exit_apcs[thread-exiting],NULL,NULL,0));
    CHECK(IsListEmpty(&thread->Tcb.Header.WaitListHead) && process->StackCount==before_count);

    /* The awakened priority-16 joiner precedes the priority-15 worker.
     * The outgoing stack must remain mapped through the last switch. */
    CHECK(PsReaperActive && PsReaperList==thread);
    CHECK(tables->Stacks[stack.Index].Owner==&thread->Tcb && thread->Tcb.InitialStack!=NULL);
    CHECK(MmArm64QueryPhysicalAddress(stack.Limit)==(frames[0]<<PAGE_SHIFT));
    CHECK(MmArm64BootFrameCounts().Allocated==before.Allocated+MM_BOOT_STACK_DATA_PAGES);
    CHECK(KeWaitForSingleObject(&timeout_event,Executive,KernelMode,FALSE,&deadline)==STATUS_TIMEOUT);
    CHECK(!PsReaperActive && PsReaperList==NULL && thread->ReaperLink==NULL);
    CHECK(ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed==before_work+1);
    CHECK(!thread->Tcb.InitialStack && !thread->Tcb.StackBase && !thread->Tcb.KernelStack);
    CHECK(tables->Stacks[stack.Index].Owner==NULL && !RtlCheckBit(&tables->StackBitmap,stack.Index));
    CHECK(MmArm64QueryPhysicalAddress(stack.Limit)==~0ULL);
    XpCheckStackFault(stack.Limit);
    CHECK(MmArm64BootFrameCounts().Free==before.Free);
    CHECK(MmArm64BootFrameCounts().Allocated==before.Allocated);

    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    for (ULONG i=0;i<MM_BOOT_STACK_DATA_PAGES;++i)
        CHECK(!slot->Frames[i] && !MmArm64BootFrameOwnedBy(frames[i],slot));
    PVOID reused=MmCreateKernelStack(FALSE,0);
    CHECK(reused==(PVOID)stack.Top && tables->Stacks[stack.Index].Generation!=stack.Generation);
    CHECK(MmArm64FreeBootStack(tables,&stack)==STATUS_INVALID_HANDLE);
    for (ULONG i=0;i<MM_BOOT_STACK_DATA_PAGES;++i) {
        CHECK(MmArm64BootFrameOwnedBy(slot->Frames[i],slot));
        CHECK(MmArm64QueryPhysicalAddress(stack.Limit + i*PAGE_SIZE)==(slot->Frames[i]<<PAGE_SHIFT));
    }
    const volatile ULONG64 *memory=(PVOID)stack.Limit;
    for (ULONG i=0;i<KERNEL_STACK_SIZE/sizeof(*memory);++i) CHECK(memory[i]==0);
    XpCheckStackFault(stack.Limit-PAGE_SIZE);
    MmDeleteKernelStack(reused,FALSE);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");

    for (PLIST_ENTRY entry=process->ThreadListHead.Flink;entry!=&process->ThreadListHead;entry=entry->Flink)
        CHECK(entry!=&thread->Tcb.ThreadListEntry);
}
VOID XpCheckThreadExit(PXP_ARM64_BOOT_BLOCK block)
{
    PKPROCESS process=&block->InitialProcess.Pcb;
    ULONG initial_stacks=process->StackCount;
    KeInitializeEvent(&timeout_event,SynchronizationEvent,FALSE);
    CHECK(PsArm64StartBootReaper()==STATUS_SUCCESS);
    CHECK(PsArm64StartBootReaper()==STATUS_INVALID_DEVICE_STATE);
    CHECK(process->StackCount==initial_stacks+1);
    CHECK(KiArm64EnableTimerDispatch()==STATUS_SUCCESS);
    for (ULONG i=0;i<ExitCount;++i)
        check_one_exit(&block->Tables,process,&exiting[i]);
    KiArm64DisableTimerDispatch();
    CHECK(starts==2 && apc_calls==2 && process->StackCount==initial_stacks+1);
    CHECK(KiProcessOutSwapListHead.Next==NULL && process->State==ProcessInMemory);
    XpTestReport("Rundown evidence exits=");XpTestHex(starts);
    XpTestReport(" reaper_work=");XpTestHex(ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed);
    XpTestReport(" pre_exit_apcs=");XpTestHex(apc_calls);
    XpTestReport(" resident_stacks=");XpTestHex(process->StackCount);XpTestReport("\n");
    XpTestReport("PASS: original XP thread termination, join, deferred stack rundown and guarded reuse\n");
}
