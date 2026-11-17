/* Bounded NT stack entry points over the existing reserved boot arena.
 * NT_UP, DAIF masked, node 0, small resident stacks only; no general PFN MM. */
#include <ntos.h>
#include "bootmap.h"
#include "../../ke/arm64/context.h"
static PMM_ARM64_BOOT_TABLES pool;
static struct {
    MM_ARM64_BOOT_STACK Stack;
    BOOLEAN Entered;
    BOOLEAN Retiring;
} allocations[MM_BOOT_STACK_COUNT];

NTSTATUS MmArm64InitializeKernelStacks(PMM_ARM64_BOOT_TABLES tables)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (!tables || !tables->Active || !tables->StacksPrepared ||
        KeNumberProcessors != 1 || (daif & 0x3c0) != 0x3c0)
        return STATUS_INVALID_DEVICE_STATE;
    if (pool) return STATUS_INVALID_DEVICE_STATE;
    pool = tables;
    return STATUS_SUCCESS;
}
NTSTATUS MmArm64LookupKernelStack(PVOID top, PMM_ARM64_BOOT_TABLES *tables,
                                  PMM_ARM64_BOOT_STACK stack)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (!pool || KeNumberProcessors != 1 || (daif & 0x3c0) != 0x3c0)
        return STATUS_INVALID_DEVICE_STATE;
    if (!top || !tables || !stack) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < MM_BOOT_STACK_COUNT; ++i) {
        MM_ARM64_BOOT_STACK *s = &allocations[i].Stack;
        if (s->Top != (ULONG_PTR)top) continue;
        if (!RtlCheckBit(&pool->StackBitmap, i) ||
            pool->Stacks[i].Generation != s->Generation) return STATUS_INVALID_HANDLE;
        *stack = *s; *tables = pool;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}
PVOID MmCreateKernelStack(BOOLEAN large, UCHAR node)
{
    if (large || node != 0 || !pool) return NULL;
    MM_ARM64_BOOT_STACK stack;
    NTSTATUS status = MmArm64AllocateBootStack(pool, &stack);
    if (!NT_SUCCESS(status)) return NULL;
    allocations[stack.Index].Stack = stack;
    allocations[stack.Index].Entered = FALSE;
    return (PVOID)stack.Top;
}
VOID MmArm64NoteKernelStackActivation(PKTHREAD thread)
{
    /* Called only after the context-switch validation, immediately before
     * the real switch. Never treat a suspended context as never-started. */
    for (ULONG i = 0; pool && i < MM_BOOT_STACK_COUNT; ++i)
        if (allocations[i].Stack.Top == (ULONG_PTR)thread->StackBase &&
            pool->Stacks[i].Owner == thread)
            allocations[i].Entered = TRUE;
}
NTSTATUS MmArm64MarkKernelStackRetiring(PKTHREAD thread)
{
    if (!thread || thread!=KeGetCurrentThread() || thread->State!=Terminated)
        return STATUS_INVALID_DEVICE_STATE;
    MM_ARM64_BOOT_STACK stack;
    PMM_ARM64_BOOT_TABLES tables;
    NTSTATUS status = MmArm64LookupKernelStack(thread->StackBase, &tables, &stack);
    if (!NT_SUCCESS(status)) return status;
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (sp<stack.Limit || sp>=stack.Top) return STATUS_INVALID_DEVICE_STATE;
    ULONG index=stack.Index;
    if (tables->Stacks[index].Owner!=thread || !allocations[index].Entered ||
        allocations[index].Retiring)
        return STATUS_INVALID_DEVICE_STATE;
    allocations[index].Retiring=TRUE;
    return STATUS_SUCCESS;
}
VOID MmDeleteKernelStack(PVOID top, BOOLEAN large)
{
    MM_ARM64_BOOT_STACK stack;
    PMM_ARM64_BOOT_TABLES tables;
    NTSTATUS status = MmArm64LookupKernelStack(top, &tables, &stack);
    if (large) ExRaiseStatus(STATUS_NOT_SUPPORTED);
    if (!NT_SUCCESS(status)) ExRaiseStatus(status);
    ULONG index = stack.Index;
    PKTHREAD owner = tables->Stacks[index].Owner;
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (sp >= stack.Limit && sp < stack.Top) ExRaiseStatus(STATUS_DEVICE_BUSY);
    if (owner) {
        /* A started stack may only be freed once rundown has marked it and
         * the processor is executing on a different stack. */
        BOOLEAN deletable = allocations[index].Entered ?
            allocations[index].Retiring && owner->State==Terminated :
            owner->State==Initialized &&
            owner->KernelStack==(PVOID)(stack.Top-sizeof(KARM64_SWITCH_FRAME));
        if (!deletable || owner==KeGetCurrentThread() || owner->StackBase!=top)
            ExRaiseStatus(STATUS_DEVICE_BUSY);
        status = KiArm64ReleaseKernelContext(owner, tables, &stack);
        if (!NT_SUCCESS(status)) ExRaiseStatus(status);
    }
    status = MmArm64FreeBootStack(tables, &stack);
    if (!NT_SUCCESS(status)) ExRaiseStatus(status);
    RtlZeroMemory(&allocations[index], sizeof(allocations[index]));
}
