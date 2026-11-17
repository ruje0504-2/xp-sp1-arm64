#include "boot.h"

/* Keep firmware boot pages reserved until all remaining ownership/runtime
 * dependencies are audited. Taking over page tables alone does not retire
 * every firmware reference. Loader image/data, ACPI and MMIO also remain. */
static TYPE_OF_MEMORY BlMemoryType(const XP_EFI_MEMORY_DESCRIPTOR *entry)
{
    if (entry->Attribute & XP_EFI_MEMORY_RUNTIME) return LoaderFirmwarePermanent;
    switch (entry->Type) {
    case 1: return LoaderLoadedProgram;
    case 2: return LoaderMemoryData;
    case 7:
        return (entry->Attribute & 8) && !(entry->Attribute & 0x70000) ?
            LoaderFree : LoaderReserve;
    case 8: return LoaderBad;
    default: return LoaderFirmwarePermanent;
    }
}
NTSTATUS BlArm64ConvertMemoryMap(PXP_ARM64_BOOT_BLOCK block)
{
    if (!block) return STATUS_INVALID_PARAMETER;
    InitializeListHead(&block->Nt.MemoryDescriptorListHead);
    block->DescriptorCount = 0;
    block->FreePages = block->ReservedPages = 0;
    if (block->DescriptorVersion != 1 ||
        block->DescriptorSize < sizeof(XP_EFI_MEMORY_DESCRIPTOR) ||
        (block->DescriptorSize & 7) || !block->MapSize ||
        block->MapSize > sizeof(block->Map) ||
        block->MapSize % block->DescriptorSize ||
        block->MapSize / block->DescriptorSize > XP_ARM64_BOOT_DESCRIPTOR_CAPACITY)
        return STATUS_INVALID_PARAMETER;
    ULONG count = (ULONG)(block->MapSize / block->DescriptorSize);
    for (ULONG i = 0; i < count; ++i) {
        const XP_EFI_MEMORY_DESCRIPTOR *entry =
            (const XP_EFI_MEMORY_DESCRIPTOR *)(block->Map + i * block->DescriptorSize);
        ULONG64 base = entry->PhysicalStart >> PAGE_SHIFT;
        /* Original XP descriptors use 32-bit page indices/counts. */
        if ((entry->PhysicalStart & (PAGE_SIZE - 1)) || !entry->NumberOfPages ||
            base > MAXULONG || entry->NumberOfPages > MAXULONG ||
            entry->NumberOfPages > (1ULL << 32) - base)
            return STATUS_INVALID_PARAMETER;
        PMEMORY_ALLOCATION_DESCRIPTOR nt = &block->Descriptors[i];
        RtlZeroMemory(nt, sizeof(*nt));
        nt->MemoryType = BlMemoryType(entry);
        nt->BasePage = (ULONG)base;
        nt->PageCount = (ULONG)entry->NumberOfPages;
    }
    /* Sort before publishing the list. Firmware maps need not be sorted. */
    for (ULONG i = 1; i < count; ++i) {
        MEMORY_ALLOCATION_DESCRIPTOR value = block->Descriptors[i];
        ULONG j = i;
        while (j && block->Descriptors[j - 1].BasePage > value.BasePage) {
            block->Descriptors[j] = block->Descriptors[j - 1];
            --j;
        }
        block->Descriptors[j] = value;
    }
    for (ULONG i = 1; i < count; ++i)
        if ((ULONG64)block->Descriptors[i - 1].BasePage +
            block->Descriptors[i - 1].PageCount > block->Descriptors[i].BasePage)
            return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < count; ++i) {
        PMEMORY_ALLOCATION_DESCRIPTOR nt = &block->Descriptors[i];
        InsertTailList(&block->Nt.MemoryDescriptorListHead, &nt->ListEntry);
        if (nt->MemoryType == LoaderFree) block->FreePages += nt->PageCount;
        else block->ReservedPages += nt->PageCount;
    }
    block->DescriptorCount = count;
    return STATUS_SUCCESS;
}
