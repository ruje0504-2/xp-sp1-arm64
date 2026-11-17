#ifndef XP_ARM64_KERNEL_CONTEXT_H
#define XP_ARM64_KERNEL_CONTEXT_H
#include <ntos.h>
#include "../../mm/arm64/bootmap.h"
/* ABI-level voluntary kernel switch only. Dispatcher, user state, APC and
 * interrupt return belong to the still-unported higher-level NT paths. */
typedef struct DECLSPEC_ALIGN(16) _KARM64_SWITCH_FRAME {
    ULONG64 X19_X28[10];
    ULONG64 Fp, Lr;
    ULONG64 D8_D15[8];
    ULONG Fpcr, Fpsr, HasFp, Reserved;
} KARM64_SWITCH_FRAME, *PKARM64_SWITCH_FRAME;
_Static_assert(sizeof(KARM64_SWITCH_FRAME) == 176, "Kernel switch frame");
NTSTATUS KiArm64PrepareKernelContext(PKTHREAD Thread, PMM_ARM64_BOOT_TABLES Tables,
    const MM_ARM64_BOOT_STACK *Stack, PKSYSTEM_ROUTINE SystemRoutine,
    PKSTART_ROUTINE StartRoutine, PVOID StartContext);
NTSTATUS KiArm64ReleaseKernelContext(PKTHREAD Thread, PMM_ARM64_BOOT_TABLES Tables,
                                     const MM_ARM64_BOOT_STACK *Stack);
NTSTATUS KiArm64SwitchKernelContext(PKTHREAD From, PKTHREAD To);
VOID KiInitializeContextThread(PKTHREAD Thread, PKSYSTEM_ROUTINE SystemRoutine,
    PKSTART_ROUTINE StartRoutine, PVOID StartContext, PCONTEXT ContextRecord);
VOID KiArm64SwapContextRaw(PKTHREAD From, PKTHREAD To);
VOID KiArm64KernelEntry(VOID);
#endif
