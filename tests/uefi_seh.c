#include "../base/boot/arm64/boot.h"
#include "../base/ntos/rtl/arm64/seh.h"
#include "uefi_test.h"
extern IMAGE_DOS_HEADER __ImageBase;
extern ULONG XpSehContract(volatile ULONG *, ULONG);
extern ULONG XpSehOuter(volatile ULONG *, ULONG);
extern volatile ULONG XpFinallyObserved, XpSearchObserved;
extern ULONG XpSearchOuter(volatile ULONG *), XpSehContinue(volatile ULONG *);
extern ULONG XpSameFrameFinally(volatile ULONG *, ULONG);
static RTL_ARM64_UNWIND_IMAGE image;
static ULONG_PTR low, high;
static BOOLEAN enabled;
volatile ULONG XpSoftwareRaiseCount;
extern const UCHAR RtlArm64RaiseStatusTrap[];
BOOLEAN XpHandleScopeFault(PKTRAP_FRAME frame)
{
    if (((frame->Esr >> 26) & 63) == 0x3c && (frame->Esr & 65535) == 0x7fe)
        XpTestFailure(__LINE__);
    if (!enabled) return FALSE;
    if (frame->Pc == (ULONG_PTR)RtlArm64RaiseStatusTrap &&
        ((frame->Esr >> 26) & 63) == 0x3c && (frame->Esr & 65535) == 0x7f0)
        ++XpSoftwareRaiseCount;
    PKTHREAD current = KeGetCurrentThread();
    ULONG_PTR stack_low = current ? (ULONG_PTR)current->StackLimit : low;
    ULONG_PTR stack_high = current ? (ULONG_PTR)current->StackBase : high;
    if (RtlArm64DispatchScopeException(&image, frame, stack_low, stack_high)) return TRUE;
    XpTestReport("FAIL: scope dispatch rejected a real exception\n");
    XpTestFailure(__LINE__);
}
VOID XpEnableScopeExceptions(PXP_ARM64_BOOT_BLOCK block, BOOLEAN active)
{
    UCHAR *base = (PVOID)&__ImageBase;
    IMAGE_NT_HEADERS64 *nt = (PVOID)(base + __ImageBase.e_lfanew);
    CHECK(RtlArm64InitializeUnwindImage(base, nt->OptionalHeader.SizeOfImage, &image) == STATUS_SUCCESS);
    low = block->StackBase; high = block->Nt.KernelStack;
    enabled = active;
}
VOID XpCheckScopeExceptions(PXP_ARM64_BOOT_BLOCK block)
{
    UCHAR *base = (PVOID)&__ImageBase;
    IMAGE_NT_HEADERS64 *nt = (PVOID)(base + __ImageBase.e_lfanew);
    CHECK(RtlArm64InitializeUnwindImage(base, nt->OptionalHeader.SizeOfImage, &image) == STATUS_SUCCESS);
    low = block->StackBase; high = block->Nt.KernelStack;
    volatile ULONG *guard = (PVOID)(ULONG_PTR)block->StackAllocationBase;
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)guard) == ~0ULL);
    ULONG64 faults_before = block->Pcr.TrapCount;
    enabled = TRUE;
    ULONG result = XpSehContract(guard, 0x4132);
    CHECK(result == ((0x4132u + 7) ^ (ULONG)STATUS_ACCESS_VIOLATION));
    CHECK(RtlArm64SehLastStatus() == STATUS_SUCCESS);
    XpFinallyObserved = 0;
    result = XpSehOuter(guard, 0x4132);
    CHECK(result == ((0x4132u + 11) ^ (ULONG)STATUS_ACCESS_VIOLATION));
    CHECK(XpFinallyObserved == 0x4132u + 3);
    CHECK(RtlArm64SehLastStatus() == STATUS_SUCCESS);
    XpSearchObserved = 0;
    CHECK(XpSearchOuter(guard) == (ULONG)STATUS_ACCESS_VIOLATION);
    CHECK(XpSearchObserved == (ULONG)STATUS_ACCESS_VIOLATION);
    XpFinallyObserved = 0;
    CHECK(XpSameFrameFinally(guard, 0x4132) == ((0x4132u + 13) ^ (ULONG)STATUS_ACCESS_VIOLATION));
    CHECK(XpFinallyObserved == 0x4132u + 13 + 5);
    CHECK(XpSehContinue(guard) == 0x55 + 11);
    ULONG64 cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    ULONG64 disabled = cpacr & ~(3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(disabled) : "memory");
    CHECK(XpSehContract(guard, 0x4132) == ((0x4132u + 7) ^ (ULONG)STATUS_ACCESS_VIOLATION));
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(cpacr) : "memory");
    enabled = FALSE;
    CHECK(block->Pcr.TrapCount == faults_before + 6);
    XpTestReport("PASS: real access fault, original C scope search, parent-frame filter, finally unwind and exception target resume\n");
}
