/* In-kernel controls for the original XP nonpaged pool allocator. */
#include <ntos.h>
#include <pool.h>
#include "../base/ntos/ob/arm64/bootobject.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/ps/arm64/bootexit.h"
#include "../base/ntos/ps/arm64/bootprocess.h"
#include "../base/ntos/mm/arm64/bootframes.h"
#include "uefi_test.h"

extern POOL_DESCRIPTOR NonPagedPoolDescriptor;
extern PPOOL_DESCRIPTOR ExpPagedPoolDescriptor[];
extern PFAST_MUTEX ExpPagedPoolMutex;
extern PPOOL_TRACKER_BIG_PAGES PoolBigPageTable;
extern SIZE_T PoolTrackTableSize, PoolBigPageTableSize;
extern ULONG ExPoolFailures;
VOID XpCheckStackFault(ULONG_PTR Address);

static PPOOL_HEADER header(PVOID allocation)
{
    return (PPOOL_HEADER)allocation - 1;
}

static const POOL_TRACKER_TABLE *find_tag(ULONG tag)
{
    for (SIZE_T i = 0; i < PoolTrackTableSize; ++i)
        if (PoolTrackTable[i].Key == (tag & ~PROTECTED_POOL)) return &PoolTrackTable[i];
    CHECK(FALSE);
    return NULL;
}

static __declspec(noinline) NTSTATUS raise_on_exhaustion(POOL_TYPE pool)
{
    try {
        ExAllocatePoolWithTag(pool | POOL_RAISE_IF_ALLOCATION_FAILURE,
                              128, 'esrP');
    } except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

static ULONG fill_pool_gaps(POOL_TYPE type, PVOID *pages, ULONG count, ULONG tag,
                            PVOID *fillers, ULONG capacity)
{
    SIZE_T gap_bytes[16];
    ULONG used = 0;
    for (ULONG i = 0; i < count; ++i) {
        PUCHAR page = pages[i];
        BOOLEAN seen = FALSE;
        for (ULONG n = 0; n < i; ++n) seen |= pages[n] == page;
        if (seen) continue;
        for (SIZE_T offset = 0; offset < PAGE_SIZE;) {
            const POOL_HEADER *block = (PVOID)(page + offset);
            SIZE_T bytes = block->BlockSize * POOL_SMALLEST_BLOCK;
            CHECK(bytes && bytes <= PAGE_SIZE - offset);
            if (!block->PoolType && bytes > POOL_OVERHEAD) {
                CHECK(used < (sizeof(gap_bytes) / sizeof(*gap_bytes)) && used < capacity);
                gap_bytes[used++] = bytes - POOL_OVERHEAD;
            }
            offset += bytes;
        }
    }
    for (ULONG i = 0; i < used; ++i) {
        fillers[i] = ExAllocatePoolWithTag(type, gap_bytes[i], tag);
        CHECK(fillers[i]);
        BOOLEAN same = FALSE;
        for (ULONG n = 0; n < count; ++n) same |= PAGE_ALIGN(fillers[i]) == pages[n];
        CHECK(same);
    }
    return used;
}

VOID XpCheckExecutivePool(VOID)
{
    const ULONG small_tag = 'lmsP';
    const ULONG big_tag = 'gibP' | PROTECTED_POOL;
    const ULONG filler_tag = 'lifP';
    PETHREAD idle = CONTAINING_RECORD(KeGetCurrentThread(), ETHREAD, Tcb);
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    POOL_DESCRIPTOR *pool = &NonPagedPoolDescriptor;
    ULONG initial_pages = pool->TotalPages;
    ULONG descriptor_pages = BYTES_TO_PAGES(2 * (sizeof(POOL_DESCRIPTOR) + sizeof(FAST_MUTEX)));
    ULONG initial_allocs = pool->RunningAllocs, initial_frees = pool->RunningDeAllocs;
    ULONG initial_failures = ExPoolFailures;
    CHECK((ULONG_PTR)PAGE_ALIGN(MM_BOOT_POOL_BASE + PAGE_SIZE - 1) == MM_BOOT_POOL_BASE);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && pool->PoolType == NonPagedPool);
    CHECK(pool->LockAddress && initial_pages > 0 && pool->TotalBigPages == descriptor_pages);
    CHECK(header(idle)->PoolTag == 'erhT' && header(idle)->PreviousSize == 0);
    CHECK(find_tag('erhT')->NonPagedBytes == EX_REAL_POOL_USAGE(sizeof(*idle)));
    PPHYSICAL_MEMORY_DESCRIPTOR runs = MmPhysicalMemoryBlock;
    CHECK(PAGE_ALIGN(runs) == PAGE_ALIGN(idle) && header(runs)->PoolTag == 'lMmM');
    const POOL_TRACKER_TABLE *limits = find_tag('lMmM');
    SIZE_T run_bytes = FIELD_OFFSET(PHYSICAL_MEMORY_DESCRIPTOR, Run) +
        runs->NumberOfRuns * sizeof(*runs->Run);
    CHECK(limits->NonPagedAllocs == 2 && limits->NonPagedFrees == 1 &&
          limits->NonPagedBytes == EX_REAL_POOL_USAGE(run_bytes));
    ULONG tracker_pages = BYTES_TO_PAGES(PoolTrackTableSize * sizeof(*PoolTrackTable)) +
        BYTES_TO_PAGES(PoolBigPageTableSize * sizeof(*PoolBigPageTable));
    CHECK(find_tag('looP')->NonPagedBytes == (SIZE_T)(tracker_pages + descriptor_pages) * PAGE_SIZE);

    /* Keep the resident idle, memory-run and object-type pages apart
     * from the page this control must be able to retire. */
    PVOID pages[] = {PAGE_ALIGN(idle), PAGE_ALIGN(OBJECT_TO_OBJECT_HEADER(ObpTypeObjectType)),
                     PAGE_ALIGN(OBJECT_TO_OBJECT_HEADER(ObpDirectoryObjectType)),
                     PAGE_ALIGN(OBJECT_TO_OBJECT_HEADER(ObpSymbolicLinkObjectType)),
                     PAGE_ALIGN(OBJECT_TO_OBJECT_HEADER(SeTokenObjectType)),
                     PAGE_ALIGN(OBJECT_TO_OBJECT_HEADER(PsProcessType)),
                     PAGE_ALIGN(OBJECT_TO_OBJECT_HEADER(PsThreadType))};
    PVOID fillers[16];
    ULONG gap_count = fill_pool_gaps(NonPagedPool, pages, (sizeof(pages) / sizeof(*pages)), filler_tag,
                                    fillers, (sizeof(fillers) / sizeof(*fillers)));
    KIRQL previous;
    KeRaiseIrql(DISPATCH_LEVEL, &previous);
    CHECK(previous == PASSIVE_LEVEL);
    PUCHAR a = ExAllocatePoolWithTag(NonPagedPool, 96, small_tag);
    PUCHAR b = ExAllocatePoolWithTag(NonPagedPool, 160, small_tag);
    PUCHAR c = ExAllocatePoolWithTag(NonPagedPool, 288, small_tag);
    CHECK(a && b && c && a < c && c < b);
    CHECK(PAGE_ALIGN(a) == PAGE_ALIGN(b) && PAGE_ALIGN(a) == PAGE_ALIGN(c));
    CHECK(PAGE_ALIGN(a) != PAGE_ALIGN(idle) && pool->TotalPages == initial_pages + 1);
    CHECK(header(a)->PoolTag == small_tag && header(b)->PoolTag == small_tag &&
          header(c)->PoolTag == small_tag && header(a)->PreviousSize == 0);
    BOOLEAN quota;
    CHECK(ExQueryPoolBlockSize(c, &quota) == 288 && !quota);
    const POOL_TRACKER_TABLE *small = find_tag(small_tag);
    CHECK(small->NonPagedAllocs == 3 && !small->NonPagedFrees);
    CHECK(small->NonPagedBytes ==
          EX_REAL_POOL_USAGE(96) + EX_REAL_POOL_USAGE(160) + EX_REAL_POOL_USAGE(288));
    c[0] = 0xa5;
    c[287] = 0x5a;
    ULONG_PTR retired_small = (ULONG_PTR)PAGE_ALIGN(a);
    ExFreePoolWithTag(a, small_tag);
    /* The next allocation splits the larger free block at its front. */
    PVOID front = ExAllocatePoolWithTag(NonPagedPool, 32, small_tag);
    CHECK(front == a && header(front)->PreviousSize == 0);
    ExFreePoolWithTag(front, small_tag);
    ExFreePoolWithTag(b, small_tag);
    PVOID exact = ExAllocatePoolWithTag(NonPagedPool, 160, small_tag);
    CHECK(exact == b);
    ExFreePoolWithTag(exact, small_tag);
    CHECK(c[0] == 0xa5 && c[287] == 0x5a);
    ExFreePoolWithTag(c, small_tag);
    CHECK(pool->TotalPages == initial_pages && small->NonPagedAllocs == 5 &&
          small->NonPagedFrees == 5 && !small->NonPagedBytes);
    CHECK(KeGetCurrentIrql() == DISPATCH_LEVEL);
    KeLowerIrql(previous);
    XpCheckStackFault(retired_small);

    /* The largest small allocation leaves a one-unit header-only fragment. */
    PVOID max_small = ExAllocatePoolWithTag(NonPagedPool, POOL_BUDDY_MAX, small_tag);
    CHECK(max_small && PAGE_ALIGN(max_small) != max_small);
    CHECK(header(max_small)->BlockSize == POOL_LIST_HEADS - 1);
    ExFreePoolWithTag(max_small, small_tag);
    CHECK(pool->TotalPages == initial_pages);

    volatile UCHAR *big = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE + 1, big_tag);
    CHECK(big && PAGE_ALIGNED(big) && pool->TotalBigPages == descriptor_pages + 2);
    BOOLEAN found = FALSE;
    for (SIZE_T i = 0; i < PoolBigPageTableSize; ++i) {
        if (PoolBigPageTable[i].Va != (PVOID)big) continue;
        CHECK(PoolBigPageTable[i].Key == big_tag && PoolBigPageTable[i].NumberOfPages == 2);
        found = TRUE;
        break;
    }
    CHECK(found);
    const POOL_TRACKER_TABLE *large = find_tag(big_tag);
    CHECK(large->NonPagedBytes == 2 * PAGE_SIZE && large->NonPagedAllocs == 1);
    big[0] = 0x42;
    big[PAGE_SIZE] = 0x84;
    CHECK(big[0] == 0x42 && big[PAGE_SIZE] == 0x84);
    ExFreePoolWithTag((PVOID)big, big_tag);
    CHECK(pool->TotalBigPages == descriptor_pages && !large->NonPagedBytes && large->NonPagedFrees == 1);
    XpCheckStackFault((ULONG_PTR)big);
    XpCheckStackFault((ULONG_PTR)big + PAGE_SIZE);

    ULONG remaining = MM_BOOT_POOL_PAGES - tracker_pages - initial_pages - descriptor_pages;
    PVOID full = ExAllocatePool(NonPagedPool, (SIZE_T)remaining * PAGE_SIZE);
    CHECK(full && pool->TotalBigPages == remaining + descriptor_pages);
    CHECK(!ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, big_tag));
    CHECK(!ExAllocatePoolWithTag(NonPagedPool, 128, small_tag));
    CHECK(raise_on_exhaustion(NonPagedPool) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(ExPoolFailures == initial_failures + 3 && KeGetCurrentIrql() == PASSIVE_LEVEL);
    ExFreePool(full);
    for (ULONG i = 0; i < gap_count; ++i) ExFreePoolWithTag(fillers[i], filler_tag);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    CHECK(pool->TotalPages == initial_pages && pool->TotalBigPages == descriptor_pages);
    CHECK(pool->RunningAllocs - initial_allocs == pool->RunningDeAllocs - initial_frees);
    CHECK(find_tag('erhT')->NonPagedBytes == EX_REAL_POOL_USAGE(sizeof(*idle)));
    CHECK(MmPhysicalMemoryBlock == runs);

    XpTestReport("Memory limits evidence address="); XpTestHex((ULONG_PTR)runs);
    XpTestReport(" runs="); XpTestHex(runs->NumberOfRuns);
    XpTestReport(" managed="); XpTestHex(runs->NumberOfPages);
    XpTestReport(" tag_allocs="); XpTestHex(limits->NonPagedAllocs);
    XpTestReport(" tag_frees="); XpTestHex(limits->NonPagedFrees); XpTestReport("\n");

    XpTestReport("Executive pool evidence idle="); XpTestHex((ULONG_PTR)idle);
    XpTestReport(" tracker_pages="); XpTestHex(tracker_pages);
    XpTestReport(" paged_descriptor_pages="); XpTestHex(descriptor_pages);
    XpTestReport(" resident_small_pages="); XpTestHex(initial_pages);
    XpTestReport(" small_allocs="); XpTestHex(small->NonPagedAllocs);
    XpTestReport(" small_frees="); XpTestHex(small->NonPagedFrees);
    XpTestReport(" full_big_pages="); XpTestHex(remaining);
    XpTestReport(" failures="); XpTestHex(ExPoolFailures - initial_failures);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP nonpaged pool descriptor, live idle allocation, tagged split/merge and exhaustion\n");
}

static ETHREAD pool_waiter;

static VOID paged_pool_waiter(PVOID parameter)
{
    PETHREAD thread = parameter;
    const ULONG tag = 'rwPP';
    volatile UCHAR *block = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, tag);
    CHECK(block && KeGetCurrentIrql() == PASSIVE_LEVEL && PAGE_ALIGNED(block));
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)block) == ~0ULL);
    block[0] = 0xa5;
    CHECK(block[0] == 0xa5);
    ExFreePoolWithTag((PVOID)block, tag);
    PsArm64ExitBootThread(thread, STATUS_SUCCESS);
}

static VOID start_waiter(PKSTART_ROUTINE routine, PVOID argument)
{
    routine(argument);
    CHECK(FALSE);
}

static VOID check_paged_pool_lock(PKPROCESS process)
{
    ULONG stacks = process->StackCount;
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = KeInitializeThread(&pool_waiter.Tcb, NULL, start_waiter,
                                         paged_pool_waiter, &pool_waiter, NULL, NULL, process);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(status == STATUS_SUCCESS);
    KeSetPriorityThread(&pool_waiter.Tcb, 19);
    KIRQL previous = ExLockPool(PagedPool);
    CHECK(previous == PASSIVE_LEVEL && KeGetCurrentIrql() == APC_LEVEL &&
          ExpPagedPoolMutex->Owner == KeGetCurrentThread() && ExpPagedPoolMutex->Count == 0);
    KeReadyThread(&pool_waiter.Tcb);
    CHECK(pool_waiter.Tcb.State == Waiting && pool_waiter.Tcb.WaitReason == WrExecutive &&
          pool_waiter.Tcb.WaitBlockList->Object == &ExpPagedPoolMutex->Event &&
          ExpPagedPoolMutex->Count == -1 && ExpPagedPoolMutex->Contention == 1);
    ExUnlockPool(PagedPool, previous);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    CHECK(KeWaitForSingleObject(&pool_waiter.Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);

    KEVENT park;
    LARGE_INTEGER grace;
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    grace.QuadPart = -1000000;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &grace) == STATUS_TIMEOUT);
    KiArm64DisableTimerDispatch();
    CHECK(process->StackCount == stacks && !PsReaperList && !pool_waiter.Tcb.InitialStack &&
          pool_waiter.Tcb.State == Terminated && pool_waiter.ExitStatus == STATUS_SUCCESS);
    CHECK(ExpPagedPoolMutex->Count == 1 && !ExpPagedPoolMutex->Owner &&
          IsListEmpty(&ExpPagedPoolMutex->Event.Header.WaitListHead));
}

VOID XpCheckExecutivePagedPool(PKPROCESS process)
{
    const ULONG small_tag = 'smPP';
    const ULONG big_tag = 'gbPP' | PROTECTED_POOL;
    PPOOL_DESCRIPTOR master = ExpPagedPoolDescriptor[0];
    PPOOL_DESCRIPTOR small = ExpPagedPoolDescriptor[1];
    CHECK(master && small == master + 1 && master->LockAddress == ExpPagedPoolMutex &&
          small->LockAddress != master->LockAddress && !master->PoolIndex && small->PoolIndex == 1);
    ULONG initial_small_pages = small->TotalPages;
    ULONG initial_big_pages = master->TotalBigPages;
    CHECK(master->PoolType == PagedPool && small->PoolType == PagedPool &&
          master->RunningAllocs == 2 && small->RunningAllocs >= 4 &&
          initial_big_pages == 2 && initial_small_pages > 0);
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG initial_failures = ExPoolFailures;
    ULONG initial_small_allocs = small->RunningAllocs;
    ULONG initial_small_frees = small->RunningDeAllocs;
    PVOID resident[16], fillers[16];
    ULONG resident_count = 0;
    for (ULONG i = 0; i < MM_BOOT_PAGED_POOL_PAGES; ++i) {
        ULONG_PTR page = MM_BOOT_PAGED_POOL_BASE + i * PAGE_SIZE;
        if (page == ObpKernelHandleTable->TableCode || page == PspCidTable->TableCode ||
            MmArm64QueryPhysicalAddress(page) == ~0ULL)
            continue;
        CHECK(resident_count < sizeof(resident) / sizeof(*resident));
        resident[resident_count++] = (PVOID)page;
    }
    CHECK(resident_count == initial_small_pages);
    ULONG filler_count = fill_pool_gaps(PagedPool, resident, resident_count, 'flPP',
                                        fillers, (sizeof(fillers) / sizeof(*fillers)));
    KIRQL previous;
    KeRaiseIrql(APC_LEVEL, &previous);

    PUCHAR a = ExAllocatePoolWithTag(PagedPool, 96, small_tag);
    PUCHAR b = ExAllocatePoolWithTag(PagedPool, 160, small_tag);
    PUCHAR c = ExAllocatePoolWithTag(PagedPoolCacheAligned, 288, small_tag);
    CHECK(a && b && c && a < c && c < b && PAGE_ALIGN(a) == PAGE_ALIGN(b) &&
          PAGE_ALIGN(a) == PAGE_ALIGN(c) && small->TotalPages == initial_small_pages + 1 && !master->TotalPages);
    CHECK(PAGE_ALIGN(a) != PAGE_ALIGN(ObpKernelHandleTable) &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)a) != ~0ULL);
    CHECK(header(a)->PoolTag == small_tag && header(b)->PoolTag == small_tag &&
          header(c)->PoolTag == small_tag && header(a)->PreviousSize == 0 &&
          header(a)->PoolIndex == 1 && header(a)->PoolType == 6);
    BOOLEAN quota;
    CHECK(ExQueryPoolBlockSize(c, &quota) == 288 && !quota);
    const POOL_TRACKER_TABLE *tag = find_tag(small_tag);
    CHECK(tag->PagedAllocs == 3 && !tag->PagedFrees && !tag->NonPagedAllocs &&
          tag->PagedBytes == EX_REAL_POOL_USAGE(96) + EX_REAL_POOL_USAGE(160) + EX_REAL_POOL_USAGE(288));
    c[0] = 0xa5;
    c[287] = 0x5a;
    ExFreePoolWithTag(a, small_tag);
    PVOID front = ExAllocatePoolWithTag(PagedPool, 32, small_tag);
    CHECK(front == a);
    ExFreePoolWithTag(front, small_tag);
    ExFreePoolWithTag(b, small_tag);
    PVOID exact = ExAllocatePoolWithTag(PagedPool, 160, small_tag);
    CHECK(exact == b && c[0] == 0xa5 && c[287] == 0x5a);
    ExFreePoolWithTag(exact, small_tag);
    ExFreePoolWithTag(c, small_tag);
    CHECK(small->TotalPages == initial_small_pages && !tag->PagedBytes);
    XpCheckStackFault((ULONG_PTR)PAGE_ALIGN(a));
    CHECK(KeGetCurrentIrql() == APC_LEVEL);
    KeLowerIrql(previous);

    PVOID max_small = ExAllocatePoolWithTag(PagedPool, POOL_BUDDY_MAX, small_tag);
    CHECK(max_small && !PAGE_ALIGNED(max_small));
    ExFreePoolWithTag(max_small, small_tag);
    CHECK(small->RunningAllocs == initial_small_allocs + 6 + filler_count &&
          small->RunningDeAllocs == initial_small_frees + 6 &&
          tag->PagedAllocs == 6 && tag->PagedFrees == 6 && !tag->PagedBytes);

    volatile UCHAR *big = ExAllocatePoolWithTag(PagedPool, 2 * PAGE_SIZE + 1, big_tag);
    CHECK(big && PAGE_ALIGNED(big) && master->TotalBigPages == initial_big_pages + 3 &&
          ExQueryPoolBlockSize((PVOID)big, &quota) == PAGE_SIZE && !quota);
    for (ULONG i = 0; i < 3; ++i)
        CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)big + i * PAGE_SIZE) == ~0ULL);
    BOOLEAN found = FALSE;
    for (SIZE_T i = 0; i < PoolBigPageTableSize; ++i) {
        if (PoolBigPageTable[i].Va != (PVOID)big) continue;
        CHECK(PoolBigPageTable[i].Key == big_tag && PoolBigPageTable[i].NumberOfPages == 3);
        found = TRUE;
        break;
    }
    CHECK(found);
    const POOL_TRACKER_TABLE *large = find_tag(big_tag);
    CHECK(large->PagedAllocs == 1 && large->PagedBytes == 3 * PAGE_SIZE && !large->NonPagedBytes);
    CHECK(big[PAGE_SIZE - 1] == 0);
    big[PAGE_SIZE + 1] = 0x29;
    CHECK(big[PAGE_SIZE + 1] == 0x29 && MmArm64QueryPhysicalAddress((ULONG_PTR)big + 2 * PAGE_SIZE) == ~0ULL);
    ExFreePoolWithTag((PVOID)big, big_tag);
    CHECK(master->TotalBigPages == initial_big_pages && !large->PagedBytes && large->PagedFrees == 1);
    XpCheckStackFault((ULONG_PTR)big);
    XpCheckStackFault((ULONG_PTR)big + PAGE_SIZE);

    PVOID untouched = ExAllocatePool(PagedPool, PAGE_SIZE);
    CHECK(untouched && PAGE_ALIGNED(untouched) && MmArm64QueryPhysicalAddress((ULONG_PTR)untouched) == ~0ULL);
    ExFreePool(untouched);
    const SIZE_T capacity = (MM_BOOT_PAGED_POOL_PAGES - initial_small_pages - initial_big_pages) * PAGE_SIZE;
    PVOID full = ExAllocatePool(PagedPool, capacity);
    CHECK(full && master->TotalBigPages == MM_BOOT_PAGED_POOL_PAGES - initial_small_pages);
    CHECK(!ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, small_tag));
    CHECK(!ExAllocatePoolWithTag(PagedPool, 128, small_tag));
    CHECK(raise_on_exhaustion(PagedPool) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(ExPoolFailures == initial_failures + 3 && KeGetCurrentIrql() == PASSIVE_LEVEL);
    ExFreePool(full);
    check_paged_pool_lock(process);
    for (ULONG i = 0; i < filler_count; ++i) ExFreePoolWithTag(fillers[i], 'flPP');

    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    CHECK(!master->TotalPages && master->TotalBigPages == initial_big_pages &&
          small->TotalPages == initial_small_pages && !small->TotalBigPages);
    CHECK(master->RunningAllocs == initial_big_pages + 4 && master->RunningDeAllocs == 4);
    XpTestReport("Executive paged pool evidence small_allocs="); XpTestHex(tag->PagedAllocs);
    XpTestReport(" small_frees="); XpTestHex(tag->PagedFrees);
    XpTestReport(" big_allocs="); XpTestHex(master->RunningAllocs - initial_big_pages);
    XpTestReport(" big_frees="); XpTestHex(master->RunningDeAllocs);
    XpTestReport(" failures="); XpTestHex(ExPoolFailures - initial_failures);
    XpTestReport(" contention="); XpTestHex(ExpPagedPoolMutex->Contention);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP executive paged pool, tagged split/merge, lazy large blocks and lock contention\n");
}
