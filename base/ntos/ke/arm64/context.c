#include "context.h"
#include "scheduler.h"
static BOOLEAN switch_environment(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return KeNumberProcessors == 1 && (daif & 0x3c0) == 0x3c0;
}
static ULONG fp_enabled(VOID)
{
    ULONG64 cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    return (ULONG)((cpacr >> 20) & 1);
}
static NTSTATUS prepare_context(PKTHREAD thread, PMM_ARM64_BOOT_TABLES tables,
    const MM_ARM64_BOOT_STACK *stack, PKSYSTEM_ROUTINE system,
    PKSTART_ROUTINE start, PVOID parameter, BOOLEAN populated)
{
    if (!switch_environment()) return STATUS_INVALID_DEVICE_STATE;
    if (!thread || !stack || !system || thread->KernelStack)
        return STATUS_INVALID_PARAMETER;
    if (populated) {
        if (thread->StackBase != (PVOID)stack->Top ||
            thread->InitialStack != (PVOID)stack->Top ||
            thread->StackLimit != (PVOID)stack->Limit)
            return STATUS_INVALID_PARAMETER;
    } else if (thread->StackBase) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = MmArm64BindBootStack(tables, stack, thread);
    if (!NT_SUCCESS(status)) return status;
    PKARM64_SWITCH_FRAME frame = (PVOID)(stack->Top - sizeof(KARM64_SWITCH_FRAME));
    RtlZeroMemory(frame, sizeof(*frame));
    frame->X19_X28[0] = (ULONG_PTR)system;
    frame->X19_X28[1] = (ULONG_PTR)start;
    frame->X19_X28[2] = (ULONG_PTR)parameter;
    frame->Lr = (ULONG_PTR)(populated ? KiArm64NtKernelEntry : KiArm64KernelEntry);
    frame->HasFp = fp_enabled();
    thread->InitialStack = thread->StackBase = (PVOID)stack->Top;
    thread->StackLimit = (PVOID)stack->Limit;
    thread->KernelStack = frame;
    thread->PreviousMode = KernelMode;
    /* Do not fabricate dispatcher initialization, readiness or completion. */
    return STATUS_SUCCESS;
}
NTSTATUS KiArm64PrepareKernelContext(PKTHREAD thread, PMM_ARM64_BOOT_TABLES tables,
    const MM_ARM64_BOOT_STACK *stack, PKSYSTEM_ROUTINE system,
    PKSTART_ROUTINE start, PVOID parameter)
{
    return prepare_context(thread, tables, stack, system, start, parameter, FALSE);
}
VOID KiInitializeContextThread(PKTHREAD thread, PKSYSTEM_ROUTINE system,
    PKSTART_ROUTINE start, PVOID parameter, PCONTEXT user)
{
    /* Original KeInitThread has already populated these three stack fields.
     * Reject unsupported EL0 before binding or modifying the target stack. */
    if (user) ExRaiseStatus(STATUS_NOT_SUPPORTED);
    if (!thread) ExRaiseStatus(STATUS_INVALID_PARAMETER);
    /* KeInitializeThread with a supplied stack and no entry routine is
     * the original idle bootstrap contract. Only the active, registered
     * loader stack can adopt the current context in this way. */
    if (!system && !start && !parameter && KeLoaderBlock &&
        KeLoaderBlock->Thread==(ULONG_PTR)thread) {
        ULONG_PTR sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        if (!switch_environment() || KeGetCurrentThread() || KeGetCurrentIrql()!=PASSIVE_LEVEL ||
            thread->ApcState.Process!=(PKPROCESS)KeLoaderBlock->Process ||
            thread->StackBase!=(PVOID)KeLoaderBlock->KernelStack ||
            thread->InitialStack!=thread->StackBase || thread->KernelStack ||
            sp<(ULONG_PTR)thread->StackLimit || sp>=(ULONG_PTR)thread->StackBase)
            ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
        thread->KernelStack=(PVOID)sp;
        thread->PreviousMode=KernelMode;
        return;
    }
    MM_ARM64_BOOT_STACK stack;
    PMM_ARM64_BOOT_TABLES tables;
    NTSTATUS status = MmArm64LookupKernelStack(thread->StackBase, &tables, &stack);
    if (!NT_SUCCESS(status)) ExRaiseStatus(status);
    status = prepare_context(thread, tables, &stack, system, start, parameter, TRUE);
    if (!NT_SUCCESS(status)) ExRaiseStatus(status);
}
NTSTATUS KiArm64ReleaseKernelContext(PKTHREAD thread, PMM_ARM64_BOOT_TABLES tables,
                                     const MM_ARM64_BOOT_STACK *stack)
{
    if (!switch_environment()) return STATUS_INVALID_DEVICE_STATE;
    if (!thread || !stack || thread->StackBase != (PVOID)stack->Top ||
        thread->StackLimit != (PVOID)stack->Limit || !thread->KernelStack)
        return STATUS_INVALID_PARAMETER;
    if (thread == KeGetCurrentThread()) return STATUS_DEVICE_BUSY;
    NTSTATUS status = MmArm64UnbindBootStack(tables, stack, thread);
    if (!NT_SUCCESS(status)) return status;
    thread->KernelStack = thread->InitialStack = thread->StackBase = thread->StackLimit = NULL;
    return STATUS_SUCCESS;
}
NTSTATUS KiArm64SwitchKernelContext(PKTHREAD from, PKTHREAD to)
{
    if (!switch_environment()) return STATUS_INVALID_DEVICE_STATE;
    if (!from || !to || from == to || KeGetCurrentThread() != from)
        return STATUS_INVALID_PARAMETER;
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (sp < (ULONG_PTR)from->StackLimit ||
        sp - (ULONG_PTR)from->StackLimit < sizeof(KARM64_SWITCH_FRAME) ||
        sp >= (ULONG_PTR)from->StackBase)
        return STATUS_STACK_OVERFLOW;
    ULONG_PTR target = (ULONG_PTR)to->KernelStack;
    ULONG_PTR limit = (ULONG_PTR)to->StackLimit, top = (ULONG_PTR)to->StackBase;
    if (!target || (target & 15) || target < limit || target >= top ||
        top - target < sizeof(KARM64_SWITCH_FRAME) ||
        (to->PreviousMode != KernelMode && (to->PreviousMode != UserMode || !to->Teb || !to->TrapFrame)))
        return STATUS_INVALID_PARAMETER;
    PKARM64_SWITCH_FRAME frame = (PVOID)target;
    if (frame->HasFp != fp_enabled())
        return STATUS_NOT_SUPPORTED; /* No lazy-FP ownership implementation. */
    MmArm64NoteKernelStackActivation(to);
    KiArm64SwapContextRaw(from, to);
    return STATUS_SUCCESS;
}

VOID KiRundownThread(PKTHREAD thread)
{
    if (KeGetCurrentIrql()!=SYNCH_LEVEL)
        KeBugCheckEx((ULONG)STATUS_INVALID_DEVICE_STATE,0x29,(ULONG_PTR)thread,0,0);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status=MmArm64MarkKernelStackRetiring(thread);
    if (!NT_SUCCESS(status))
        KeBugCheckEx((ULONG)status,0x29,(ULONG_PTR)thread,0,0);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
