#ifndef XP_ARM64_SCHEDULER_H
#define XP_ARM64_SCHEDULER_H
#include "context.h"
NTSTATUS KiArm64InitializeDispatcher(PKPROCESS Process);
VOID KiArm64DispatchInterrupt(KIRQL InterruptedIrql);
__declspec(noreturn) VOID KiArm64StartThread(PKSYSTEM_ROUTINE System, PKSTART_ROUTINE Start, PVOID Parameter);
VOID KiArm64NtKernelEntry(VOID);
extern volatile ULONG64 KiArm64SchedulerSwitches;
extern volatile ULONG64 KiArm64QuantumSwitches;
extern volatile ULONG_PTR KiArm64BugCheckData[5];
NTSTATUS KiArm64EnableTimerDispatch(VOID);
VOID KiArm64DisableTimerDispatch(VOID);
VOID KiArm64ClockTimerInterrupt(PKTRAP_FRAME Frame);
VOID KiArm64ProfileClockInterrupt(PKTRAP_FRAME Frame);
__declspec(noreturn) VOID KiArm64IdleLoop(VOID);
typedef struct {
    ULONG64 Interrupts, QueueInsertions, Ticks, StartTicks, RuntimeTicks;
} KI_ARM64_TIMER_DISPATCH_STATS;
VOID KiArm64GetTimerDispatchStats(KI_ARM64_TIMER_DISPATCH_STATS *Stats);
#endif
