#include "../base/ntos/mm/arm64/bootframes_private.h"
#include "../base/ntos/mm/arm64/bootpool.h"
#include "../base/ntos/mm/arm64/bootsyspte.h"
#include "xp-paged-pool.h"
#include "uefi_test.h"

extern MM_PAGED_POOL_INFO MmPagedPoolInfo;
extern ULONG MmPagedPoolCommit;
VOID XpCheckStackFault(ULONG_PTR Address);

typedef struct {
    MDL Mdl;
    PFN_NUMBER Frames[2];
} TWO_PAGE_MDL;

static PVOID map_test_mdl(PMDL mdl)
{
    return MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached, NULL, FALSE, NormalPagePriority);
}

static NTSTATUS try_lock_test_mdl(PMDL mdl)
{
    try {
        MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
    } except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

static VOID check_pinned_page(PFN_NUMBER frame, USHORT refs, BOOLEAN resident)
{
    PMMPFN pfn = MI_PFN_ELEMENT(frame);
    CHECK(pfn->u3.e2.ReferenceCount == refs && pfn->u4.LockCharged &&
          pfn->u2.ShareCount == (ULONG)resident &&
          pfn->u3.e1.PageLocation == (resident ? ActiveAndValid : TransitionPage) &&
          (pfn->PteAddress != NULL) == resident);
}

VOID XpCheckBootMdl(VOID)
{
    const ULONG pages = 2;
    const ULONG bytes = pages * PAGE_SIZE;
    const PFN_NUMBER charge = MmSystemLockPagesCount;
    const PFN_NUMBER unmaterialized = MiArm64BootDemandZeroPages;
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG ptes = MiCountFreeSystemPtes(SystemPteSpace);
    PUCHAR source = MiAllocatePoolPages(NonPagedPool, bytes, 0);
    CHECK(source && !BYTE_OFFSET(source));
    source[PAGE_SIZE - 1] = 0x41;
    source[PAGE_SIZE] = 0x42;

    TWO_PAGE_MDL hold, range;
    CHECK(MmSizeOfMdl(source + PAGE_SIZE - 1, 2) == sizeof(range));
    MmInitializeMdl(&hold.Mdl, source + PAGE_SIZE, 1);
    MmProbeAndLockPages(&hold.Mdl, KernelMode, IoReadAccess);
    PFN_NUMBER second = hold.Frames[0];
    PFN_NUMBER first = MmArm64QueryPhysicalAddress((ULONG_PTR)source) >> PAGE_SHIFT;
    CHECK(first != second && MmSystemLockPagesCount == charge + 1);

    /* Force the second page to reach XP's lock limit after capturing the first. */
    ULONG reference_limit = MmReferenceCountCheck;
    MmReferenceCountCheck = 2;
    MmInitializeMdl(&range.Mdl, source + PAGE_SIZE - 1, 2);
    CHECK(try_lock_test_mdl(&range.Mdl) == STATUS_WORKING_SET_QUOTA);
    MmReferenceCountCheck = reference_limit;
    CHECK(!range.Mdl.MdlFlags && range.Frames[0] == MM_EMPTY_LIST && range.Frames[1] == MM_EMPTY_LIST &&
          MI_PFN_ELEMENT(first)->u3.e2.ReferenceCount == 1 && !MI_PFN_ELEMENT(first)->u4.LockCharged &&
          MmSystemLockPagesCount == charge + 1);
    check_pinned_page(second, 2, TRUE);

    MmProbeAndLockPages(&range.Mdl, KernelMode, IoModifyAccess);
    CHECK(range.Frames[0] == first && range.Frames[1] == second && range.Mdl.Process == NULL &&
          (range.Mdl.MdlFlags & (MDL_PAGES_LOCKED | MDL_WRITE_OPERATION)) ==
              (MDL_PAGES_LOCKED | MDL_WRITE_OPERATION) && MmSystemLockPagesCount == charge + pages);
    check_pinned_page(first, 2, TRUE);
    check_pinned_page(second, 3, TRUE);

    PMMPTE all = MiReserveSystemPtes(ptes, SystemPteSpace);
    CHECK(all && !MiCountFreeSystemPtes(SystemPteSpace) && !map_test_mdl(&range.Mdl) &&
          !range.Mdl.MappedSystemVa && !(range.Mdl.MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) &&
          MmSystemLockPagesCount == charge + pages);
    MiReleaseSystemPtes(all, ptes, SystemPteSpace);
    PUCHAR alias = map_test_mdl(&range.Mdl);
    PUCHAR second_alias = map_test_mdl(&hold.Mdl);
    CHECK(alias && second_alias && alias != source + PAGE_SIZE - 1 &&
          BYTE_OFFSET(alias) == PAGE_SIZE - 1 && alias[0] == 0x41 && alias[1] == 0x42 &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)alias) == ((first << PAGE_SHIFT) | (PAGE_SIZE - 1)) &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)alias + 1) == (second << PAGE_SHIFT) &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)second_alias) == (second << PAGE_SHIFT) &&
          MiCountFreeSystemPtes(SystemPteSpace) == ptes - 3);
    for (ULONG i = 0; i < pages; ++i) {
        ULONG_PTR va = (ULONG_PTR)PAGE_ALIGN(alias) + i * PAGE_SIZE;
        PMMPTE pte = MmSystemPteBase + ((va - MM_BOOT_IO_BASE) >> PAGE_SHIFT);
        ULONG64 par = MmArm64QueryTranslation(va, TRUE);
        CHECK(!(par & 1) && (par >> 56) == 0xff && ((par >> 7) & 3) == 3 &&
              (pte->u.Long & (3ULL << 53)) == (3ULL << 53));
    }
    alias[1] = 0x5a;
    CHECK(*second_alias == 0x5a && source[PAGE_SIZE] == 0x5a);
    MmUnmapLockedPages(second_alias, &hold.Mdl);
    XpCheckStackFault((ULONG_PTR)second_alias);
    CHECK(hold.Mdl.MdlFlags == MDL_PAGES_LOCKED && !hold.Mdl.MappedSystemVa);
    CHECK(MiFreePoolPages(source) == pages);
    XpCheckStackFault((ULONG_PTR)source);
    check_pinned_page(first, 1, FALSE);
    check_pinned_page(second, 2, FALSE);

    PUCHAR replacement = MiAllocatePoolPages(NonPagedPool, bytes, 0);
    CHECK(replacement == source && !replacement[PAGE_SIZE] && alias[1] == 0x5a &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)replacement) != (first << PAGE_SHIFT) &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)replacement + PAGE_SIZE) != (second << PAGE_SHIFT));
    MmUnlockPages(&hold.Mdl);
    check_pinned_page(second, 1, FALSE);
    MmUnlockPages(&range.Mdl);
    XpCheckStackFault((ULONG_PTR)alias);
    CHECK(MI_PFN_ELEMENT(first)->u3.e1.PageLocation == FreePageList &&
          MI_PFN_ELEMENT(second)->u3.e1.PageLocation == FreePageList &&
          MmSystemLockPagesCount == charge && MiFreePoolPages(replacement) == pages);

    source = MiAllocatePoolPages(PagedPool, bytes, 0);
    CHECK(source && MmArm64QueryPhysicalAddress((ULONG_PTR)source) == ~0ULL &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)source + PAGE_SIZE) == ~0ULL &&
          MiArm64BootDemandZeroPages == unmaterialized + pages);
    MmInitializeMdl(&range.Mdl, source, bytes);
    MmProbeAndLockPages(&range.Mdl, KernelMode, IoWriteAccess);
    CHECK(MiArm64BootDemandZeroPages == unmaterialized && MmSystemLockPagesCount == charge + pages);
    alias = map_test_mdl(&range.Mdl);
    CHECK(alias && !alias[PAGE_SIZE]);
    alias[PAGE_SIZE] = 0x13;
    CHECK(source[PAGE_SIZE] == 0x13);
    CHECK(MiFreePoolPages(source) == pages && alias[PAGE_SIZE] == 0x13);
    MmUnlockPages(&range.Mdl);
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)alias) == ~0ULL);

    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(MmSystemLockPagesCount == charge && MiArm64BootDemandZeroPages == unmaterialized &&
          after.Free == before.Free && after.Allocated == before.Allocated &&
          MiCountFreeSystemPtes(SystemPteSpace) == ptes);
    XpTestReport("MDL evidence locked_pages="); XpTestHex(pages);
    XpTestReport(" free_ptes="); XpTestHex(ptes);
    XpTestReport(" free_pfns="); XpTestHex(after.Free);
    XpTestReport("\n");
    XpTestReport("PASS: ARM64 kernel MDL pinning, cached NX aliases, rollback and last-lock reclamation\n");
}

static PFN_NUMBER check_resident(ULONG_PTR address, PMMPTE pte, ULONG64 demand_zero)
{
    ULONG64 physical = MmArm64QueryPhysicalAddress(address);
    CHECK(physical != ~0ULL && physical != address && !(physical & (PAGE_SIZE - 1)));
    PFN_NUMBER frame = physical >> PAGE_SHIFT;
    const MMPFN *pfn = MI_PFN_ELEMENT(frame);
    CHECK(pfn->u3.e1.PageLocation == ActiveAndValid && pfn->u3.e2.ReferenceCount == 1 &&
          pfn->u2.ShareCount == 1 && !pfn->u4.KernelStack && pfn->PteAddress == pte &&
          pfn->u4.PteFrame == ((ULONG_PTR)pte >> PAGE_SHIFT) &&
          pfn->OriginalPte.u.Long == demand_zero && (pte->u.Long & (3ULL << 53)) == (3ULL << 53));
    return frame;
}

VOID XpCheckPagedPool(VOID)
{
    MM_PAGED_POOL_INFO *info = &MmPagedPoolInfo;
    ULONG permanent = RtlNumberOfSetBits(info->PagedPoolAllocationMap);
    CHECK(permanent >= 2 && permanent < MM_BOOT_PAGED_POOL_PAGES && info->AllocatedPagedPool == permanent &&
          info->PagedPoolCommit == permanent && MmPagedPoolCommit == permanent &&
          !MiArm64BootDemandZeroPages && RtlNumberOfSetBits(info->EndOfPagedPoolBitmap) == permanent);
    for (ULONG i = 0; i < permanent; ++i) {
        CHECK(RtlCheckBit(info->PagedPoolAllocationMap, i) &&
              RtlCheckBit(info->EndOfPagedPoolBitmap, i) &&
              MmArm64QueryPhysicalAddress(MM_BOOT_PAGED_POOL_BASE + i * PAGE_SIZE) != ~0ULL);
    }
    const ULONG_PTR base = MM_BOOT_PAGED_POOL_BASE + permanent * PAGE_SIZE;
    const ULONG available = (ULONG)MM_BOOT_PAGED_POOL_PAGES - permanent;
    const SIZE_T capacity = available * PAGE_SIZE;
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    CHECK(info->LastPteForPagedPool - info->FirstPteForPagedPool + 1 == MM_BOOT_PAGED_POOL_PAGES);
    CHECK(!MiAllocatePoolPages(PagedPool, 0, 0) && !MiAllocatePoolPages(PagedPool, MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE + 1, 0));
    CHECK(!MiAllocatePoolPages(PagedPool, PAGE_SIZE, 1));
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    PVOID first = MiAllocatePoolPages(PagedPool, 2 * PAGE_SIZE + 1, 0);
    PVOID neighbor = MiAllocatePoolPages(PagedPool, PAGE_SIZE, 0);
    CHECK((ULONG_PTR)first == base && (ULONG_PTR)neighbor == base + 3 * PAGE_SIZE);
    CHECK(info->AllocatedPagedPool == permanent + 4 && info->PagedPoolCommit == permanent + 4 &&
          MmPagedPoolCommit == permanent + 4 && MiArm64BootDemandZeroPages == 4);
    CHECK(RtlNumberOfSetBits(info->EndOfPagedPoolBitmap) == permanent + 2 &&
          RtlCheckBit(info->EndOfPagedPoolBitmap, permanent + 2) &&
          RtlCheckBit(info->EndOfPagedPoolBitmap, permanent + 3));
    CHECK(!MiFreePoolPages((PVOID)(base + 1)) && !MiFreePoolPages((PVOID)(base + PAGE_SIZE)));

    PMMPTE ptes = info->FirstPteForPagedPool + permanent;
    ULONG64 demand_zero = ptes[0].u.Long;
    for (ULONG i = 0; i < 4; ++i) {
        CHECK(MmArm64QueryPhysicalAddress(base + i * PAGE_SIZE) == ~0ULL);
        CHECK(demand_zero && !(demand_zero & 1) && ptes[i].u.Long == demand_zero);
    }
    MM_ARM64_BOOT_PFN_COUNTS committed = MmArm64BootFrameCounts();
    CHECK(committed.Free == before.Free && committed.Allocated == before.Allocated);

    KIRQL previous;
    KeRaiseIrql(DISPATCH_LEVEL, &previous);
    CHECK(!MiAllocatePoolPages(PagedPool, PAGE_SIZE, 0) && !MiFreePoolPages(first));
    XpCheckStackFault(base); /* A valid reservation may only fault at APC_LEVEL or below. */
    KeLowerIrql(previous);
    CHECK(ptes[0].u.Long == demand_zero && MiArm64BootDemandZeroPages == 4);

    KeRaiseIrql(APC_LEVEL, &previous);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    volatile KPCR *pcr = KeGetPcr();
    ULONG64 traps = pcr->TrapCount;
    volatile ULONG64 *read_page = first;
    volatile ULONG64 *write_page = (PVOID)(base + PAGE_SIZE);
    CHECK(read_page[PAGE_SIZE / sizeof(*read_page) - 1] == 0);
    CHECK(pcr->TrapCount == traps + 1 && pcr->LastTrapEsrValid &&
          ((pcr->LastTrapEsr >> 26) & 63) == 0x25 && (pcr->LastTrapEsr & 127) == 7);
    ULONG64 read_esr = pcr->LastTrapEsr;
    write_page[13] = 0xcafe12348765ULL;
    CHECK(pcr->TrapCount == traps + 2 && pcr->LastTrapEsrValid &&
          ((pcr->LastTrapEsr >> 26) & 63) == 0x25 && (pcr->LastTrapEsr & 127) == 71);
    ULONG64 write_esr = pcr->LastTrapEsr;
    CHECK(write_page[13] == 0xcafe12348765ULL);
    for (ULONG i = 0; i < PAGE_SIZE / sizeof(*read_page); ++i) {
        CHECK(read_page[i] == 0);
        if (i != 13) CHECK(write_page[i] == 0);
    }
    read_page[2] = 0xabcdef012345ULL;
    PFN_NUMBER read_frame = check_resident(base, ptes, demand_zero);
    CHECK(check_resident(base + PAGE_SIZE, ptes + 1, demand_zero) != read_frame);
    CHECK(pcr->TrapCount == traps + 2 && MiArm64BootDemandZeroPages == 2);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    KeLowerIrql(previous);

    MM_ARM64_BOOT_PFN_COUNTS resident = MmArm64BootFrameCounts();
    CHECK(resident.Free + 2 == before.Free && resident.Allocated == before.Allocated + 2);
    CHECK(MmArm64QueryPhysicalAddress(base + 2 * PAGE_SIZE) == ~0ULL);
    CHECK(MiFreePoolPages(first) == 3 && !MiFreePoolPages(first));
    CHECK(ptes[0].u.Long == 0 && ptes[1].u.Long == 0 && ptes[2].u.Long == 0);
    XpCheckStackFault(base);
    XpCheckStackFault(base + PAGE_SIZE);
    CHECK(MiArm64BootDemandZeroPages == 1 && MmPagedPoolCommit == permanent + 1);
    CHECK(MiFreePoolPages(neighbor) == 1);

    PVOID all = MiAllocatePoolPages(PagedPool, capacity, 0);
    CHECK(all == first && MiArm64BootDemandZeroPages == available);
    CHECK(!MiAllocatePoolPages(PagedPool, 1, 0));
    CHECK(MiFreePoolPages(all) == available);

    PVOID again = MiAllocatePoolPages(PagedPool, PAGE_SIZE, 0);
    CHECK(again == first && MmArm64QueryPhysicalAddress(base) == ~0ULL);
    CHECK(read_page[2] == 0 && MiArm64BootDemandZeroPages == 0);
    CHECK(MiFreePoolPages(again) == 1 && MmPagedPoolCommit == permanent &&
          info->PagedPoolCommit == permanent && info->AllocatedPagedPool == permanent &&
          RtlNumberOfSetBits(info->PagedPoolAllocationMap) == permanent &&
          RtlNumberOfSetBits(info->EndOfPagedPoolBitmap) == permanent);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(after.Free == before.Free && after.Allocated == before.Allocated);
    XpTestReport("Paged pool evidence read_esr="); XpTestHex(read_esr);
    XpTestReport(" write_esr="); XpTestHex(write_esr);
    XpTestReport(" aperture_pages="); XpTestHex(MM_BOOT_PAGED_POOL_PAGES);
    XpTestReport(" resident_boot_pages="); XpTestHex(permanent);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: NT paged pool bitmap commitment, real ARM64 demand-zero and PFN reclamation\n");
}
