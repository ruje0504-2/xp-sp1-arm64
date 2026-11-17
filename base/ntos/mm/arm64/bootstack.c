#include <ntos.h>
#include "bootmap.h"
#include "bootframes.h"

/* Guarded virtual slots backed by the boot PFN list. Restrict this early
 * allocator to its actual synchronization environment rather than pretending
 * to implement the general scheduler/MM spinlock contract. */
static BOOLEAN allowed(PMM_ARM64_BOOT_TABLES tables)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return tables && tables->Active && tables->StacksPrepared &&
           KeNumberProcessors == 1 && (daif & 0x3c0) == 0x3c0;
}
static MM_ARM64_BOOT_STACK_SLOT *find_slot(PMM_ARM64_BOOT_TABLES tables,
                                           const MM_ARM64_BOOT_STACK *stack)
{
    if (!stack || stack->Index >= MM_BOOT_STACK_COUNT) return NULL;
    MM_ARM64_BOOT_STACK_SLOT *slot = &tables->Stacks[stack->Index];
    ULONG_PTR limit = tables->StackArena +
        ((ULONG64)(stack->Index * MM_BOOT_STACK_STRIDE + 1) << PAGE_SHIFT);
    if (!RtlCheckBit(&tables->StackBitmap, stack->Index) ||
        slot->Generation != stack->Generation || stack->Limit != limit ||
        stack->Top != limit + KERNEL_STACK_SIZE)
        return NULL;
    return slot;
}
static VOID map_stack(MM_ARM64_BOOT_STACK_SLOT *slot, BOOLEAN present)
{
    for (ULONG i = 1; i < MM_BOOT_STACK_STRIDE; ++i) {
        ULONG64 descriptor=0;
        if (present) {
            ULONG64 physical=slot->Frames[i-1]<<PAGE_SHIFT;
            ULONG64 par=MmArm64QueryTranslation(physical,TRUE);
            descriptor=slot->Descriptors[i] | physical | ((par&(1ULL<<9))>>4);
        }
        *slot->Ptes[i]=descriptor;
        if (present) MmArm64RecordBootMapping(slot, slot->Frames[i-1], slot->Ptes[i], TRUE);
        __asm__ volatile("dc cvac, %0" :: "r"(slot->Ptes[i]) : "memory");
    }
    /* Complete translation retirement before exposing a slot for reuse. */
    __asm__ volatile("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
}
static VOID release_frames(MM_ARM64_BOOT_STACK_SLOT *slot, ULONG count)
{
    while (count) {
        PFN_NUMBER *frame=&slot->Frames[--count];
        NTSTATUS status=MmArm64ReturnBootFrame(slot,*frame);
        if (!NT_SUCCESS(status)) KeBugCheckEx((ULONG)status,0x35,*frame,0,0);
        *frame=0;
    }
}
NTSTATUS MmArm64AllocateBootStack(PMM_ARM64_BOOT_TABLES tables, PMM_ARM64_BOOT_STACK stack)
{
    if (!stack) return STATUS_INVALID_PARAMETER;
    if (!allowed(tables)) return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(stack, sizeof(*stack));
    ULONG index = MAXULONG, hint = 0;
    for (ULONG attempt = 0; attempt < MM_BOOT_STACK_COUNT; ++attempt) {
        index = RtlFindClearBitsAndSet(&tables->StackBitmap, 1, hint);
        if (index == MAXULONG) return STATUS_INSUFFICIENT_RESOURCES;
        if (tables->Stacks[index].Generation != ~0ULL) break;
        /* Do not wrap a generation or let a saturated slot hide other free slots. */
        RtlClearBits(&tables->StackBitmap, index, 1);
        hint = (index + 1) % MM_BOOT_STACK_COUNT;
        index = MAXULONG;
    }
    if (index == MAXULONG) return STATUS_INSUFFICIENT_RESOURCES;
    MM_ARM64_BOOT_STACK_SLOT *slot = &tables->Stacks[index];
    for (ULONG i=0;i<MM_BOOT_STACK_DATA_PAGES;++i) {
        NTSTATUS status=MmArm64TakeBootFrame(slot,&slot->Frames[i]);
        if (!NT_SUCCESS(status)) {
            release_frames(slot,i);
            RtlClearBits(&tables->StackBitmap,index,1);
            return status;
        }
    }
    ++slot->Generation;
    map_stack(slot, TRUE);
    stack->Index = index;
    stack->Generation = slot->Generation;
    stack->Limit = tables->StackArena +
        ((ULONG64)(index * MM_BOOT_STACK_STRIDE + 1) << PAGE_SHIFT);
    stack->Top = stack->Limit + KERNEL_STACK_SIZE;
    RtlZeroMemory((PVOID)stack->Limit, KERNEL_STACK_SIZE);
    return STATUS_SUCCESS;
}
NTSTATUS MmArm64FreeBootStack(PMM_ARM64_BOOT_TABLES tables, PMM_ARM64_BOOT_STACK stack)
{
    if (!allowed(tables)) return STATUS_INVALID_DEVICE_STATE;
    MM_ARM64_BOOT_STACK_SLOT *slot = find_slot(tables, stack);
    if (!slot) return STATUS_INVALID_HANDLE;
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (slot->Owner || (sp >= stack->Limit && sp < stack->Top)) return STATUS_DEVICE_BUSY;
    for (ULONG i=0;i<MM_BOOT_STACK_DATA_PAGES;++i)
        if (!MmArm64BootFrameOwnedBy(slot->Frames[i],slot)) return STATUS_INVALID_DEVICE_STATE;
    map_stack(slot, FALSE);
    release_frames(slot,MM_BOOT_STACK_DATA_PAGES);
    RtlClearBits(&tables->StackBitmap, stack->Index, 1);
    RtlZeroMemory(stack, sizeof(*stack));
    return STATUS_SUCCESS;
}
NTSTATUS MmArm64BindBootStack(PMM_ARM64_BOOT_TABLES tables,
                              const MM_ARM64_BOOT_STACK *stack, PVOID owner)
{
    if (!allowed(tables)) return STATUS_INVALID_DEVICE_STATE;
    if (!owner) return STATUS_INVALID_PARAMETER;
    MM_ARM64_BOOT_STACK_SLOT *slot = find_slot(tables, stack);
    if (!slot) return STATUS_INVALID_HANDLE;
    if (slot->Owner) return STATUS_DEVICE_BUSY;
    slot->Owner = owner;
    return STATUS_SUCCESS;
}
NTSTATUS MmArm64UnbindBootStack(PMM_ARM64_BOOT_TABLES tables,
                                const MM_ARM64_BOOT_STACK *stack, PVOID owner)
{
    if (!allowed(tables)) return STATUS_INVALID_DEVICE_STATE;
    MM_ARM64_BOOT_STACK_SLOT *slot = find_slot(tables, stack);
    if (!slot || !owner || slot->Owner != owner) return STATUS_INVALID_HANDLE;
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (sp >= stack->Limit && sp < stack->Top) return STATUS_DEVICE_BUSY;
    slot->Owner = NULL;
    return STATUS_SUCCESS;
}
