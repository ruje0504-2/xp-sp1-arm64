/* Resident and demand-zero page suppliers for the early NT_UP system pools. */
#include <ntos.h>
#include "bootpool.h"
#include "bootframes_private.h"
#include "xp-paged-pool.h"

#define POOL_DAIF_MASK 0x3c0
#define POOL_PA_MASK 0x0000fffffffff000ULL
/* Software marker in a hardware-invalid L3 PTE. Zero means unallocated. */


typedef struct _BOOT_POOL_PAGE {
    PFN_NUMBER Frame;
    ULONG AllocationPages; /* Nonzero only at the start of an allocation. */
} BOOT_POOL_PAGE;

static volatile ULONG64 *pool_ptes;
static ULONG64 pool_descriptor;
static RTL_BITMAP pool_bitmap;
static ULONG pool_bits[MM_BOOT_POOL_PAGES / 32];
static BOOT_POOL_PAGE pool_pages[MM_BOOT_POOL_PAGES];

MM_PAGED_POOL_INFO MmPagedPoolInfo;
ULONG MmPagedPoolCommit;
PFN_COUNT MiArm64BootDemandZeroPages;
static volatile ULONG64 *paged_ptes;
static ULONG paged_pool_bits[MM_BOOT_PAGED_POOL_PAGES / 32];
static ULONG paged_end_bits[MM_BOOT_PAGED_POOL_PAGES / 32];
static RTL_BITMAP paged_pool_bitmap, paged_end_bitmap;
static PFN_NUMBER paged_frames[MM_BOOT_PAGED_POOL_PAGES];

BOOLEAN MmArm64IsBootNonPagedPoolAddress(PVOID address)
{
    ULONG_PTR offset = (ULONG_PTR)address - MM_BOOT_POOL_BASE;
    return pool_ptes && offset < MM_BOOT_POOL_PAGES * PAGE_SIZE &&
           (pool_ptes[offset >> PAGE_SHIFT] & 1) != 0;
}

BOOLEAN MmArm64IsBootPagedPoolAddress(PVOID address)
{
    ULONG_PTR offset = (ULONG_PTR)address - MM_BOOT_PAGED_POOL_BASE;
    /* Large executive blocks may be freed before their first touch. */
    return paged_ptes && offset < MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE &&
           paged_ptes[offset >> PAGE_SHIFT] != 0;
}

NTSTATUS MmArm64InitializeNonPagedPool(PMM_ARM64_BOOT_TABLES tables)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (pool_ptes || !tables || !tables->Active || !tables->NonPagedPoolPtes ||
        !tables->NonPagedPoolDescriptor || KeNumberProcessors != 1 ||
        KeGetCurrentIrql() != HIGH_LEVEL || (daif & POOL_DAIF_MASK) != POOL_DAIF_MASK)
        return STATUS_INVALID_DEVICE_STATE;
    for (ULONG i = 0; i < MM_BOOT_POOL_PAGES; ++i)
        if (tables->NonPagedPoolPtes[i]) return STATUS_CONFLICTING_ADDRESSES;
    RtlInitializeBitMap(&pool_bitmap, pool_bits, MM_BOOT_POOL_PAGES);
    RtlClearAllBits(&pool_bitmap);
    pool_descriptor = tables->NonPagedPoolDescriptor;
    pool_ptes = tables->NonPagedPoolPtes;
    return STATUS_SUCCESS;
}

static ULONG64 mask_interrupts(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    return daif;
}

static VOID restore_interrupts(ULONG64 daif)
{
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}

static VOID sync_pool_mappings(volatile ULONG64 *ptes, ULONG start, ULONG pages)
{
    for (ULONG i = start; i < start + pages; ++i)
        __asm__ volatile("dc cvac, %0" :: "r"(&ptes[i]) : "memory");
    __asm__ volatile("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
}

static VOID return_pool_frames(ULONG start, ULONG pages)
{
    BOOT_POOL_PAGE *owner = &pool_pages[start];
    while (pages) {
        BOOT_POOL_PAGE *page = &pool_pages[start + --pages];
        NTSTATUS status = MmArm64ReturnBootFrame(owner, page->Frame);
        if (!NT_SUCCESS(status)) KeBugCheckEx(MEMORY_MANAGEMENT, 0x36, start, page->Frame, status);
        page->Frame = 0;
    }
    owner->AllocationPages = 0;
}

ULONG64 MmArm64BootPoolDescriptor(PFN_NUMBER frame)
{
    ULONG64 physical = frame << PAGE_SHIFT;
    ULONG64 par = MmArm64QueryTranslation(physical, TRUE);
    if (!pool_descriptor || (par & 1) || (par & POOL_PA_MASK) != physical || (par >> 56) != 255 ||
        ((par >> 7) & 3) != 3)
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x36, frame, physical, par);
    /* PAR.NS occupies bit 9, while the leaf descriptor uses bit 5. */
    return physical | pool_descriptor | ((par & (1ULL << 9)) >> 4);
}

static VOID map_zeroed_pool_page(volatile ULONG64 *pte, PVOID owner, PFN_NUMBER frame)
{
    ULONG64 descriptor = MmArm64BootPoolDescriptor(frame);
    RtlZeroMemory((PVOID)(ULONG_PTR)(frame << PAGE_SHIFT), PAGE_SIZE);
    *pte = descriptor;
    MmArm64RecordBootMapping(owner, frame, pte, FALSE);
}

NTSTATUS MmArm64InitializePagedPool(PMM_ARM64_BOOT_TABLES tables)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (paged_ptes || !pool_ptes || !tables || !tables->Active || !tables->PagedPoolPtes ||
        KeNumberProcessors != 1 || KeGetCurrentIrql() != HIGH_LEVEL ||
        (daif & POOL_DAIF_MASK) != POOL_DAIF_MASK)
        return STATUS_INVALID_DEVICE_STATE;
    for (ULONG i = 0; i < MM_BOOT_PAGED_POOL_PAGES; ++i) {
        if (tables->PagedPoolPtes[i]) return STATUS_CONFLICTING_ADDRESSES;
        paged_frames[i] = MM_EMPTY_LIST;
    }

    /* MiBuildPagedPool initially exposes one leaf's worth of PTEs. This
     * bootstrap aperture is exactly one leaf, so every bitmap bit is free. */
    RtlInitializeBitMap(&paged_pool_bitmap, paged_pool_bits, MM_BOOT_PAGED_POOL_PAGES);
    RtlClearAllBits(&paged_pool_bitmap);
    RtlInitializeBitMap(&paged_end_bitmap, paged_end_bits, MM_BOOT_PAGED_POOL_PAGES);
    RtlClearAllBits(&paged_end_bitmap);
    MmPagedPoolInfo.PagedPoolAllocationMap = &paged_pool_bitmap;
    MmPagedPoolInfo.EndOfPagedPoolBitmap = &paged_end_bitmap;
    MmPagedPoolInfo.FirstPteForPagedPool = (PMMPTE)tables->PagedPoolPtes;
    MmPagedPoolInfo.LastPteForPagedPool = MmPagedPoolInfo.FirstPteForPagedPool +
                                          MM_BOOT_PAGED_POOL_PAGES - 1;
    paged_ptes = tables->PagedPoolPtes;
    return STATUS_SUCCESS;
}

static PVOID allocate_paged_pool(SIZE_T bytes)
{
    if (!paged_ptes || KeNumberProcessors != 1 || KeGetCurrentIrql() > APC_LEVEL ||
        !bytes || bytes > MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE)
        return NULL;
    ULONG pages = (ULONG)((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
    ULONG64 daif = mask_interrupts();
    /* No pagefile exists during Phase 0. Charge every untouched page
     * against the shared real PFN supply while leaving it on the free list. */
    if (MmAvailablePages < MiArm64BootDemandZeroPages ||
        pages > MmAvailablePages - MiArm64BootDemandZeroPages) {
        restore_interrupts(daif);
        return NULL;
    }

    ULONG start = RtlFindClearBitsAndSet(MmPagedPoolInfo.PagedPoolAllocationMap, pages,
                                         MmPagedPoolInfo.PagedPoolHint);
    if (start == MAXULONG) {
        restore_interrupts(daif);
        return NULL;
    }
    if (pages == 1) MmPagedPoolInfo.PagedPoolHint = start + pages;
    for (ULONG i = start; i < start + pages; ++i)
        paged_ptes[i] = MM_ARM64_KERNEL_DEMAND_ZERO_PTE;
    RtlSetBit(MmPagedPoolInfo.EndOfPagedPoolBitmap, start + pages - 1);
    MmPagedPoolInfo.AllocatedPagedPool += pages;
    MmPagedPoolInfo.PagedPoolCommit += pages;
    MmPagedPoolCommit += pages;
    MiArm64BootDemandZeroPages += pages;

    restore_interrupts(daif);
    return (PVOID)(MM_BOOT_PAGED_POOL_BASE + ((ULONG_PTR)start << PAGE_SHIFT));
}

static BOOLEAN commit_pool_page(ULONG index)
{
    if (paged_ptes[index] != MM_ARM64_KERNEL_DEMAND_ZERO_PTE ||
        !RtlCheckBit(MmPagedPoolInfo.PagedPoolAllocationMap, index))
        return FALSE;

    PFN_NUMBER *page = &paged_frames[index];
    if (!MiArm64BootDemandZeroPages || *page != MM_EMPTY_LIST)
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x41286, index, MiArm64BootDemandZeroPages, *page);
    --MiArm64BootDemandZeroPages;
    NTSTATUS status = MmArm64TakeBootFrame(page, page);
    if (!NT_SUCCESS(status))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x41286, index, MmAvailablePages, status);
    map_zeroed_pool_page(paged_ptes + index, page, *page);
    MI_PFN_ELEMENT(*page)->OriginalPte.u.Long = MM_ARM64_KERNEL_DEMAND_ZERO_PTE;
    sync_pool_mappings(paged_ptes, index, 1);
    return TRUE;
}

BOOLEAN MmArm64ResolvePagedPoolFault(PKTRAP_FRAME frame)
{
    ULONG_PTR offset = frame->Far - MM_BOOT_PAGED_POOL_BASE;
    ULONG64 esr = frame->Esr;
    if (!paged_ptes || offset >= MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE ||
        KeNumberProcessors != 1 || frame->PreviousMode != KernelMode ||
        frame->PreviousIrql > APC_LEVEL || (frame->Cpsr & 31) != 5 ||
        ((esr >> 26) & 63) != 0x25 || (esr & 63) != 7 ||
        (esr & ((1ULL << 10) | (1ULL << 8) | (1ULL << 7))))
        return FALSE;
    ULONG64 daif = mask_interrupts();
    BOOLEAN committed = commit_pool_page((ULONG)(offset >> PAGE_SHIFT));
    restore_interrupts(daif);
    return committed;
}

/* The caller keeps normal/fast interrupts masked across capture and pin. */
NTSTATUS MmArm64PinBootPoolPage(ULONG_PTR address, BOOLEAN write, PPFN_NUMBER frame)
{
    ULONG_PTR offset = address - MM_BOOT_POOL_BASE;
    volatile ULONG64 *pte;
    if (pool_ptes && offset < MM_BOOT_POOL_PAGES * PAGE_SIZE) {
        pte = pool_ptes + (offset >> PAGE_SHIFT);
    } else {
        offset = address - MM_BOOT_PAGED_POOL_BASE;
        if (!paged_ptes || offset >= MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE ||
            KeGetCurrentIrql() > APC_LEVEL)
            return STATUS_ACCESS_VIOLATION;
        ULONG index = (ULONG)(offset >> PAGE_SHIFT);
        if (paged_ptes[index] == MM_ARM64_KERNEL_DEMAND_ZERO_PTE && !commit_pool_page(index))
            return STATUS_ACCESS_VIOLATION;
        pte = paged_ptes + index;
    }
    ULONG64 descriptor = *pte;
    ULONG64 par = MmArm64QueryTranslation(address, write);
    if ((descriptor & 3) != 3 || (par & 1) || (descriptor & POOL_PA_MASK) != (par & POOL_PA_MASK))
        return STATUS_ACCESS_VIOLATION;
    PFN_NUMBER page = (descriptor & POOL_PA_MASK) >> PAGE_SHIFT;
    NTSTATUS status = MmArm64PinBootFrame(page, pte, write);
    if (NT_SUCCESS(status)) *frame = page;
    return status;
}

static ULONG free_paged_pool(ULONG_PTR offset)
{
    if (!paged_ptes || KeNumberProcessors != 1 || (offset & (PAGE_SIZE - 1)) ||
        KeGetCurrentIrql() > APC_LEVEL)
        return 0;
    ULONG start = (ULONG)(offset >> PAGE_SHIFT);
    ULONG64 daif = mask_interrupts();
    PRTL_BITMAP allocation = MmPagedPoolInfo.PagedPoolAllocationMap;
    PRTL_BITMAP ends = MmPagedPoolInfo.EndOfPagedPoolBitmap;
    if (!RtlCheckBit(allocation, start) ||
        (start && RtlCheckBit(allocation, start - 1) && !RtlCheckBit(ends, start - 1))) {
        restore_interrupts(daif);
        return 0;
    }

    ULONG end = start;
    while (end < MM_BOOT_PAGED_POOL_PAGES && !RtlCheckBit(ends, end)) ++end;
    if (end == MM_BOOT_PAGED_POOL_PAGES)
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x41286, start, end, 0);

    for (ULONG i = start; i <= end; ++i) {
        PFN_NUMBER frame = paged_frames[i];
        if (paged_ptes[i] == MM_ARM64_KERNEL_DEMAND_ZERO_PTE &&
            frame == MM_EMPTY_LIST && MiArm64BootDemandZeroPages) {
            --MiArm64BootDemandZeroPages;
        } else if ((paged_ptes[i] & (POOL_PA_MASK | 3)) != ((frame << PAGE_SHIFT) | 3) ||
                   !MmArm64BootFrameOwnedBy(frame, &paged_frames[i])) {
            KeBugCheckEx(MEMORY_MANAGEMENT, 0x41286, i, paged_ptes[i], frame);
        }
        paged_ptes[i] = 0;
    }

    ULONG pages = end - start + 1;
    sync_pool_mappings(paged_ptes, start, pages);
    for (ULONG i = start; i <= end; ++i) {
        PFN_NUMBER *page = &paged_frames[i];
        if (*page == MM_EMPTY_LIST) continue;
        NTSTATUS status = MmArm64ReturnBootFrame(page, *page);
        if (!NT_SUCCESS(status)) KeBugCheckEx(MEMORY_MANAGEMENT, 0x41286, i, *page, status);
        *page = MM_EMPTY_LIST;
    }
    RtlClearBit(ends, end);
    RtlClearBits(allocation, start, pages);
    MmPagedPoolInfo.AllocatedPagedPool -= pages;
    MmPagedPoolInfo.PagedPoolCommit -= pages;
    MmPagedPoolCommit -= pages;
    if (start < MmPagedPoolInfo.PagedPoolHint) MmPagedPoolInfo.PagedPoolHint = start;
    restore_interrupts(daif);
    return pages;
}

PVOID MiAllocatePoolPages(POOL_TYPE type, SIZE_T bytes, ULONG session_allocation)
{
    if (type == PagedPool && !session_allocation) return allocate_paged_pool(bytes);
    if (!pool_ptes || KeNumberProcessors != 1 || type != NonPagedPool || session_allocation || !bytes ||
        bytes > MM_BOOT_POOL_PAGES * PAGE_SIZE || KeGetCurrentIrql() > DISPATCH_LEVEL)
        return NULL;

    ULONG pages = (ULONG)((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
    ULONG64 daif = mask_interrupts();
    ULONG start = RtlFindClearBitsAndSet(&pool_bitmap, pages, 0);
    if (start == MAXULONG) {
        restore_interrupts(daif);
        return NULL;
    }

    BOOT_POOL_PAGE *owner = &pool_pages[start];
    for (ULONG i = 0; i < pages; ++i) {
        NTSTATUS status = MmArm64TakeBootFrame(owner, &pool_pages[start + i].Frame);
        if (!NT_SUCCESS(status)) {
            return_pool_frames(start, i);
            RtlClearBits(&pool_bitmap, start, pages);
            restore_interrupts(daif);
            return NULL;
        }
    }

    for (ULONG i = start; i < start + pages; ++i)
        map_zeroed_pool_page(pool_ptes + i, owner, pool_pages[i].Frame);
    owner->AllocationPages = pages;
    sync_pool_mappings(pool_ptes, start, pages);
    restore_interrupts(daif);
    return (PVOID)(MM_BOOT_POOL_BASE + ((ULONG_PTR)start << PAGE_SHIFT));
}

ULONG MiFreePoolPages(PVOID address)
{
    ULONG_PTR paged_offset = (ULONG_PTR)address - MM_BOOT_PAGED_POOL_BASE;
    if (paged_offset < MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE) return free_paged_pool(paged_offset);

    ULONG_PTR offset = (ULONG_PTR)address - MM_BOOT_POOL_BASE;
    if (!pool_ptes || KeNumberProcessors != 1 || offset >= MM_BOOT_POOL_PAGES * PAGE_SIZE ||
        (offset & (PAGE_SIZE - 1)) || KeGetCurrentIrql() > DISPATCH_LEVEL)
        return 0;

    ULONG start = (ULONG)(offset >> PAGE_SHIFT);
    ULONG64 daif = mask_interrupts();
    BOOT_POOL_PAGE *owner = &pool_pages[start];
    ULONG pages = owner->AllocationPages;
    if (pages) {
        for (ULONG i = start; i < start + pages; ++i) {
            PFN_NUMBER frame = pool_pages[i].Frame;
            if (!MmArm64BootFrameOwnedBy(frame, owner))
                KeBugCheckEx(MEMORY_MANAGEMENT, 0x36, i, frame, 0);
            pool_ptes[i] = 0;
        }
        /* Stop using the high alias before these PFNs can change owner. */
        sync_pool_mappings(pool_ptes, start, pages);
        return_pool_frames(start, pages);
        RtlClearBits(&pool_bitmap, start, pages);
    }
    restore_interrupts(daif);
    return pages;
}
