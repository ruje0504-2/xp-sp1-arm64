#ifndef XP_HAL_ARM64_VIRTIO_INPUT_H
#define XP_HAL_ARM64_VIRTIO_INPUT_H
#include <ntos.h>

typedef struct {
    NTSTATUS DiscoveryStatus, StartStatus;
    ULONG Transports, KeyboardIrq, MouseIrq;
    ULONG64 KeyboardInterrupts, MouseInterrupts, KeyboardEvents, MouseEvents, Faults;
} HAL_ARM64_INPUT_STATS;

NTSTATUS HalArm64InitializeVirtioInput(VOID);
VOID HalArm64GetInputStats(HAL_ARM64_INPUT_STATS *Stats);
#endif
