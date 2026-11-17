/* Tests the real architecture/MM boundary required by KeInitThread.
 * This is NOT a replacement KeInitThread or proof of scheduler execution. */
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/ke/arm64/context.h"
#include "../base/ntos/rtl/arm64/seh.h"
#include "uefi_test.h"
extern VOID XpEnableScopeExceptions(PXP_ARM64_BOOT_BLOCK Block, BOOLEAN Active);
extern VOID KeUninitThread(PKTHREAD Thread);
extern const UCHAR RtlArm64RaiseStatusTrap[];
extern volatile ULONG XpSoftwareRaiseCount;
static KTHREAD thread;
static CONTEXT user;
static ULONG cleanups, seen_flags, seen_code;
static VOID never_started(PKSTART_ROUTINE start, PVOID parameter)
{
    UNREFERENCED_PARAMETER(start); UNREFERENCED_PARAMETER(parameter);
    CHECK(FALSE);
}
static VOID stack_fields(PVOID top)
{
    RtlZeroMemory(&thread, sizeof(thread));
    thread.StackBase = thread.InitialStack = top;
    thread.StackLimit = (PUCHAR)top - KERNEL_STACK_SIZE;
}
static __declspec(noinline) LONG observe(PEXCEPTION_POINTERS pointers)
{
    CHECK(pointers->ExceptionRecord->ExceptionAddress == (PVOID)RtlArm64RaiseStatusTrap);
    CHECK(pointers->ExceptionRecord->NumberParameters == 0);
    seen_flags = pointers->ExceptionRecord->ExceptionFlags;
    seen_code = pointers->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
static __declspec(noinline) VOID raise_with_cleanup(NTSTATUS status)
{
    volatile ULONG cookie = 0x51ab;
    try { ExRaiseStatus(status); }
    finally { CHECK(AbnormalTermination()); cleanups = cookie; }
}
static __declspec(noinline) NTSTATUS catch_raise(NTSTATUS status)
{
    try { raise_with_cleanup(status); }
    except (observe(GetExceptionInformation())) { return GetExceptionCode(); }
    return STATUS_UNSUCCESSFUL;
}
static __declspec(noinline) NTSTATUS try_initialize(PCONTEXT context, PKSYSTEM_ROUTINE system)
{
    try { KiInitializeContextThread(&thread, system, NULL, NULL, context); }
    except (observe(GetExceptionInformation())) { return GetExceptionCode(); }
    return STATUS_SUCCESS;
}
static __declspec(noinline) NTSTATUS try_delete(PVOID top)
{
    try { MmDeleteKernelStack(top, FALSE); }
    except (observe(GetExceptionInformation())) { return GetExceptionCode(); }
    return STATUS_SUCCESS;
}
VOID XpCheckThreadContextBoundary(PXP_ARM64_BOOT_BLOCK block)
{
    XpEnableScopeExceptions(block, TRUE);
    ULONG64 before = block->Pcr.TrapCount;
    CHECK(catch_raise(STATUS_INSUFFICIENT_RESOURCES) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(cleanups == 0x51ab && seen_code == (ULONG)STATUS_INSUFFICIENT_RESOURCES);
    CHECK(seen_flags == EXCEPTION_NONCONTINUABLE);
    CHECK(MmCreateKernelStack(TRUE, 0) == NULL);
    CHECK(MmCreateKernelStack(FALSE, 1) == NULL);
    for (ULONG attempt = 0; attempt < 3; ++attempt) {
        PVOID top = MmCreateKernelStack(FALSE, 0);
        CHECK(top != NULL);
        stack_fields(top);
        if (attempt < 2) {
            CHECK(try_initialize(attempt == 0 ? &user : NULL,
                attempt == 0 ? never_started : NULL) ==
                (attempt == 0 ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER));
            CHECK(thread.KernelStack == NULL && thread.StackBase == top);
            /* Same real MM cleanup needed by original KeInitThread's except.
             * The original constructor itself remains unlinked. */
            CHECK(try_delete(top) == STATUS_SUCCESS);
        } else {
            CHECK(try_initialize(NULL, never_started) == STATUS_SUCCESS);
            CHECK(thread.PreviousMode == KernelMode && thread.KernelStack != NULL);
            /* Execute the unchanged original before-start destructor. */
            KeUninitThread(&thread);
            CHECK(thread.InitialStack == NULL && thread.KernelStack == NULL);
        }
        CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)top - PAGE_SIZE) == ~0ULL);
        CHECK(try_delete(top) == STATUS_INVALID_HANDLE);
    }
    PVOID tops[MM_BOOT_STACK_COUNT];
    for (ULONG i = 0; i < MM_BOOT_STACK_COUNT; ++i) {
        tops[i] = MmCreateKernelStack(FALSE, 0); CHECK(tops[i] != NULL);
    }
    CHECK(MmCreateKernelStack(FALSE, 0) == NULL);
    for (ULONG i = 0; i < MM_BOOT_STACK_COUNT; ++i) MmDeleteKernelStack(tops[i], FALSE);
    ULONG64 cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    ULONG64 disabled = cpacr & ~(3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(disabled) : "memory");
    cleanups = 0;
    CHECK(catch_raise(STATUS_NOT_SUPPORTED) == STATUS_NOT_SUPPORTED && cleanups == 0x51ab);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(cpacr) : "memory");
    CHECK(block->Pcr.TrapCount == before + 7);
    CHECK(RtlNumberOfClearBits(&block->Tables.StackBitmap) == MM_BOOT_STACK_COUNT);
    CHECK(XpSoftwareRaiseCount == 11);
    XpTestReport("Thread context evidence software_raises=");
    XpTestHex(XpSoftwareRaiseCount); XpTestReport("\n");
    XpEnableScopeExceptions(block, FALSE);
    XpTestReport("PASS: NT context/MM boundary, real software raise/finally, failure stack reclamation and original KeUninitThread\n");
}
