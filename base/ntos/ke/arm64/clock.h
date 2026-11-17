#ifndef XP_KE_ARM64_CLOCK_H
#define XP_KE_ARM64_CLOCK_H
#include "../../../boot/arm64/time.h"
typedef struct {
    ULONG64 Frequency, BootCounter, SampleCounter, LastCounter;
    ULONG64 UtcAtSample, InterruptTime, SystemTime, Publications;
    ULONG64 SharedPagePhysical, SharedPagePar;
    ULONG SharedPageLoaderType;
    BOOLEAN Initialized;
} KI_ARM64_CLOCK_STATE;
NTSTATUS KiArm64CounterTo100ns(ULONG64 Delta, ULONG64 Frequency, PULONG64 Value);
NTSTATUS KiArm64InitializeBootClock(const XP_ARM64_BOOT_TIME *Time, ULONG64 Physical);
NTSTATUS KiArm64UpdateBootClock(ULONG64 Counter);
VOID KiArm64GetBootClockState(KI_ARM64_CLOCK_STATE *State);
#endif
