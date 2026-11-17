#ifndef XP_HAL_ARM64_GICV2_H
#define XP_HAL_ARM64_GICV2_H
#include <ntos.h>
#include "../../../boot/arm64/platform.h"
typedef struct {
    ULONG64 Frequency, Period, Interrupts, Spurious, MissedPeriods, LastCounter;
    ULONG64 DispatchInterrupts, ApcInterrupts;
    ULONG64 GicDistributorVa, GicCpuInterfaceVa;
    ULONG LastInterruptedIrql, LastIntId, LastFailure;
    BOOLEAN Initialized, TimerActive;
} HAL_ARM64_IRQ_STATS;
NTSTATUS HalArm64InitializeGicV2(const HAL_ARM64_BOOT_PLATFORM *Platform);
NTSTATUS HalArm64InitializePhase1(PLOADER_PARAMETER_BLOCK LoaderBlock);
NTSTATUS HalArm64StartBootTimer(ULONG Hertz);
VOID HalArm64StopBootTimer(VOID);
VOID HalArm64StartProfileInterrupt(KPROFILE_SOURCE Source);
VOID HalArm64StopProfileInterrupt(KPROFILE_SOURCE Source);
BOOLEAN HalArm64HandleBootIrq(PKTRAP_FRAME Frame);
ULONG64 HalArm64ReadCounter(VOID);
LARGE_INTEGER HalArm64QueryPerformanceCounter(PLARGE_INTEGER Frequency);
ULONG64 HalArm64ReadTimerControl(VOID);
ULONG HalArm64ReadPriorityMask(VOID);
ULONG HalArm64TimerPendingOrActive(VOID);
ULONG HalArm64SoftwareIrqPending(VOID);
ULONG HalArm64SoftwareIrqActive(VOID);
VOID HalArm64GetIrqStats(HAL_ARM64_IRQ_STATS *Stats);
#endif
