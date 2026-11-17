/* Real original constructors, APC suspension, object wait and ready selection.
 * Threads used by the wait and quantum controls remain live. */
#include "ki.h"
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/hal/arm64/gicv2.h"
#include "uefi_test.h"
VOID XpCheckThreadExit(PXP_ARM64_BOOT_BLOCK Block);
extern VOID XpEnableScopeExceptions(PXP_ARM64_BOOT_BLOCK Block, BOOLEAN Active);
VOID XpCheckExecutivePool(VOID);
VOID XpCheckExecutivePagedPool(PKPROCESS Process);
VOID XpCheckLookasides(VOID);
VOID XpCheckExecutiveHandles(PKPROCESS Process);
VOID XpCheckBootObjectState(VOID);
VOID XpCheckBootObjectTypes(VOID);
VOID XpCheckBootNamespace(VOID);
VOID XpCheckBootObjectPaths(VOID);
VOID XpCheckBootObjectInsertion(VOID);
VOID XpCheckBootObjectClose(PKPROCESS Process);
VOID XpCheckBootSecurityGlobals(VOID);
VOID XpCheckBootLogonSessions(VOID);
VOID XpCheckBootSystemToken(VOID);
VOID XpCheckBootProcessObjects(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckBootSystemProcess(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckBootSystemThread(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckBootPhase1(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckBootMdl(VOID);
VOID XpCheckPagedPool(VOID);
VOID XpCheckExecutiveResource(PKPROCESS Process);
VOID XpCheckFastMutex(PKPROCESS Process);
static PEPROCESS process;
static KTHREAD control, worker_thread, rejected;
static PKTHREAD idle_thread;
static KEVENT gate, park, idle_started;
extern volatile ULONG XpSoftwareRaiseCount;
static KSEMAPHORE token;
static volatile ULONG stage;
static PXP_ARM64_BOOT_BLOCK boot_block;
static ULONG parameter;
static __declspec(noreturn) VOID system_start(PKSTART_ROUTINE start, PVOID argument)
{
    CHECK(KeGetCurrentIrql()==PASSIVE_LEVEL && start!=NULL);
    start(argument);
    CHECK(FALSE);
    for (;;) __asm__ volatile("wfe");
}
static VOID idle(PVOID argument)
{
    UNREFERENCED_PARAMETER(argument);
    CHECK(KeGetCurrentThread()==idle_thread && KiIdleSummary==1);
    /* A one-time test notification proves the idle entry really ran and
     * the waiting control is awakened by the original event algorithm. */
    CHECK(KeSetEvent(&idle_started,0,FALSE)==0);
    KiArm64IdleLoop();
}
static VOID worker(PVOID argument)
{
    CHECK(argument==&parameter && KeGetCurrentThread()==&worker_thread);
    volatile ULONG cookie=0x51a1;
    stage=1;
    LARGE_INTEGER deadline; deadline.QuadPart=-100000000;
    CHECK(KeWaitForSingleObject(&gate, Executive, KernelMode, FALSE, &deadline)==STATUS_SUCCESS);
    CHECK(cookie==0x51a1 && KeGetCurrentThread()==&worker_thread);
    CHECK(worker_thread.SuspendCount==0 && !worker_thread.ApcState.KernelApcInProgress);
    CHECK(KeWaitForSingleObject(&token, Executive, KernelMode, FALSE, NULL)==STATUS_SUCCESS);
    CHECK(token.Header.SignalState==0);
    LARGE_INTEGER zero; zero.QuadPart=0;
    CHECK(KeWaitForSingleObject(&token, Executive, KernelMode, FALSE, &zero)==STATUS_TIMEOUT);
    LARGE_INTEGER past; past.QuadPart=1;
    CHECK(KeWaitForSingleObject(&token, Executive, KernelMode, FALSE, &past)==STATUS_TIMEOUT);
    CHECK(!worker_thread.Timer.Header.Inserted && IsListEmpty(&token.Header.WaitListHead));
    CHECK(KeReleaseSemaphore(&token, 0, 1, FALSE)==0);
    stage=2;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, NULL)==STATUS_SUCCESS);
    CHECK(FALSE); /* Remains genuinely Waiting when the control ends. */
}
static KEVENT never_signaled, periodic_done;
static KTIMER delayed_timer, periodic_timer, restart_timer;
static KDPC delayed_dpc, periodic_dpc, slow_dpc, removed_dpc;
static volatile ULONG delayed_calls, periodic_calls, slow_calls, removed_calls;
static ULONG64 nested_irqs, delayed_ticks;
static ULONG masked_runtime_ticks;
static VOID timer_callback(PKDPC dpc,PVOID context,PVOID low,PVOID high)
{
    CHECK(KeGetCurrentIrql()==DISPATCH_LEVEL && KeIsExecutingDpc());
    LARGE_INTEGER now; KeQuerySystemTime(&now);
    ULONG64 timestamp=((ULONG64)PtrToUlong(high)<<32)|PtrToUlong(low);
    CHECK(timestamp<=(ULONG64)now.QuadPart && timestamp!=0);
    if (dpc==&delayed_dpc) {
        CHECK(context==&delayed_timer); ++delayed_calls;
    } else {
        CHECK(dpc==&periodic_dpc && context==&periodic_timer);
        if (++periodic_calls==3) KeSetEvent(&periodic_done,0,FALSE);
    }
}
static VOID slow_callback(PKDPC dpc,PVOID context,PVOID a,PVOID b)
{
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(b);
    CHECK(dpc==&slow_dpc && KeGetCurrentIrql()==DISPATCH_LEVEL && KeIsExecutingDpc());
    HAL_ARM64_IRQ_STATS before,after;
    HalArm64GetIrqStats(&before);
    ULONG before_dpc=KeGetCurrentPrcb()->DpcTime;
    ULONG before_thread=control.KernelTime;
    ULONG64 end=HalArm64ReadCounter()+before.Frequency/200;
    while ((LONG64)(HalArm64ReadCounter()-end)<0) __asm__ volatile("yield");
    HalArm64GetIrqStats(&after);
    CHECK(after.Interrupts>before.Interrupts && KeIsExecutingDpc());
    CHECK(KeGetCurrentPrcb()->DpcTime>before_dpc && control.KernelTime==before_thread);
    nested_irqs=after.Interrupts-before.Interrupts;
    ++slow_calls;
}
static VOID removed_callback(PKDPC dpc,PVOID context,PVOID a,PVOID b)
{
    UNREFERENCED_PARAMETER(dpc);UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(b);
    ++removed_calls;
}
static ULONG64 test_deadline(VOID)
{
    HAL_ARM64_IRQ_STATS hal;
    HalArm64GetIrqStats(&hal);
    return HalArm64ReadCounter()+hal.Frequency;
}
static VOID wait_callback(volatile ULONG *calls)
{
    ULONG64 until=test_deadline();
    while (*calls==0) {
        CHECK(HalArm64ReadCounter()<until);
        __asm__ volatile("yield");
    }
}
static VOID drain_software_interrupts(VOID)
{
    ULONG64 until=test_deadline();
    while (HalArm64SoftwareIrqPending()) {
        CHECK(HalArm64ReadCounter()<until);
        __asm__ volatile("yield");
    }
    CHECK(!HalArm64SoftwareIrqActive());
}

static VOID check_timer_service(VOID)
{
    CHECK(KeTickCount.QuadPart==0);
    KeInitializeEvent(&never_signaled,SynchronizationEvent,FALSE);
    KeInitializeEvent(&periodic_done,SynchronizationEvent,FALSE);
    KeInitializeTimer(&delayed_timer);KeInitializeTimer(&periodic_timer);KeInitializeTimer(&restart_timer);
    KeInitializeDpc(&delayed_dpc,timer_callback,&delayed_timer);
    KeInitializeDpc(&periodic_dpc,timer_callback,&periodic_timer);
    KeInitializeDpc(&slow_dpc,slow_callback,NULL);
    KeInitializeDpc(&removed_dpc,removed_callback,NULL);
    CHECK(KiArm64EnableTimerDispatch()==STATUS_SUCCESS);
    KIRQL passive;
    KeRaiseIrql(CLOCK_LEVEL,&passive);
    PKPRCB prcb=KeGetCurrentPrcb();
    ULONG before_interrupt=prcb->InterruptTime, before_thread=control.KernelTime;
    ULONG64 start_tick=KeTickCount.QuadPart;
    HAL_ARM64_IRQ_STATS hal;
    HalArm64GetIrqStats(&hal);
    ULONG64 delay=HalArm64ReadCounter()+hal.Frequency/200;
    while (HalArm64ReadCounter()<delay) __asm__ volatile("yield");
    CHECK((ULONG64)KeTickCount.QuadPart==start_tick);
    KeLowerIrql(CLOCK_LEVEL-1);
    ULONG64 until=test_deadline();
    while (prcb->InterruptTime==before_interrupt) {
        CHECK(HalArm64ReadCounter()<until);
        __asm__ volatile("yield" ::: "memory");
    }
    masked_runtime_ticks=prcb->InterruptTime-before_interrupt;
    CHECK(masked_runtime_ticks>=5 && control.KernelTime==before_thread);
    KeLowerIrql(passive);
    ULONG64 idle_before=idle_thread->ContextSwitches;
    ULONG64 expected=KeQueryInterruptTime()+50000;
    LARGE_INTEGER relative;relative.QuadPart=-50000;
    CHECK(KeWaitForSingleObject(&never_signaled,Executive,KernelMode,FALSE,&relative)==STATUS_TIMEOUT);
    CHECK(KeQueryInterruptTime()>=expected && idle_thread->ContextSwitches>idle_before);
    CHECK(!control.Timer.Header.Inserted && IsListEmpty(&never_signaled.Header.WaitListHead));
    LARGE_INTEGER absolute;KeQuerySystemTime(&absolute);absolute.QuadPart+=30000;
    CHECK(KeWaitForSingleObject(&never_signaled,Executive,KernelMode,FALSE,&absolute)==STATUS_TIMEOUT);
    LARGE_INTEGER after;KeQuerySystemTime(&after);CHECK(after.QuadPart>=absolute.QuadPart);
    CHECK(!control.Timer.Header.Inserted && IsListEmpty(&never_signaled.Header.WaitListHead));
    /* Permit actual CLOCK_LEVEL IRQs while preventing DPC-level consumption
     * for more than one full 256-bucket revolution. No time or signal is injected. */
    KIRQL old;KeRaiseIrql(DISPATCH_LEVEL,&old);
    relative.QuadPart=-30000;
    CHECK(!KeSetTimerEx(&delayed_timer,relative,0,&delayed_dpc));
    ULONG64 tick_before=KeTickCount.QuadPart;
    HalArm64GetIrqStats(&hal);
    ULONG64 end=HalArm64ReadCounter()+hal.Frequency*270/1000;
    while ((LONG64)(HalArm64ReadCounter()-end)<0) __asm__ volatile("yield");
    delayed_ticks=(ULONG64)KeTickCount.QuadPart-tick_before;
    CHECK(delayed_ticks>=TIMER_TABLE_SIZE && delayed_calls==0);
    CHECK(delayed_timer.Header.Inserted && delayed_timer.DueTime.QuadPart<=KeQueryInterruptTime());
    CHECK(KeGetCurrentPrcb()->DpcQueueDepth!=0);
    KeLowerIrql(old);
    wait_callback(&delayed_calls);
    CHECK(delayed_calls==1 && !delayed_timer.Header.Inserted && delayed_timer.Header.SignalState==1);
    CHECK(!KeCancelTimer(&delayed_timer));
    /* Original DPC duplicate suppression and cancellation, then a real DPC
     * kept active long enough for nested CLOCK IRQ delivery. */
    KeRaiseIrql(DISPATCH_LEVEL,&old);
    CHECK(KeInsertQueueDpc(&slow_dpc,NULL,NULL));
    CHECK(!KeInsertQueueDpc(&slow_dpc,NULL,NULL));
    CHECK(KeInsertQueueDpc(&removed_dpc,NULL,NULL));
    CHECK(KeRemoveQueueDpc(&removed_dpc));
    CHECK(!KeRemoveQueueDpc(&removed_dpc));
    KeLowerIrql(old);
    wait_callback(&slow_calls);
    CHECK(slow_calls==1 && removed_calls==0 && slow_dpc.Lock==NULL && removed_dpc.Lock==NULL);
    relative.QuadPart=-30000;
    CHECK(!KeSetTimerEx(&periodic_timer,relative,3,&periodic_dpc));
    LARGE_INTEGER watchdog;watchdog.QuadPart=-1000000;
    CHECK(KeWaitForSingleObject(&periodic_done,Executive,KernelMode,FALSE,&watchdog)==STATUS_SUCCESS);
    CHECK(periodic_calls>=3 && KeCancelTimer(&periodic_timer));
    ULONG canceled_count=periodic_calls;
    relative.QuadPart=-60000;
    CHECK(KeWaitForSingleObject(&never_signaled,Executive,KernelMode,FALSE,&relative)==STATUS_TIMEOUT);
    CHECK(periodic_calls==canceled_count && !periodic_timer.Header.Inserted);
    relative.QuadPart=-500000;
    CHECK(!KeSetTimerEx(&restart_timer,relative,0,NULL));
    KiArm64DisableTimerDispatch();
    CHECK(restart_timer.Header.Inserted);
    ULONG64 stopped_tick=KeTickCount.QuadPart;
    KI_ARM64_TIMER_DISPATCH_STATS stopped_stats,restarted_stats;
    KiArm64GetTimerDispatchStats(&stopped_stats);
    HalArm64GetIrqStats(&hal);
    end=HalArm64ReadCounter()+hal.Frequency*60/1000;
    while ((LONG64)(HalArm64ReadCounter()-end)<0) __asm__ volatile("yield");
    CHECK((ULONG64)KeTickCount.QuadPart==stopped_tick && restart_timer.Header.Inserted);
    CHECK(KiArm64EnableTimerDispatch()==STATUS_SUCCESS);
    KiArm64GetTimerDispatchStats(&restarted_stats);
    CHECK(restarted_stats.RuntimeTicks-stopped_stats.RuntimeTicks <
        restarted_stats.Ticks-stopped_tick);
    CHECK(KeWaitForSingleObject(&restart_timer,Executive,KernelMode,FALSE,&watchdog)==STATUS_SUCCESS);
    CHECK(!restart_timer.Header.Inserted && KeQueryInterruptTime()>=restart_timer.DueTime.QuadPart);
    KiArm64DisableTimerDispatch();
    drain_software_interrupts();
    KI_ARM64_TIMER_DISPATCH_STATS stats;KiArm64GetTimerDispatchStats(&stats);
    CHECK(stats.Interrupts>0 && stats.QueueInsertions>0 && stats.Ticks>stats.StartTicks);
    CHECK(prcb->KernelTime==stats.RuntimeTicks);
    CHECK(prcb->KernelTime==process->Pcb.KernelTime+prcb->DpcTime+prcb->InterruptTime);
    CHECK(process->Pcb.KernelTime==control.KernelTime+idle_thread->KernelTime+worker_thread.KernelTime);
    CHECK((ULONG64)KeTickCount.QuadPart==KeQueryInterruptTime()/10000);
    CHECK(!HalArm64TimerPendingOrActive() && !KeIsExecutingDpc());
    CHECK(KeGetCurrentPrcb()->DpcQueueDepth==0 && IsListEmpty(&KeGetCurrentPrcb()->DpcListHead));
    for (ULONG i=0;i<TIMER_TABLE_SIZE;++i) CHECK(IsListEmpty(&KiTimerTableListHead[i]));
    CHECK(KeGetCurrentThread()==&control && control.State==Running && KeGetCurrentIrql()==PASSIVE_LEVEL);
    XpTestReport("Timer wait evidence irq=");XpTestHex(stats.Interrupts);
    XpTestReport(" queued=");XpTestHex(stats.QueueInsertions);
    XpTestReport(" ticks=");XpTestHex(stats.Ticks-stats.StartTicks);
    XpTestReport(" delayed_ticks=");XpTestHex(delayed_ticks);
    XpTestReport(" nested_irq=");XpTestHex(nested_irqs);
    XpTestReport(" periodic_calls=");XpTestHex(periodic_calls);
    XpTestReport(" switches=");XpTestHex(KiArm64SchedulerSwitches);
    XpTestReport(" idle_switches=");XpTestHex(idle_thread->ContextSwitches);XpTestReport("\n");
    XpTestReport("PASS: actual IRQ timer expiry, original DPC retirement, idle timeout wake, table wrap and periodic cancellation\n");
}
static KAPC self_apc;
static KDPC self_dpc;
static volatile ULONG apc_calls, dispatch_calls;
static VOID sgi_apc(PKAPC apc, PKNORMAL_ROUTINE *routine, PVOID *context,
                    PVOID *a, PVOID *b)
{
    UNREFERENCED_PARAMETER(routine);UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(b);
    CHECK(apc==&self_apc && KeGetCurrentThread()==&control);
    CHECK(KeGetCurrentIrql()==APC_LEVEL && !HalArm64SoftwareIrqActive());
    ++apc_calls;
}
static VOID sgi_dpc(PKDPC dpc, PVOID context, PVOID a, PVOID b)
{
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(b);
    CHECK(dpc==&self_dpc && KeGetCurrentThread()==&control);
    CHECK(KeGetCurrentIrql()==DISPATCH_LEVEL && KeIsExecutingDpc() && !HalArm64SoftwareIrqActive());
    ++dispatch_calls;
}
static VOID check_software_interrupts(VOID)
{
    HAL_ARM64_IRQ_STATS before,after;
    HalArm64GetIrqStats(&before);
    CHECK(!before.TimerActive && !HalArm64SoftwareIrqPending());
    KeInitializeDpc(&self_dpc,sgi_dpc,NULL);
    KeInitializeApc(&self_apc,&control,OriginalApcEnvironment,sgi_apc,NULL,NULL,KernelMode,NULL);
    KIRQL passive=KeRaiseIrqlToDpcLevel();
    CHECK(KeInsertQueueApc(&self_apc,NULL,NULL,0));
    CHECK(KeInsertQueueDpc(&self_dpc,NULL,NULL));
    CHECK(HalArm64SoftwareIrqPending()==((1u<<APC_LEVEL)|(1u<<DISPATCH_LEVEL)));
    CHECK(!apc_calls && !dispatch_calls && !HalArm64SoftwareIrqActive());

    KeLowerIrql(APC_LEVEL);
    wait_callback(&dispatch_calls);
    CHECK(!apc_calls && HalArm64SoftwareIrqPending()==(1u<<APC_LEVEL));
    KeLowerIrql(passive);
    wait_callback(&apc_calls);
    drain_software_interrupts();
    HalArm64GetIrqStats(&after);
    CHECK(after.Interrupts==before.Interrupts && !after.TimerActive);
    CHECK(after.ApcInterrupts==before.ApcInterrupts+1 &&
          after.DispatchInterrupts==before.DispatchInterrupts+1);
    CHECK(apc_calls==1 && dispatch_calls==1 && !self_apc.Inserted && !self_dpc.Lock);
}

static KTHREAD spin_peer;
static KEVENT peer_parked;
static volatile ULONG control_spins, peer_spins, stop_spin;
static ULONG64 spin_deadline;
ULONG XpSpinQuantum(volatile ULONG *counter, const volatile ULONG *done,
                    ULONG target, ULONG64 deadline, ULONG seed);
static VOID spin_thread(PVOID argument)
{
    CHECK(argument==&spin_peer && KeGetCurrentThread()==&spin_peer);
    CHECK(XpSpinQuantum(&peer_spins,&stop_spin,1,spin_deadline,2)==1);
    CHECK(KeGetCurrentIrql()==PASSIVE_LEVEL && KeGetCurrentThread()==&spin_peer);
    KeSetEvent(&peer_parked,0,TRUE);
    KeWaitForSingleObject(&park,Executive,KernelMode,FALSE,NULL);
    CHECK(FALSE);
}
static VOID check_stack_owner(PKTHREAD thread)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    PMM_ARM64_BOOT_TABLES tables;
    MM_ARM64_BOOT_STACK stack;
    CHECK(MmArm64LookupKernelStack(thread->StackBase,&tables,&stack)==STATUS_SUCCESS);
    CHECK(tables->Stacks[stack.Index].Owner==thread && thread->StackLimit==(PVOID)stack.Limit);
    CHECK((ULONG_PTR)thread->KernelStack>=stack.Limit && (ULONG_PTR)thread->KernelStack<stack.Top);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
static VOID check_quantum(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    CHECK(KeInitializeThread(&spin_peer,NULL,system_start,spin_thread,&spin_peer,NULL,NULL,&process->Pcb)==STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    KeInitializeEvent(&peer_parked,SynchronizationEvent,FALSE);
    CHECK(KeSetPriorityThread(&spin_peer,control.Priority)==0);
    CHECK(spin_peer.Priority==control.Priority && !process->Pcb.DisableQuantum);
    KeReadyThread(&spin_peer);
    CHECK(spin_peer.State==Ready && spin_peer.ContextSwitches==0 && KeGetCurrentPrcb()->NextThread==NULL);
    check_stack_owner(&spin_peer);

    HAL_ARM64_IRQ_STATS irq_before,irq_after;
    KI_ARM64_TIMER_DISPATCH_STATS timer_before,timer_after;
    HalArm64GetIrqStats(&irq_before);
    KiArm64GetTimerDispatchStats(&timer_before);
    ULONG before_control=control.ContextSwitches;
    ULONG before_kernel=control.KernelTime, before_process=process->Pcb.KernelTime;
    ULONG64 before_quantum=KiArm64QuantumSwitches;
    spin_deadline=test_deadline();
    CHECK(KiArm64EnableTimerDispatch()==STATUS_SUCCESS);
    CHECK(XpSpinQuantum(&control_spins,&spin_peer.ContextSwitches,4,spin_deadline,1)==1);
    _disable();
    CHECK(KeGetCurrentThread()==&control && control.State==Running && KeGetCurrentIrql()==PASSIVE_LEVEL);
    CHECK(control.ContextSwitches>=before_control+4 && spin_peer.ContextSwitches>=4);
    CHECK(KiArm64QuantumSwitches-before_quantum>=8 && control_spins!=0 && peer_spins!=0);
    CHECK(spin_peer.State==Ready && !spin_peer.WaitBlockList && !control.Timer.Header.Inserted);
    CHECK(KiReadySummary==(1u<<control.Priority) && !KeGetCurrentPrcb()->NextThread);
    CHECK(KiDispatcherReadyListHead[control.Priority].Flink==&spin_peer.WaitListEntry &&
          KiDispatcherReadyListHead[control.Priority].Blink==&spin_peer.WaitListEntry);
    CHECK(control.KernelTime>before_kernel && spin_peer.KernelTime>0);
    CHECK(process->Pcb.KernelTime-before_process==control.KernelTime-before_kernel+spin_peer.KernelTime);
    CHECK(spin_peer.Quantum==process->Pcb.ThreadQuantum);
    check_stack_owner(&control);
    check_stack_owner(&spin_peer);
    HalArm64GetIrqStats(&irq_after);
    KiArm64GetTimerDispatchStats(&timer_after);
    CHECK(irq_after.Interrupts>irq_before.Interrupts && irq_after.DispatchInterrupts>irq_before.DispatchInterrupts);
    CHECK(timer_after.RuntimeTicks>timer_before.RuntimeTicks);

    XpTestReport("Quantum evidence clock_irq=");XpTestHex(irq_after.Interrupts-irq_before.Interrupts);
    XpTestReport(" dispatch_sgi=");XpTestHex(irq_after.DispatchInterrupts-irq_before.DispatchInterrupts);
    XpTestReport(" quantum_switches=");XpTestHex(KiArm64QuantumSwitches-before_quantum);
    XpTestReport(" control_entries=");XpTestHex(control.ContextSwitches-before_control);
    XpTestReport(" peer_entries=");XpTestHex(spin_peer.ContextSwitches);
    XpTestReport(" control_ticks=");XpTestHex(control.KernelTime-before_kernel);
    XpTestReport(" peer_ticks=");XpTestHex(spin_peer.KernelTime);
    XpTestReport(" sampled_ticks=");XpTestHex(timer_after.RuntimeTicks-timer_before.RuntimeTicks);
    XpTestReport(" masked_runtime_ticks=");XpTestHex(masked_runtime_ticks);
    XpTestReport(" independent_apc_sgi=");XpTestHex(apc_calls);
    XpTestReport(" independent_dispatch_sgi=");XpTestHex(dispatch_calls);XpTestReport("\n");
    /* Let the peer leave its spin only after measuring non-cooperative turns. */
    __atomic_store_n(&stop_spin,1,__ATOMIC_RELEASE);
    LARGE_INTEGER relative;relative.QuadPart=-5000000;
    _enable();
    CHECK(KeWaitForSingleObject(&peer_parked,Executive,KernelMode,FALSE,&relative)==STATUS_SUCCESS);
    KiArm64DisableTimerDispatch();
    drain_software_interrupts();
    CHECK(spin_peer.State==Waiting && spin_peer.WaitBlockList->Object==&park);
    XpTestReport("PASS: real GICv2 APC/dispatch SGIs and original XP quantum-end preemption\n");
}
static VOID controller(PVOID argument)
{
    CHECK(argument==boot_block && KeGetCurrentThread()==&control);
    CHECK(control.Header.Type==ThreadObject && control.State==Running);
    CHECK(control.ApcState.Process==&process->Pcb);
    CHECK(worker_thread.State==Initialized && worker_thread.SuspendApc.NormalRoutine==KiSuspendThread);
    CHECK(worker_thread.SuspendApc.KernelRoutine==(PKKERNEL_ROUTINE)KiSuspendNop);
    CHECK(worker_thread.SuspendApc.RundownRoutine==(PKRUNDOWN_ROUTINE)KiSuspendRundown);
    CHECK(process->Pcb.StackCount==3);
    CHECK(KeSetPriorityThread(&worker_thread,18)==0);
    KeReadyThread(&worker_thread);
    CHECK(stage==1 && worker_thread.State==Waiting && worker_thread.WaitReason==Executive);
    CHECK(worker_thread.WaitBlockList->Object==&gate && !IsListEmpty(&gate.Header.WaitListHead));
    CHECK(worker_thread.Timer.Header.Inserted);
    ULONG64 due=worker_thread.Timer.DueTime.QuadPart;
    ULONG bucket=(ULONG)((due/10000)&(TIMER_TABLE_SIZE-1));
    CHECK(KiTimerTableListHead[bucket].Flink==&worker_thread.Timer.TimerListEntry);
    CHECK(KeSuspendThread(&worker_thread)==0);
    ULONG64 until=test_deadline();
    while (worker_thread.State!=Waiting || worker_thread.WaitReason!=Suspended) {
        CHECK(HalArm64ReadCounter()<until);
        __asm__ volatile("yield" ::: "memory");
    }
    CHECK(stage==1 && worker_thread.State==Waiting && worker_thread.WaitReason==Suspended);
    CHECK(worker_thread.SuspendCount==1 && worker_thread.ApcState.KernelApcInProgress);
    CHECK(worker_thread.WaitBlockList->Object==&worker_thread.SuspendSemaphore);
    CHECK(worker_thread.SuspendSemaphore.Header.SignalState==0);
    CHECK(!worker_thread.Timer.Header.Inserted && IsListEmpty(&KiTimerTableListHead[bucket]));
    CHECK(KeResumeThread(&worker_thread)==1);
    CHECK(stage==1 && worker_thread.State==Waiting && worker_thread.WaitReason==Executive);
    CHECK(worker_thread.SuspendCount==0 && !worker_thread.ApcState.KernelApcInProgress);
    CHECK(worker_thread.WaitBlockList->Object==&gate);
    CHECK(worker_thread.Timer.Header.Inserted && worker_thread.Timer.DueTime.QuadPart==due);
    CHECK(KeSetEvent(&gate,0,FALSE)==0);
    CHECK(stage==2 && worker_thread.State==Waiting && worker_thread.WaitBlockList->Object==&park);
    CHECK(gate.Header.SignalState==0 && IsListEmpty(&gate.Header.WaitListHead));
    CHECK(KeWaitForSingleObject(&token,Executive,KernelMode,FALSE,NULL)==STATUS_SUCCESS);
    CHECK(token.Header.SignalState==0 && control.State==Running && KeGetCurrentIrql()==PASSIVE_LEVEL);
    CHECK(!worker_thread.Timer.Header.Inserted && IsListEmpty(&KiTimerTableListHead[bucket]));
    CHECK(process->Pcb.ProcessLock==0 && worker_thread.ApcQueueLock==0 && control.ApcQueueLock==0);
    CHECK(KeWaitForSingleObject(&idle_started,Executive,KernelMode,FALSE,NULL)==STATUS_SUCCESS);
    CHECK(idle_thread->ContextSwitches==1 && idle_thread->State==Standby && KiIdleSummary==0);
    CHECK(KiReadySummary==0 && KeGetCurrentPrcb()->NextThread==NULL);
    CHECK(worker_thread.ContextSwitches>=4 && control.ContextSwitches>=4);
    CHECK(KiArm64SchedulerSwitches==11 && XpSoftwareRaiseCount==14);
    ULONG threads=0;
    for (PLIST_ENTRY e=process->Pcb.ThreadListHead.Flink; e!=&process->Pcb.ThreadListHead; e=e->Flink) {
        CHECK(++threads<=3);
    }
    CHECK(threads==3);
    XpTestReport("Original thread evidence switches=");XpTestHex(KiArm64SchedulerSwitches);
    XpTestReport(" worker_switches=");XpTestHex(worker_thread.ContextSwitches);
    XpTestReport(" process_threads=");XpTestHex(threads);
    XpTestReport(" idle_switches=");XpTestHex(idle_thread->ContextSwitches);
    XpTestReport(" total_raises=");XpTestHex(XpSoftwareRaiseCount);XpTestReport("\n");
    XpTestReport("PASS: unchanged XP thread initialization, APC suspend/resume, real blocking wait and priority scheduling\n");
    check_timer_service();
    check_software_interrupts();
    check_quantum();
    XpCheckThreadExit(boot_block);
    XpCheckExecutiveResource(&process->Pcb);
    XpCheckFastMutex(&process->Pcb);
    XpCheckExecutivePagedPool(&process->Pcb);
    XpCheckLookasides();
    XpCheckExecutiveHandles(&process->Pcb);
    XpCheckBootObjectState();
    XpCheckBootObjectTypes();
    XpCheckBootNamespace();
    XpCheckBootObjectPaths();
    XpCheckBootObjectInsertion();
    XpCheckBootObjectClose(&process->Pcb);
    XpCheckBootSecurityGlobals();
    XpCheckBootLogonSessions();
    XpCheckBootSystemToken();
    XpCheckBootProcessObjects(boot_block);
    XpCheckBootSystemProcess(boot_block);
    XpCheckBootSystemThread(boot_block);
    XpCheckBootPhase1(boot_block);
    XpCheckBootMdl();
    drain_software_interrupts();
    if (boot_block->Display.VirtualBase) {
        XpTestReport("PASS: display scanout final frame ready\n");
        for (;;) __asm__ volatile("wfe");
    }
    XpTestFinish(0);
}
__declspec(noreturn) VOID XpRunOriginalThreads(PXP_ARM64_BOOT_BLOCK block)
{
    boot_block=block;
    CHECK(KeGetCurrentThread()==NULL && KeGetCurrentIrql()==HIGH_LEVEL);
    process=&block->InitialProcess;
    CHECK(block->Nt.Process==(ULONG_PTR)&process->Pcb);
    CHECK(KiProcessorBlock[0]==KeGetCurrentPrcb() && KiReadySummary==0);
    XpEnableScopeExceptions(block,TRUE);
    /* Original KeInitThread, including its own compiler-emitted except. */
    ULONG64 failures_before=block->Pcr.TrapCount;
    CONTEXT user;
    CHECK(KeInitThread(&rejected,NULL,system_start,worker,NULL,&user,NULL,&process->Pcb)==STATUS_NOT_SUPPORTED);
    CHECK(rejected.InitialStack==NULL && RtlNumberOfClearBits(&block->Tables.StackBitmap)==MM_BOOT_STACK_COUNT);
    RtlZeroMemory(&rejected,sizeof(rejected));
    CHECK(KeInitThread(&rejected,NULL,NULL,worker,NULL,NULL,NULL,&process->Pcb)==STATUS_INVALID_PARAMETER);
    CHECK(rejected.InitialStack==NULL && RtlNumberOfClearBits(&block->Tables.StackBitmap)==MM_BOOT_STACK_COUNT);
    PVOID exhausted[MM_BOOT_STACK_COUNT];
    for (ULONG i=0;i<MM_BOOT_STACK_COUNT;++i) { exhausted[i]=MmCreateKernelStack(FALSE,0);CHECK(exhausted[i]!=NULL); }
    RtlZeroMemory(&rejected,sizeof(rejected));
    CHECK(KeInitThread(&rejected,NULL,system_start,worker,NULL,NULL,NULL,&process->Pcb)==STATUS_INSUFFICIENT_RESOURCES);
    for (ULONG i=0;i<MM_BOOT_STACK_COUNT;++i) MmDeleteKernelStack(exhausted[i],FALSE);
    CHECK(RtlNumberOfClearBits(&block->Tables.StackBitmap)==MM_BOOT_STACK_COUNT);
    CHECK(block->Pcr.TrapCount==failures_before+2);
    CHECK(KiArm64InitializeBootIdle(block)==STATUS_SUCCESS);
    CHECK(XpSoftwareRaiseCount==13);
    XpCheckExecutivePool();
    XpCheckPagedPool();
    idle_thread=KeGetCurrentPrcb()->IdleThread;
    CHECK(idle_thread==KeGetCurrentThread() && block->Nt.Thread==(ULONG_PTR)idle_thread);
    CHECK(idle_thread->Header.Type==ThreadObject && idle_thread->State==Running);
    CHECK(idle_thread->InitialStack==(PVOID)block->Nt.KernelStack &&
          idle_thread->StackLimit==(PVOID)(ULONG_PTR)block->StackBase &&
          !idle_thread->EnableStackSwap && KiIdleSummary==1);
    CHECK(process->Pcb.StackCount==1 && process->Pcb.ActiveProcessors==1 &&
          process->Pcb.ThreadListHead.Flink==&idle_thread->ThreadListEntry);
    CHECK(RtlNumberOfClearBits(&block->Tables.StackBitmap)==MM_BOOT_STACK_COUNT);
    CHECK(KiArm64SchedulerSwitches==0 && KeGetCurrentIrql()==PASSIVE_LEVEL);
    CHECK(KeInitializeThread(&control,NULL,system_start,controller,block,NULL,NULL,&process->Pcb)==STATUS_SUCCESS);
    CHECK(KeInitializeThread(&worker_thread,NULL,system_start,worker,&parameter,NULL,NULL,&process->Pcb)==STATUS_SUCCESS);
    CHECK(KeSetPriorityThread(&control,16)==0);
    KeInitializeEvent(&gate,SynchronizationEvent,FALSE);
    KeInitializeEvent(&park,SynchronizationEvent,FALSE);
    KeInitializeEvent(&idle_started,SynchronizationEvent,FALSE);
    KeInitializeSemaphore(&token,1,1);
    /* Leave the real idle context through original ready selection. It
     * resumes here when the startup system thread first blocks. */
    KeReadyThread(&control);
    CHECK(KeGetCurrentThread()==idle_thread && idle_thread->State==Running);
    CHECK(KeGetCurrentIrql()==PASSIVE_LEVEL && idle_thread->ContextSwitches==1);
    CHECK((ULONG_PTR)idle_thread->KernelStack>=block->StackBase &&
          (ULONG_PTR)idle_thread->KernelStack<block->Nt.KernelStack);
    XpTestReport("PASS: original KeInitializeThread adopted the boot idle stack and original ready selection resumed it\n");
    idle(NULL);
    KeBugCheckEx((ULONG)STATUS_THREAD_IS_TERMINATING,0,0,0,0);
}
