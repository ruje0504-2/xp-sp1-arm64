#include "../base/ntos/ex/arm64/bootexec.h"
#include "../base/ntos/mm/arm64/bootframes.h"
#include "uefi_test.h"

extern GENERAL_LOOKASIDE ExpSmallNPagedPoolLookasideLists[POOL_SMALL_LISTS];
extern GENERAL_LOOKASIDE ExpSmallPagedPoolLookasideLists[POOL_SMALL_LISTS];
extern LIST_ENTRY ExNPagedLookasideListHead, ExPagedLookasideListHead, ExPoolLookasideListHead;

static NPAGED_LOOKASIDE_LIST nonpaged;
static PAGED_LOOKASIDE_LIST paged;
static struct DECLSPEC_ALIGN(16) {
    SLIST_ENTRY Link;
} entries[4];

static VOID check_boot_lists(VOID)
{
    PKPRCB prcb = KeGetCurrentPrcb();
    const LIST_ENTRY *entry = ExPoolLookasideListHead.Flink;
    CHECK(IsListEmpty(&ExNPagedLookasideListHead) && IsListEmpty(&ExPagedLookasideListHead));
    for (ULONG i = 0; i < POOL_SMALL_LISTS; ++i) {
        PGENERAL_LOOKASIDE n = &ExpSmallNPagedPoolLookasideLists[i];
        PGENERAL_LOOKASIDE p = &ExpSmallPagedPoolLookasideLists[i];
        CHECK((ULONG_PTR)n >> 48 == 0xffff && (ULONG_PTR)p >> 48 == 0xffff);
        CHECK(prcb->PPNPagedLookasideList[i].P == n && prcb->PPNPagedLookasideList[i].L == n &&
              prcb->PPPagedLookasideList[i].P == p && prcb->PPPagedLookasideList[i].L == p);
        CHECK(entry == &n->ListEntry && entry->Flink == &p->ListEntry &&
              p->ListEntry.Blink == entry);
        CHECK(n->Type == NonPagedPool && p->Type == PagedPool && n->Depth == 2 && p->Depth == 2 &&
              n->MaximumDepth == 256 && p->MaximumDepth == 256 &&
              n->Size == (i + 1) * 32 && p->Size == n->Size);
        CHECK(!QueryDepthSList(&n->ListHead) && !QueryDepthSList(&p->ListHead));
        entry = p->ListEntry.Flink;
    }
    CHECK(entry == &ExPoolLookasideListHead);
}

VOID XpCheckPhase1PoolLookasides(VOID)
{
    PKPRCB prcb = KeGetCurrentPrcb();
    PGENERAL_LOOKASIDE first = prcb->PPNPagedLookasideList[0].P;
    CHECK(first && first != prcb->PPNPagedLookasideList[0].L && !BYTE_OFFSET(first));
    const SIZE_T bytes = 2 * POOL_SMALL_LISTS * sizeof(*first);
    BOOLEAN quota;
    /* XP reports one page for a page-aligned big pool block. Verify all
     * pages of this private table through the live translation separately. */
    CHECK(ExQueryPoolBlockSize(first, &quota) == PAGE_SIZE && !quota);
    for (SIZE_T offset = 0; offset < bytes; offset += PAGE_SIZE)
        CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)first + offset) != ~0ULL);
    const LIST_ENTRY *entry = ExpSmallPagedPoolLookasideLists[POOL_SMALL_LISTS - 1].ListEntry.Flink;
    for (ULONG i = 0; i < POOL_SMALL_LISTS; ++i) {
        const PP_LOOKASIDE_LIST *pointers[2] = {
            &prcb->PPNPagedLookasideList[i], &prcb->PPPagedLookasideList[i]
        };
        PGENERAL_LOOKASIDE globals[2] = {
            &ExpSmallNPagedPoolLookasideLists[i], &ExpSmallPagedPoolLookasideLists[i]
        };
        for (ULONG type = 0; type < 2; ++type) {
            PGENERAL_LOOKASIDE list = pointers[type]->P;
            CHECK(list == &first[2 * i + type] && pointers[type]->L == globals[type] &&
                  entry == &list->ListEntry && entry->Flink->Blink == entry &&
                  list->Type == (POOL_TYPE)type && list->Depth == 2 && list->MaximumDepth == 256 &&
                  list->Size == (i + 1) * 32 && list->Tag == 'LooP' &&
                  list->Allocate == ExAllocatePoolWithTag && list->Free == ExFreePool &&
                  !list->TotalAllocates && !list->TotalFrees && !QueryDepthSList(&list->ListHead));
            entry = entry->Flink;
        }
    }
    CHECK(entry == &ExPoolLookasideListHead);
    XpTestReport("Phase 1 pool lookaside evidence private="); XpTestHex(2 * POOL_SMALL_LISTS);
    XpTestReport(" bytes="); XpTestHex(bytes);
    XpTestReport("\n");
    XpTestReport("PASS: XP Phase 1 per-processor pool lookasides and global fallback binding\n");
}

static VOID check_slist(VOID)
{
    SLIST_HEADER head;
    InitializeSListHead(&head);
    PSLIST_ENTRY a = &entries[0].Link, b = &entries[1].Link;
    PSLIST_ENTRY c = &entries[2].Link, d = &entries[3].Link;
    CHECK(!FirstEntrySList(&head) && QueryDepthSList(&head) == 0);
    KIRQL previous;
    KeRaiseIrql(DISPATCH_LEVEL, &previous);
    ULONG64 before, after;
    __asm__ volatile("mrs %0, daif" : "=r"(before) :: "memory");
    CHECK(!InterlockedPushEntrySList(&head, a));
    CHECK(InterlockedPushEntrySList(&head, b) == a && b->Next == a);
    c->Next = d;
    CHECK(InterlockedPushListSList(&head, c, d, 2) == b && d->Next == b);
    CHECK(QueryDepthSList(&head) == 4 && FirstEntrySList(&head) == c);
    CHECK(InterlockedPopEntrySList(&head) == c && QueryDepthSList(&head) == 3);
    ULONG64 sequence = head.Alignment >> 16;
    CHECK(sequence && InterlockedFlushSList(&head) == d);
    CHECK(d->Next == b && b->Next == a && !a->Next &&
          !InterlockedPopEntrySList(&head) && !FirstEntrySList(&head) &&
          !InterlockedFlushSList(&head) && !QueryDepthSList(&head) &&
          head.Alignment >> 16 == sequence);
    __asm__ volatile("mrs %0, daif" : "=r"(after) :: "memory");
    CHECK(before == after && KeGetCurrentIrql() == DISPATCH_LEVEL);
    KeLowerIrql(previous);
}

static PVOID allocate(POOL_TYPE type)
{
    if (type == PagedPool) return ExAllocateFromPagedLookasideList(&paged);
    return ExAllocateFromNPagedLookasideList(&nonpaged);
}

static VOID release(POOL_TYPE type, PVOID pointer)
{
    if (type == PagedPool) ExFreeToPagedLookasideList(&paged, pointer);
    else ExFreeToNPagedLookasideList(&nonpaged, pointer);
}

static VOID check_cache(POOL_TYPE type, PGENERAL_LOOKASIDE list)
{
    KIRQL raised = type == PagedPool ? APC_LEVEL : DISPATCH_LEVEL;
    KIRQL previous;
    KeRaiseIrql(raised, &previous);
    ULONG_PTR base = type == PagedPool ? MM_BOOT_PAGED_POOL_BASE : MM_BOOT_POOL_BASE;
    SIZE_T pages = type == PagedPool ? MM_BOOT_PAGED_POOL_PAGES : MM_BOOT_POOL_PAGES;
    PVOID blocks[6];
    for (ULONG i = 0; i < 6; ++i) {
        blocks[i] = allocate(type);
        ULONG_PTR va = (ULONG_PTR)blocks[i];
        CHECK(va - base < pages * PAGE_SIZE);
        for (ULONG j = 0; j < i; ++j) CHECK(blocks[i] != blocks[j]);
        *(volatile UCHAR *)blocks[i] = (UCHAR)i + 0x80;
    }
    CHECK(list->Depth == 4 && list->TotalAllocates == 6 && list->AllocateMisses == 6 &&
          !FirstEntrySList(&list->ListHead));
    for (ULONG i = 0; i < 6; ++i) release(type, blocks[i]);
    CHECK(list->TotalFrees == 6 && list->FreeMisses == 2 &&
          QueryDepthSList(&list->ListHead) == 4);
    for (ULONG i = 0; i < 4; ++i) CHECK(allocate(type) == blocks[3 - i]);
    CHECK(list->TotalAllocates == 10 && list->AllocateMisses == 6 &&
          !QueryDepthSList(&list->ListHead));

    for (ULONG i = 0; i < 4; ++i) release(type, blocks[i]);
    CHECK(KeGetCurrentIrql() == raised);
    KeLowerIrql(previous);
}

VOID XpCheckLookasides(VOID)
{
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    check_boot_lists();
    check_slist();
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();

    ExInitializeNPagedLookasideList(&nonpaged, NULL, NULL, 0, 320, 'nLsX', 0);
    ExInitializePagedLookasideList(&paged, NULL, NULL, 0, 320, 'pLsX', 0);
    CHECK(ExNPagedLookasideListHead.Flink == &nonpaged.L.ListEntry &&
          ExPagedLookasideListHead.Flink == &paged.L.ListEntry);
    CHECK(nonpaged.L.Allocate == ExAllocatePoolWithTag && paged.L.Allocate == ExAllocatePoolWithTag &&
          nonpaged.L.Free == ExFreePool && paged.L.Free == ExFreePool);
    check_cache(NonPagedPool, &nonpaged.L);
    check_cache(PagedPool, &paged.L);

    /* Build real misses without changing the original allocation counters. */
    PVOID n[80], p[80];
    for (ULONG i = 0; i < 80; ++i) {
        n[i] = ExAllocateFromNPagedLookasideList(&nonpaged);
        p[i] = ExAllocateFromPagedLookasideList(&paged);
        CHECK(n[i] && p[i]);
    }
    CHECK(nonpaged.L.AllocateMisses == 82 && paged.L.AllocateMisses == 82);
    for (ULONG i = 0; i < 3; ++i) ExAdjustLookasideDepth();
    USHORT peak_nonpaged = nonpaged.L.Depth, peak_paged = paged.L.Depth;
    CHECK(peak_nonpaged == 34 && peak_paged == 34 &&
          nonpaged.L.LastAllocateMisses == 82 && paged.L.LastAllocateMisses == 82);
    for (ULONG i = 0; i < 3; ++i) ExAdjustLookasideDepth();
    USHORT final_nonpaged = nonpaged.L.Depth, final_paged = paged.L.Depth;
    CHECK(final_nonpaged == 24 && final_paged == 24);
    for (ULONG i = 0; i < 80; ++i) {
        ExFreeToNPagedLookasideList(&nonpaged, n[i]);
        ExFreeToPagedLookasideList(&paged, p[i]);
    }
    CHECK(QueryDepthSList(&nonpaged.L.ListHead) == final_nonpaged &&
          QueryDepthSList(&paged.L.ListHead) == final_paged);
    ExDeleteNPagedLookasideList(&nonpaged);
    ExDeletePagedLookasideList(&paged);
    CHECK(IsListEmpty(&ExNPagedLookasideListHead) && IsListEmpty(&ExPagedLookasideListHead) &&
          !QueryDepthSList(&nonpaged.L.ListHead) && !QueryDepthSList(&paged.L.ListHead));
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Lookaside evidence bins="); XpTestHex(2 * POOL_SMALL_LISTS);
    XpTestReport(" nonpaged_peak="); XpTestHex(peak_nonpaged);
    XpTestReport(" paged_peak="); XpTestHex(peak_paged);
    XpTestReport(" nonpaged_final="); XpTestHex(final_nonpaged);
    XpTestReport(" paged_final="); XpTestHex(final_paged);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: XP Phase 0 lookasides, sequenced lists, real pool caching and depth adjustment\n");
}
