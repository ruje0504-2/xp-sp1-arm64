/* ARM64 kernel architecture ABI for the XP port.
 * This is a new port layout. Existing x86/AMD64 structure offsets do not apply.
 * The register frames are consumed by the matching ARM64 entry assembly. */
#ifndef XP_KERNEL_ARM64_H
#define XP_KERNEL_ARM64_H

#include "../ex/arm64/interlocked.h"

typedef ULONG PFN_COUNT;
typedef LONG64 SPFN_NUMBER, *PSPFN_NUMBER;
typedef ULONG64 PFN_NUMBER, *PPFN_NUMBER;

/* Use the existing NT64 software IRQL numbering. The GIC priority
 * implementation must supply Raise/Lower before interrupts are enabled. */
#define PASSIVE_LEVEL 0
#define LOW_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define CLOCK_LEVEL 13
#define IPI_LEVEL 14
#define POWER_LEVEL 14
#define PROFILE_LEVEL 15
/* NT software profiling default in 100 ns units; no timer is armed here. */
#define DEFAULT_PROFILE_INTERVAL 39063
/* The HAL sends APC and dispatch requests as local GICv2 SGIs. */
VOID KiRequestSoftwareInterrupt(KIRQL RequestIrql);
/* Mark the departing thread's resident architecture context for retirement. */
struct _KTHREAD;
VOID KiRundownThread(struct _KTHREAD *Thread);
#define HIGH_LEVEL 15
#if defined(NT_UP)
#define SYNCH_LEVEL DISPATCH_LEVEL
#else
#define SYNCH_LEVEL (IPI_LEVEL - 1)
#endif

#define PAGE_SHIFT 12
/* Original PAGE_ALIGN complements PAGE_SIZE - 1 before widening to ULONG_PTR. */
#define PAGE_SIZE (1 << PAGE_SHIFT)
#define STACK_ALIGN 16
#define STACK_ROUND (STACK_ALIGN - 1)
#define KERNEL_STACK_SIZE (6 * PAGE_SIZE)
#define KERNEL_LARGE_STACK_SIZE (16 * PAGE_SIZE)
#define KERNEL_LARGE_STACK_COMMIT KERNEL_STACK_SIZE

/* Reuse the XP NT64 kernel shared-data VA. The reserved physical backing
 * is mapped here through TTBR1 before any clock or wait consumer starts. */
#define KI_USER_SHARED_DATA 0xfffff78000000000ULL
extern KUSER_SHARED_DATA * const KiArm64SharedUserData;
#define SharedUserData KiArm64SharedUserData

/* Read the XP three-word time publication in order on weakly ordered ARM64.
 * The clock writer publishes High2, Low, High1 with store ordering.
 * Storage must be resident, coherent Normal memory in the ISH domain. */
FORCEINLINE VOID KiArm64ReadSharedTime(const volatile KSYSTEM_TIME *Time,
                                      PLARGE_INTEGER Value)
{
    LONG high;
    ULONG low;
    do {
        high = Time->High1Time;
        __asm__ volatile("dmb ishld" ::: "memory");
        low = Time->LowPart;
        __asm__ volatile("dmb ishld" ::: "memory");
        if (high == Time->High2Time) break;
        __asm__ volatile("yield" ::: "memory");
    } while (TRUE);
    Value->HighPart = high;
    Value->LowPart = low;
}

/* AArch64 takes all exception classes through a fixed VBAR table. There is
 * no executable dispatch stub inside a KINTERRUPT allocation on this port. */
typedef struct DECLSPEC_ALIGN(16) _KTRAP_FRAME {
    ULONG64 X[31];
    ULONG64 Sp;
    ULONG64 Pc;
    ULONG64 Cpsr;
    ULONG64 Esr;
    ULONG64 Far;
    ARM64_NT_NEON128 V[32];
    ULONG Fpcr;
    ULONG Fpsr;
    KIRQL PreviousIrql;
    KPROCESSOR_MODE PreviousMode;
    UCHAR ExceptionActive;
    UCHAR FpStateSaved;
    ULONG Padding;
} KTRAP_FRAME, *PKTRAP_FRAME;

typedef struct DECLSPEC_ALIGN(16) _KEXCEPTION_FRAME {
    ULONG64 X19_X28[10];
    ULONG64 Fp;
    ULONG64 Lr;
    ULONG64 D8_D15[8];
} KEXCEPTION_FRAME, *PKEXCEPTION_FRAME;

/* ARMv8-A 4 KiB stage-1 page descriptor. Access permissions retain the
 * ARM AP encoding; callers must not treat it as the x86 Write bit. */
typedef union _HARDWARE_PTE {
    ULONG64 Value;
    struct {
        ULONG64 Valid : 1;
        ULONG64 TableOrPage : 1;
        ULONG64 AttributeIndex : 3;
        ULONG64 NonSecure : 1;
        ULONG64 AccessPermissions : 2;
        ULONG64 Shareability : 2;
        ULONG64 AccessFlag : 1;
        ULONG64 NotGlobal : 1;
        ULONG64 PageFrameNumber : 36;
        ULONG64 Reserved0 : 4;
        ULONG64 Contiguous : 1;
        ULONG64 PrivilegedExecuteNever : 1;
        ULONG64 UserExecuteNever : 1;
        ULONG64 Software : 4;
        ULONG64 Reserved1 : 5;
    };
} HARDWARE_PTE, *PHARDWARE_PTE;
_Static_assert(sizeof(HARDWARE_PTE) == 8, "ARM64 stage-1 descriptor");
extern PVOID MmHighestUserAddress;
extern PVOID MmSystemRangeStart;
extern ULONG_PTR MmUserProbeAddress;
#define MM_HIGHEST_USER_ADDRESS MmHighestUserAddress
#define MM_SYSTEM_RANGE_START MmSystemRangeStart
#define MM_USER_PROBE_ADDRESS MmUserProbeAddress

/* Original DPC platform code uses the interrupt-enable contract. Only IRQ
 * masking changes here; FIQ/debug/SError policy stays with the boot owner. */
FORCEINLINE VOID _disable(VOID)
{
    __asm__ volatile("msr daifset, #2\nisb" ::: "memory");
}
FORCEINLINE VOID _enable(VOID)
{
    __asm__ volatile("msr daifclr, #2\nisb" ::: "memory");
}
FORCEINLINE VOID KeMemoryBarrier(VOID)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif

typedef struct _KSPECIAL_REGISTERS {
    ULONG64 SctlrEl1;
    ULONG64 TcrEl1;
    ULONG64 Ttbr0El1;
    ULONG64 Ttbr1El1;
    ULONG64 MairEl1;
    ULONG64 VbarEl1;
    ULONG64 TpidrEl1;
    ULONG64 MpidrEl1;
} KSPECIAL_REGISTERS, *PKSPECIAL_REGISTERS;

typedef struct _KPROCESSOR_STATE {
    KSPECIAL_REGISTERS SpecialRegisters;
    CONTEXT ContextFrame;
} KPROCESSOR_STATE, *PKPROCESSOR_STATE;

/* Only fields currently consumed by the shared kernel headers/startup are
 * defined here. Further scheduler/accounting fields must be added alongside
 * their real consumers, before those components can be linked. */
typedef struct DECLSPEC_ALIGN(64) _KPRCB {
    USHORT MinorVersion;
    USHORT MajorVersion;
    CCHAR Number;
    UCHAR Reserved;
    USHORT BuildType;
    struct _KTHREAD *CurrentThread;
    struct _KTHREAD *NextThread;
    struct _KTHREAD *IdleThread;
    struct _KNODE *ParentNode;
    KAFFINITY SetMember;
    KAFFINITY NotSetMember;
    KSPIN_LOCK_QUEUE LockQueue[16];
    PP_LOOKASIDE_LIST PPLookasideList[16];
    PP_LOOKASIDE_LIST PPNPagedLookasideList[POOL_SMALL_LISTS];
    PP_LOOKASIDE_LIST PPPagedLookasideList[POOL_SMALL_LISTS];
    KSPIN_LOCK DpcLock;
    LIST_ENTRY DpcListHead;
    ULONG DpcCount, DpcQueueDepth, DpcRequestRate;
    ULONG MaximumDpcQueueDepth, MinimumDpcRate, TimerHand;
    volatile LOGICAL DpcInterruptRequested;
    volatile LOGICAL DpcRoutineActive;
    ULONG InterruptCount;
    ULONG KernelTime, DpcTime, InterruptTime;
    volatile LOGICAL QuantumEnd;
    KPROCESSOR_STATE ProcessorState;
} KPRCB, *PKPRCB, *RESTRICTED_POINTER PRKPRCB;

typedef BOOLEAN (*PKARM64_TRAP_HANDLER)(PKTRAP_FRAME Frame, ULONG VectorType);
typedef struct DECLSPEC_ALIGN(64) _KPCR {
    struct _KPCR *Self;
    PKPRCB CurrentPrcb;
    KIRQL Irql;
    UCHAR Number;
    USHORT MajorVersion;
    USHORT MinorVersion;
    USHORT Reserved;
    PVOID KdVersionBlock;
    PKARM64_TRAP_HANDLER TrapHandler;
    ULONG64 TrapCount;
    ULONG64 LastTrapPc;
    ULONG64 LastTrapEsr;
    ULONG LastVectorType;
    BOOLEAN LastTrapEsrValid;
    KPRCB Prcb;
} KPCR, *PKPCR;

FORCEINLINE PKPCR KeGetPcr(VOID)
{
    PKPCR pcr;
    __asm__ volatile("mov %0, x18" : "=r"(pcr));
    return pcr;
}
FORCEINLINE PKPRCB KeGetCurrentPrcb(VOID)
{
    return KeGetPcr()->CurrentPrcb;
}
FORCEINLINE ULONG KeGetCurrentProcessorNumber(VOID)
{
    return KeGetPcr()->Number;
}
FORCEINLINE struct _KTHREAD *KeGetCurrentThread(VOID)
{
    return KeGetCurrentPrcb()->CurrentThread;
}
FORCEINLINE ULONG KeIsExecutingDpc(VOID)
{
    return KeGetCurrentPrcb()->DpcRoutineActive != 0;
}
FORCEINLINE KIRQL KeGetCurrentIrql(VOID)
{
    return KeGetPcr()->Irql;
}

NTSTATUS KiArm64InitializePcr(PKPCR Pcr, ULONG Number,
                              PKSPIN_LOCK SharedLocks, PKARM64_TRAP_HANDLER Handler);
BOOLEAN KiArm64TrapDispatch(PKTRAP_FRAME Frame, ULONG VectorType);
extern const UCHAR KiArm64VectorTable[];

_Static_assert(sizeof(KTRAP_FRAME) % 16 == 0, "Trap frame alignment");
_Static_assert(sizeof(KEXCEPTION_FRAME) % 16 == 0, "Exception frame alignment");
#endif
