/* Raw architecture-context regression control with FP enabled and disabled.
 * Full original NT constructors and scheduling are exercised separately by
 * uefi_scheduler.c; this control intentionally has no dispatcher objects. */
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/ke/arm64/context.h"
#include "uefi_test.h"
static KTHREAD main_context, worker_context, duplicate_context;
static MM_ARM64_BOOT_STACK worker_stack;
static PMM_ARM64_BOOT_TABLES tables;
static volatile ULONG stage;
static BOOLEAN with_fp;
typedef struct {
    ULONG64 X19_X28[10], D8_D15[8];
    ULONG Fpcr, Fpsr;
    NTSTATUS Status;
} SWITCH_WITNESS;
extern VOID XpContextRegisterFixture(PKTHREAD From, PKTHREAD To, SWITCH_WITNESS *Witness);
extern VOID XpCheckStackFault(ULONG_PTR Address);
extern VOID XpEnableScopeExceptions(PXP_ARM64_BOOT_BLOCK Block, BOOLEAN Active);
static __declspec(noinline) NTSTATUS try_delete(PVOID top)
{
    try { MmDeleteKernelStack(top, FALSE); }
    except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    return STATUS_SUCCESS;
}
extern ULONG64 XpReadFpcr(VOID), XpReadFpsr(VOID);
extern VOID XpWriteFpState(ULONG64 Fpcr, ULONG64 Fpsr);
static SWITCH_WITNESS witness;
static ULONG parameter_token;
static VOID worker(PVOID parameter)
{
    CHECK(parameter == &parameter_token && KeGetCurrentThread() == &worker_context);
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    CHECK(sp >= worker_stack.Limit && sp < worker_stack.Top && !(sp & 15));
    volatile ULONG64 local[128];
    for (ULONG i = 0; i < 128; ++i) local[i] = 0x8900000000000000ULL | i;
    CHECK(MmArm64FreeBootStack(tables, &worker_stack) == STATUS_DEVICE_BUSY);
    CHECK(MmArm64UnbindBootStack(tables, &worker_stack, &worker_context) == STATUS_DEVICE_BUSY);
    CHECK(KiArm64ReleaseKernelContext(&worker_context, tables, &worker_stack) == STATUS_DEVICE_BUSY);
    XpCheckStackFault(worker_stack.Limit - PAGE_SIZE);
    CHECK(try_delete((PVOID)worker_stack.Top) == STATUS_DEVICE_BUSY);
    if (with_fp) {
        ULONG64 fpcr = XpReadFpcr(), fpsr = XpReadFpsr();
        CHECK(fpcr == 0 && fpsr == 0);
        XpWriteFpState(1ULL << 23, 1);
    }
    stage = 1;
    CHECK(KiArm64SwitchKernelContext(&worker_context, &main_context) == STATUS_SUCCESS);
    CHECK(KeGetCurrentThread() == &worker_context);
    for (ULONG i = 0; i < 128; ++i) CHECK(local[i] == (0x8900000000000000ULL | i));
    if (with_fp) {
        ULONG64 fpcr = XpReadFpcr(), fpsr = XpReadFpsr();
        CHECK(fpcr == (1ULL << 23) && fpsr == 1);
    }
    stage = 2;
    CHECK(KiArm64SwitchKernelContext(&worker_context, &main_context) == STATUS_SUCCESS);
    CHECK(FALSE); /* The owner explicitly retires this suspended test context. */
}
static VOID system_start(PKSTART_ROUTINE start, PVOID context)
{
    CHECK(start == worker);
    start(context);
    CHECK(FALSE);
}
static VOID roundtrip(VOID)
{
    if (with_fp) {
        XpContextRegisterFixture(&main_context, &worker_context, &witness);
        CHECK(witness.Status == STATUS_SUCCESS);
        for (ULONG i = 0; i < 10; ++i) CHECK(witness.X19_X28[i] == 0x5000 + 19 + i);
        for (ULONG i = 0; i < 8; ++i) CHECK(witness.D8_D15[i] == 0x6000 + 8 + i);
        CHECK(witness.Fpcr == (1u << 22) && witness.Fpsr == (1u << 27));
    } else {
        CHECK(KiArm64SwitchKernelContext(&main_context, &worker_context) == STATUS_SUCCESS);
    }
    CHECK(KeGetCurrentThread() == &main_context);
}
VOID XpCheckKernelContexts(PXP_ARM64_BOOT_BLOCK block)
{
    tables = &block->Tables;
    XpEnableScopeExceptions(block, TRUE);
    CHECK(KeGetCurrentThread() == NULL);
    main_context.StackLimit = (PVOID)(ULONG_PTR)block->StackBase;
    main_context.StackBase = main_context.InitialStack = (PVOID)block->Nt.KernelStack;
    main_context.PreviousMode = KernelMode;
    block->Pcr.Prcb.CurrentThread = &main_context;
    ULONG64 old_cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(old_cpacr));
    for (ULONG mode = 0; mode < 2; ++mode) {
        with_fp = mode == 0;
        ULONG64 cpacr = with_fp ? old_cpacr | (3ULL << 20) : old_cpacr & ~(3ULL << 20);
        __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(cpacr) : "memory");
        PVOID top = MmCreateKernelStack(FALSE, 0);
        CHECK(top != NULL);
        PMM_ARM64_BOOT_TABLES actual_tables;
        CHECK(MmArm64LookupKernelStack(top, &actual_tables, &worker_stack) == STATUS_SUCCESS);
        CHECK(actual_tables == tables);
        worker_context.InitialStack = worker_context.StackBase = worker_context.StackLimit = NULL;
        CHECK(KiArm64PrepareKernelContext(&worker_context, tables, &worker_stack,
            system_start, worker, &parameter_token) == STATUS_SUCCESS);
        CHECK(KiArm64PrepareKernelContext(&duplicate_context, tables, &worker_stack,
                                           system_start, worker, &parameter_token) == STATUS_DEVICE_BUSY);
        CHECK(MmArm64FreeBootStack(tables, &worker_stack) == STATUS_DEVICE_BUSY);
        stage = 0;
        roundtrip();
        CHECK(stage == 1);
        CHECK(try_delete((PVOID)worker_stack.Top) == STATUS_DEVICE_BUSY);
        roundtrip();
        CHECK(stage == 2);
        /* Explicit architecture-context cancellation after switch-out, not
         * an assertion that NT thread termination or completion exists. */
        CHECK(KiArm64ReleaseKernelContext(&worker_context, tables, &worker_stack) == STATUS_SUCCESS);
        ULONG_PTR retired = worker_stack.Limit;
        CHECK(try_delete((PVOID)worker_stack.Top) == STATUS_SUCCESS);
        RtlZeroMemory(&worker_stack, sizeof(worker_stack));
        CHECK(MmArm64QueryPhysicalAddress(retired) == ~0ULL);
        CHECK(worker_context.KernelStack == NULL && worker_context.StackBase == NULL);
    }
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(old_cpacr) : "memory");
    block->Pcr.Prcb.CurrentThread = NULL;
    RtlZeroMemory(&main_context, sizeof(main_context));
    CHECK(RtlNumberOfClearBits(&tables->StackBitmap) == MM_BOOT_STACK_COUNT);
    XpEnableScopeExceptions(block, FALSE);
    XpTestReport("PASS: real kernel-context start/resume, callee registers, FP isolation/disabled path, guarded live stack and retirement\n");
}
