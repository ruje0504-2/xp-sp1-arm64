/* ARM64 resident Phase 0 physical runs, MMPFN database and primary free list. */
#include "bootframes_private.h"
#include "bootpool.h"

PPHYSICAL_MEMORY_DESCRIPTOR MmPhysicalMemoryBlock;
PFN_COUNT MmNumberOfPhysicalPages, MmAvailablePages;
PFN_NUMBER MmLowestPhysicalPage, MmHighestPhysicalPage;
PFN_NUMBER MmSystemLockPagesCount;
PMMPFN MmPfnDatabase;
MMPFNLIST MmFreePageListHead = {0, FreePageList, MM_EMPTY_LIST, MM_EMPTY_LIST};
static PVOID *owners; /* Port ownership separate from the original MMPFN. */
static PPHYSICAL_MEMORY_DESCRIPTOR resident_memory;
static MM_ARM64_BOOT_PFN_COUNTS counts;

static BOOLEAN locked(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return KeNumberProcessors == 1 && (daif & 0x3c0) == 0x3c0;
}

NTSTATUS MmArm64InitializeBootFrames(PXP_ARM64_BOOT_BLOCK block)
{
    if (!block || !block->Tables.Active || owners || !locked() ||
        KeGetCurrentIrql() != HIGH_LEVEL || !block->BootPfnMetadataPages ||
        block->BootPfnPages <= block->BootPfnMetadataPages + 1)
        return STATUS_INVALID_DEVICE_STATE;

    SIZE_T headers = BootFrameDescriptorBytes();
    ULONG64 base = (block->SharedPagePfn + 1) << PAGE_SHIFT;
    ULONG64 capacity = ((ULONG64)block->BootPfnPages - 1 - block->BootPfnMetadataPages) << PAGE_SHIFT;
    PFN_NUMBER present = MiPagesInLoaderBlock(&block->Nt, (PBOOLEAN)boot_frame_types);
    if (capacity < headers || !present || present > MAXULONG ||
        present > (capacity - headers) / sizeof(PVOID))
        return STATUS_INVALID_DEVICE_STATE;

    PPHYSICAL_MEMORY_DESCRIPTOR memory = (PVOID)(ULONG_PTR)base;
    memory->NumberOfRuns = XP_ARM64_BOOT_DESCRIPTOR_CAPACITY;
    if (MmInitializeMemoryLimits(&block->Nt, (PBOOLEAN)boot_frame_types, memory) != memory ||
        !memory->NumberOfRuns || memory->NumberOfPages != present)
        return STATUS_INVALID_DEVICE_STATE;

    MmPfnDatabase = (PMMPFN)MM_BOOT_PFN_DATABASE_BASE;
    MmLowestPhysicalPage = memory->Run[0].BasePage;
    const PHYSICAL_MEMORY_RUN *last = &memory->Run[memory->NumberOfRuns - 1];
    MmHighestPhysicalPage = last->BasePage + last->PageCount - 1;

    PFN_NUMBER initialized = 0, excluded = 0;
    PLIST_ENTRY head = &block->Nt.MemoryDescriptorListHead;
    for (PLIST_ENTRY next = head->Flink; next != head; next = next->Flink) {
        const MEMORY_ALLOCATION_DESCRIPTOR *range = CONTAINING_RECORD(
            next, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        TYPE_OF_MEMORY type = range->MemoryType;
        if ((ULONG)type >= LoaderMaximum || !boot_frame_types[type]) continue;
        for (ULONG i = 0; i < range->PageCount; ++i) {
            PFN_NUMBER frame = (PFN_NUMBER)range->BasePage + i;
            PMMPFN pfn = MI_PFN_ELEMENT(frame);
            ++initialized;
            if (type != LoaderFree) {
                /* Boot allocations are pinned. Their full hierarchy of
                 * mapping references will be imported by the general MM. */
                pfn->u3.e1.PageLocation = ActiveAndValid;
                pfn->u3.e2.ReferenceCount = 1;
                pfn->u3.e1.CacheAttribute = MiCached;
                continue;
            }
            ULONG64 physical = frame << PAGE_SHIFT;
            ULONG64 par = MmArm64QueryTranslation(physical, TRUE);
            if (!frame || (par & 1) || (par & 0x0000fffffffff000ULL) != physical ||
                (par >> 56) != 255 || ((par >> 7) & 3) != 3) {
                pfn->u3.e1.PageLocation = BadPageList;
                pfn->u3.e1.CacheAttribute = MiNotMapped;
                ++excluded;
                continue;
            }
            MiArm64InsertBootFreePage(frame);
        }
    }
    if (initialized != present || MmAvailablePages + excluded != block->FreePages ||
        !MmAvailablePages || MmAvailablePages != MmFreePageListHead.Total)
        return STATUS_INVALID_DEVICE_STATE;
    MmPhysicalMemoryBlock = memory;
    MmNumberOfPhysicalPages = (PFN_COUNT)present;
    counts.Present = present;
    counts.Inaccessible = excluded;
    owners = (PVOID *)(ULONG_PTR)(base + headers);
    return STATUS_SUCCESS;
}

NTSTATUS MmArm64PromotePhysicalMemoryDescriptor(PXP_ARM64_BOOT_BLOCK block)
{
    if (!block || KeLoaderBlock != &block->Nt || !owners || !locked() ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (resident_memory) return STATUS_SUCCESS;
    PPHYSICAL_MEMORY_DESCRIPTOR boot = MmPhysicalMemoryBlock;
    PPHYSICAL_MEMORY_DESCRIPTOR pool = MmInitializeMemoryLimits(
        &block->Nt, (PBOOLEAN)boot_frame_types, NULL);
    if (!pool) return STATUS_INSUFFICIENT_RESOURCES;
    BOOLEAN matches = pool->NumberOfPages == boot->NumberOfPages &&
                      pool->NumberOfRuns == boot->NumberOfRuns;
    for (ULONG i = 0; matches && i < boot->NumberOfRuns; ++i) {
        matches = pool->Run[i].BasePage == boot->Run[i].BasePage &&
                  pool->Run[i].PageCount == boot->Run[i].PageCount;
    }
    if (matches) {
        MmPhysicalMemoryBlock = resident_memory = pool;
        return STATUS_SUCCESS;
    }
    ExFreePool(pool);
    return STATUS_INVALID_DEVICE_STATE;
}

static PVOID *owner_slot(PFN_NUMBER frame)
{
    PFN_NUMBER index = 0;
    for (ULONG i = 0; i < MmPhysicalMemoryBlock->NumberOfRuns; ++i) {
        const PHYSICAL_MEMORY_RUN *run = &MmPhysicalMemoryBlock->Run[i];
        if (frame < run->BasePage) return NULL;
        PFN_NUMBER offset = frame - run->BasePage;
        if (offset < run->PageCount) return &owners[index + offset];
        index += run->PageCount;
    }
    return NULL;
}

static VOID release_frame(PVOID *slot, PFN_NUMBER frame)
{
    RtlZeroMemory(MI_PFN_ELEMENT(frame), sizeof(MMPFN));
    *slot = NULL;
    --counts.Allocated;
    MiArm64InsertBootFreePage(frame);
}

NTSTATUS MmArm64PinBootFrame(PFN_NUMBER frame, volatile ULONG64 *pte, BOOLEAN write)
{
    if (!owners || !locked()) return STATUS_INVALID_DEVICE_STATE;
    PVOID *slot = owner_slot(frame);
    if (!slot || !*slot) return STATUS_ACCESS_VIOLATION;
    PMMPFN pfn = MI_PFN_ELEMENT(frame);
    if (pfn->PteAddress != (PMMPTE)pte || pfn->u2.ShareCount != 1 ||
        pfn->u3.e1.PageLocation != ActiveAndValid || !pfn->u3.e2.ReferenceCount)
        return STATUS_ACCESS_VIOLATION;
    /* iosup.c charges only the first MDL reference to a mapped page. */
    if (pfn->u3.e2.ReferenceCount >= MmReferenceCountCheck) return STATUS_WORKING_SET_QUOTA;
    if (pfn->u3.e2.ReferenceCount == 1) {
        pfn->u4.LockCharged = 1;
        ++MmSystemLockPagesCount;
    }
    ++pfn->u3.e2.ReferenceCount;
    if (write) pfn->u3.e1.Modified = 1;
    return STATUS_SUCCESS;
}

BOOLEAN MmArm64BootFramePinned(PFN_NUMBER frame)
{
    if (!owners || !locked() || !owner_slot(frame)) return FALSE;
    PMMPFN pfn = MI_PFN_ELEMENT(frame);
    return pfn->u4.LockCharged && pfn->u3.e2.ReferenceCount > pfn->u2.ShareCount;
}

VOID MmArm64UnpinBootFrame(PFN_NUMBER frame)
{
    if (!MmArm64BootFramePinned(frame))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x38, frame, 0, 0);
    PMMPFN pfn = MI_PFN_ELEMENT(frame);
    if (--pfn->u3.e2.ReferenceCount == pfn->u2.ShareCount) {
        pfn->u4.LockCharged = 0;
        --MmSystemLockPagesCount;
        if (!pfn->u3.e2.ReferenceCount) release_frame(owner_slot(frame), frame);
    }
}

NTSTATUS MmArm64TakeBootFrame(PVOID owner, PPFN_NUMBER frame)
{
    if (!owner || !frame) return STATUS_INVALID_PARAMETER;
    if (!owners || !locked()) return STATUS_INVALID_DEVICE_STATE;
    PFN_NUMBER page = MmFreePageListHead.Flink;
    if (MmAvailablePages <= MiArm64BootDemandZeroPages)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (page == MM_EMPTY_LIST)
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, page, MmAvailablePages, 0);
    PVOID *slot = owner_slot(page);
    if (!slot || *slot) KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, page, (ULONG_PTR)slot, 0);
    PMMPFN pfn = MI_PFN_ELEMENT(page);
    if (pfn->u3.e1.PageLocation != FreePageList || pfn->u3.e2.ReferenceCount ||
        pfn->u2.Blink != MM_EMPTY_LIST)
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, page, pfn->u3.e2.ShortFlags, 0);

    MiArm64RemoveBootFreePage(page);
    pfn->u3.e1.PageLocation = ActiveAndValid;
    pfn->u3.e2.ReferenceCount = 1;
    *slot = owner;
    ++counts.Allocated;
    *frame = page;
    return STATUS_SUCCESS;
}

VOID MmArm64RecordBootMapping(PVOID owner, PFN_NUMBER frame, volatile ULONG64 *pte,
                              BOOLEAN stack)
{
    if (!MmArm64BootFrameOwnedBy(frame, owner) || !pte ||
        (*pte & 0x0000fffffffff003ULL) != ((frame << PAGE_SHIFT) | 3))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, frame, (ULONG_PTR)pte, 1);
    PMMPFN pfn = MI_PFN_ELEMENT(frame);
    if (pfn->PteAddress || pfn->u2.ShareCount)
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, frame, (ULONG_PTR)pfn->PteAddress, 2);
    pfn->PteAddress = (PMMPTE)pte;
    pfn->u4.PteFrame = (ULONG_PTR)pte >> PAGE_SHIFT;
    pfn->u4.KernelStack = stack;
    pfn->u3.e1.CacheAttribute = MiCached;
    pfn->u2.ShareCount = 1;
}

NTSTATUS MmArm64ReturnBootFrame(PVOID owner, PFN_NUMBER frame)
{
    if (!owner) return STATUS_INVALID_PARAMETER;
    if (!owners || !locked()) return STATUS_INVALID_DEVICE_STATE;
    PVOID *slot = owner_slot(frame);
    if (!slot || *slot != owner) return STATUS_INVALID_HANDLE;
    PMMPFN pfn = MI_PFN_ELEMENT(frame);
    if (pfn->PteAddress && *(volatile ULONG64 *)pfn->PteAddress)
        return STATUS_DEVICE_BUSY;
    if (pfn->u4.LockCharged) {
        /* Drop the primary mapping reference. MDLs retain the PFN until
         * their last system alias has been retired and their locks released. */
        if (pfn->u2.ShareCount != 1 || pfn->u3.e2.ReferenceCount < 2)
            KeBugCheckEx(MEMORY_MANAGEMENT, 0x38, frame, pfn->u2.ShareCount, pfn->u3.e2.ReferenceCount);
        pfn->PteAddress = NULL;
        pfn->u4.PteFrame = 0;
        pfn->u2.ShareCount = 0;
        pfn->u3.e1.PageLocation = TransitionPage;
        --pfn->u3.e2.ReferenceCount;
        *slot = NULL;
    } else {
        release_frame(slot, frame);
    }
    return STATUS_SUCCESS;
}

BOOLEAN MmArm64BootFrameOwnedBy(PFN_NUMBER frame, PVOID owner)
{
    if (!owners || !locked() || !owner) return FALSE;
    PVOID *slot = owner_slot(frame);
    return slot && *slot == owner;
}

MM_ARM64_BOOT_PFN_COUNTS MmArm64BootFrameCounts(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    MM_ARM64_BOOT_PFN_COUNTS result = counts;
    result.Free = MmAvailablePages;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return result;
}
