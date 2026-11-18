#include "../base/ntos/hal/arm64/gicv2.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/ke/arm64/clock.h"
/* Actual firmware handoff control; not a replacement NT startup/scheduler.
 * All firmware services used here delegate to the real EDK2 implementation. */
#include "../base/boot/arm64/boot.h"
#include "uefi_test.h"
#include <ntimage.h>
#include "ki.h"
#include "../base/ntos/mm/arm64/bootframes_private.h"
#include "../base/ntos/mm/arm64/bootio_private.h"
static XP_ARM64_BOOT_BLOCK map_test;
extern IMAGE_DOS_HEADER __ImageBase;
extern const UCHAR XpPostExitBrk[], XpGuardRead[], XpNonExecutableReturn[];
VOID XpCheckStackFault(ULONG_PTR Address);
VOID XpCheckTranslationFault(ULONG_PTR Address, ULONG Level);
static volatile ULONG_PTR execute_probe;
static ULONG64 execute_syndrome;
VOID XpCheckUserSpaces(PXP_ARM64_BOOT_BLOCK Boot);
VOID XpCheckBootStacks(PMM_ARM64_BOOT_TABLES Tables);
__declspec(noreturn) VOID XpRunOriginalThreads(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckThreadContextBoundary(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckKernelContexts(PXP_ARM64_BOOT_BLOCK Block);
BOOLEAN XpHandleBootStackFault(PKTRAP_FRAME Frame);
BOOLEAN XpHandleUnwindCheckpoint(PKTRAP_FRAME Frame);
VOID XpCheckUnwind(PXP_ARM64_BOOT_BLOCK Block);
BOOLEAN XpHandleScopeFault(PKTRAP_FRAME Frame);
VOID XpCheckScopeExceptions(PXP_ARM64_BOOT_BLOCK Block);
BOOLEAN XpHandleHardwareInterrupt(PKTRAP_FRAME Frame, ULONG Kind);
VOID XpCheckHardwareInterrupts(PXP_ARM64_BOOT_BLOCK Block);
VOID XpCheckClockContracts(VOID);

BOOLEAN KiArm64BootTrap(PKTRAP_FRAME frame, ULONG kind)
{
    if (kind==0 && ((frame->Esr>>26)&63)==0x3c && (frame->Esr&65535)==0x7ef) {
        XpTestReport("Kernel bugcheck:");
        for (ULONG i=0;i<5;++i) { XpTestReport(" ");XpTestHex(KiArm64BugCheckData[i]); }
        XpTestReport("\n");XpTestFailure(__LINE__);
    }
    if (XpHandleHardwareInterrupt(frame, kind)) return TRUE;
    CHECK(kind == 0 && frame->PreviousMode == KernelMode);
    if (execute_probe && frame->Pc==execute_probe) {
        CHECK(frame->Far==execute_probe && frame->X[30]==(ULONG_PTR)XpNonExecutableReturn);
        CHECK(((frame->Esr>>26)&63)==0x21 && (frame->Esr&63)==0x0f);
        execute_syndrome=frame->Esr;
        frame->Pc=(ULONG_PTR)XpNonExecutableReturn;
        return TRUE;
    }
    if (XpHandleBootStackFault(frame)) return TRUE;
    if (XpHandleUnwindCheckpoint(frame)) return TRUE;
    if (XpHandleScopeFault(frame)) return TRUE;
    PXP_ARM64_BOOT_BLOCK block=CONTAINING_RECORD(KeGetPcr(),XP_ARM64_BOOT_BLOCK,Pcr);
    if (frame->Pc == (ULONG_PTR)XpGuardRead) {
        CHECK(((frame->Esr >> 26) & 63) == 0x25);
        CHECK((frame->Esr & 63) == 7 && !(frame->Esr & (1u << 6)));
        CHECK(frame->Far == block->StackAllocationBase);
        frame->Pc += 4;
        return TRUE;
    }
    CHECK(frame->Pc == (ULONG_PTR)XpPostExitBrk);
    CHECK(((frame->Esr >> 26) & 63) == 0x3c && (frame->Esr & 0xffff) == 0x71);
    CHECK(frame->Sp >= block->StackBase && frame->Sp < block->Nt.KernelStack);
    CHECK(frame->PreviousIrql == HIGH_LEVEL);
    frame->Pc += 4;
    return TRUE;
}
static void check_rejected_map(void)
{
    CHECK(BlArm64ConvertMemoryMap(&map_test) == STATUS_INVALID_PARAMETER);
    CHECK(IsListEmpty(&map_test.Nt.MemoryDescriptorListHead));
    CHECK(map_test.DescriptorCount == 0 && map_test.FreePages == 0 && map_test.ReservedPages == 0);
}
static void check_map_rules(void)
{
    /* A 48-byte firmware stride must be honored, not sizeof(descriptor). */
    map_test.DescriptorVersion = 1;
    map_test.DescriptorSize = 48;
    map_test.MapSize = 48 * 4;
    XP_EFI_MEMORY_DESCRIPTOR *m[4];
    for (ULONG i = 0; i < 4; ++i) m[i] = (PVOID)(map_test.Map + 48 * i);
    for (ULONG i = 0; i < 4; ++i) {
        m[i]->Type = 7; m[i]->Attribute = 8; m[i]->NumberOfPages = 4;
    }
    m[0]->PhysicalStart = 0x30000; m[0]->Type = 4;
    m[1]->PhysicalStart = 0x10000;
    m[2]->PhysicalStart = 0x40000; m[2]->Attribute |= XP_EFI_MEMORY_RUNTIME;
    m[3]->PhysicalStart = 0x20000; m[3]->Type = 2;
    CHECK(NT_SUCCESS(BlArm64ConvertMemoryMap(&map_test)));
    CHECK(map_test.FreePages == 4 && map_test.ReservedPages == 12 && map_test.DescriptorCount == 4);
    CHECK(map_test.Descriptors[0].BasePage == 0x10 && map_test.Descriptors[0].MemoryType == LoaderFree);
    CHECK(map_test.Descriptors[1].MemoryType == LoaderMemoryData);
    CHECK(map_test.Descriptors[2].MemoryType == LoaderFirmwarePermanent);
    CHECK(map_test.Descriptors[3].MemoryType == LoaderFirmwarePermanent);
    m[0]->PhysicalStart = 0x11000; check_rejected_map();
    m[0]->PhysicalStart = 0x30001; check_rejected_map();
    m[0]->PhysicalStart = 0x100000000000ULL; check_rejected_map();
    m[0]->PhysicalStart = 0x30000; m[0]->NumberOfPages = 0; check_rejected_map();
    m[0]->NumberOfPages = 1ULL << 32; check_rejected_map();
    m[0]->PhysicalStart = 0xffffffff000ULL; m[0]->NumberOfPages = 2; check_rejected_map();
    m[0]->PhysicalStart = 0x30000; m[0]->NumberOfPages = 4;
    map_test.DescriptorSize = 32; check_rejected_map();
    map_test.DescriptorSize = 48; map_test.DescriptorVersion = 2; check_rejected_map();
    XpTestReport("PASS: loader map stride, sorting, reservations and malformed-map rejection\n");
}
__declspec(noinline) static ULONG64 check_execute_denied(ULONG_PTR address)
{
    execute_syndrome=0;
    execute_probe=address;
    __asm__ volatile("blr %0\n.globl XpNonExecutableReturn\nXpNonExecutableReturn:"
                     :: "r"(address) : "x30", "memory");
    execute_probe=0;
    CHECK(execute_syndrome!=0);
    return execute_syndrome;
}

static VOID check_high_shared_page(const XP_ARM64_BOOT_BLOCK *block)
{
    ULONG_PTR va=(ULONG_PTR)SharedUserData;
    CHECK(va==KI_USER_SHARED_DATA);
    KI_ARM64_CLOCK_STATE clock;
    KiArm64GetBootClockState(&clock);
    CHECK(clock.Initialized && clock.Publications==1);
    CHECK(MmArm64QueryPhysicalAddress(va)==clock.SharedPagePhysical);
    CHECK(MmArm64QueryPhysicalAddress(va-PAGE_SIZE)==~0ULL);
    CHECK(MmArm64QueryPhysicalAddress(va+PAGE_SIZE)==~0ULL);
    XpCheckStackFault(clock.SharedPagePhysical); /* The old identity view really faults. */

    /* TickCountLow is still zero until the runtime clock starts. The
     * multiplier and initial time fields are the only published bytes. */
    CHECK(!SharedUserData->TickCountLow && SharedUserData->TickCountMultiplier == 0x01000000);
    ULONG first=FIELD_OFFSET(KUSER_SHARED_DATA,TickCountMultiplier);
    ULONG end=FIELD_OFFSET(KUSER_SHARED_DATA,SystemTime)+sizeof(KSYSTEM_TIME);
    const volatile UCHAR *bytes=(PVOID)va;
    for (ULONG i=0;i<PAGE_SIZE;++i)
        if (i<first || i>=end) CHECK(bytes[i]==0);

    ULONG64 saved,el0_par;
    __asm__ volatile("mrs %0, par_el1" : "=r"(saved));
    __asm__ volatile("at s1e0r, %1\nisb\nmrs %0, par_el1"
                     : "=r"(el0_par) : "r"(va) : "memory");
    __asm__ volatile("msr par_el1, %0" :: "r"(saved));
    CHECK((el0_par&1) && ((el0_par>>1)&63)==0x0f);
    ULONG64 syndrome=check_execute_denied(va);
    XpTestReport("High VA evidence ttbr1=");XpTestHex(block->Tables.KernelRoot);
    XpTestReport(" shared=");XpTestHex(va);
    XpTestReport(" backing=");XpTestHex(clock.SharedPagePhysical);
    XpTestReport(" el0_par=");XpTestHex(el0_par);
    XpTestReport(" nx_esr=");XpTestHex(syndrome);XpTestReport("\n");
    XpTestReport("PASS: owned NT high shared page via TTBR1, retired identity alias and execute protection\n");
}

static VOID check_system_ptes(VOID)
{
    ULONG free = MmTotalSystemPtes - 2; /* Two real, pinned GIC mappings. */
    CHECK(MmTotalSystemPtes == MM_BOOT_IO_PAGES);
    CHECK(MiCountFreeSystemPtes(SystemPteSpace) == free &&
          MmTotalFreeSystemPtes[SystemPteSpace] == free);
    CHECK(!MmSystemPteBase[0].u.List.Valid && MmSystemPteBase[0].u.Long);
    XpCheckStackFault(MM_BOOT_IO_BASE);
    CHECK(!MiReserveSystemPtes(0, SystemPteSpace));
    CHECK(!MiReserveSystemPtes(1, NonPagedPoolExpansion));
    CHECK(!MiReserveAlignedSystemPtes(1, SystemPteSpace, 3 * PAGE_SIZE));

    const ULONG alignment = 64 * 1024;
    PMMPTE first = MiReserveAlignedSystemPtes(5, SystemPteSpace, alignment);
    PMMPTE second = MiReserveAlignedSystemPtes(3, SystemPteSpace, alignment);
    PMMPTE gap = MiReserveSystemPtes(11, SystemPteSpace);
    CHECK(first == MmSystemPteBase && gap == first + 5 && second == first + 16);
    CHECK((ULONG_PTR)MmArm64SystemPteVa(second) == MM_BOOT_IO_BASE + alignment);
    CHECK(MiCountFreeSystemPtes(SystemPteSpace) == free - 19 &&
          MmTotalFreeSystemPtes[SystemPteSpace] == free - 19);
    CHECK(MmArm64TryReleaseSystemPtes(first + 1, 4, SystemPteSpace) == STATUS_INVALID_HANDLE);
    CHECK(MmArm64TryReleaseSystemPtes(first, 4, SystemPteSpace) == STATUS_INVALID_HANDLE);
    MiReleaseSystemPtes(first, 5, SystemPteSpace);
    MiReleaseSystemPtes(gap, 11, SystemPteSpace);
    MiReleaseSystemPtes(second, 3, SystemPteSpace); /* Merge both neighbors. */
    CHECK(MmArm64TryReleaseSystemPtes(second, 3, SystemPteSpace) == STATUS_INVALID_HANDLE);
    CHECK(MiCountFreeSystemPtes(SystemPteSpace) == free);

    ULONG failures = MmPteFailures[SystemPteSpace];
    PMMPTE tail = MiReserveSystemPtes(free - 1, SystemPteSpace);
    CHECK(tail == MmSystemPteBase + 1 && MmSystemPteBase[0].u.List.OneEntry);
    PMMPTE singleton = MiReserveSystemPtes(1, SystemPteSpace);
    CHECK(singleton == MmSystemPteBase && MiCountFreeSystemPtes(SystemPteSpace) == 0);
    CHECK(!MiReserveSystemPtes(1, SystemPteSpace) && MmPteFailures[SystemPteSpace] == failures + 1);
    MiReleaseSystemPtes(singleton, 1, SystemPteSpace);
    MiReleaseSystemPtes(tail, free - 1, SystemPteSpace);
    PMMPTE all = MiReserveSystemPtes(free, SystemPteSpace);
    CHECK(all == MmSystemPteBase && !MmTotalFreeSystemPtes[SystemPteSpace]);
    for (ULONG i = 0; i < free; ++i) CHECK(!all[i].u.Long);
    MiReleaseSystemPtes(all, free, SystemPteSpace);
    CHECK(MiCountFreeSystemPtes(SystemPteSpace) == free && MmTotalFreeSystemPtes[SystemPteSpace] == free);
    XpCheckStackFault(MM_BOOT_IO_BASE);

    XpTestReport("System PTE evidence capacity="); XpTestHex(MmTotalSystemPtes);
    XpTestReport(" free="); XpTestHex(free);
    XpTestReport(" alignment="); XpTestHex(alignment);
    XpTestReport(" failures="); XpTestHex(MmPteFailures[SystemPteSpace] - failures); XpTestReport("\n");
    XpTestReport("PASS: original system PTE free clusters, aligned splits, merges, exhaustion and reuse\n");
}

static VOID check_boot_io(const XP_ARM64_BOOT_BLOCK *block)
{
    HAL_ARM64_IRQ_STATS stats;
    HalArm64GetIrqStats(&stats);
    ULONG_PTR end = MM_BOOT_IO_BASE + MM_BOOT_IO_PAGES * PAGE_SIZE;
    CHECK(stats.Initialized && stats.GicDistributorVa == end - PAGE_SIZE &&
          stats.GicCpuInterfaceVa == end - 2 * PAGE_SIZE);
    CHECK(MmArm64QueryPhysicalAddress(stats.GicDistributorVa) == block->Platform.GicDistributor &&
          MmArm64QueryPhysicalAddress(stats.GicCpuInterfaceVa) == block->Platform.GicCpuInterface);
    CHECK(MmTrackPtes == 1 && MiPteHeader.Count == 2);
    CHECK(MiPlatformCacheAttributes[MmNonCached] == MiCached &&
          MiPlatformCacheAttributes[MmMaximumCacheType + MmNonCached] == MiNonCached);
    HAL_PLATFORM_INFORMATION info;
    ULONG required = 0;
    CHECK(HalArm64QueryBootPlatformInformation(HalPlatformInformation, 0, NULL, &required) ==
          STATUS_BUFFER_TOO_SMALL && required == sizeof(info));
    CHECK(HalArm64QueryBootPlatformInformation(HalPlatformInformation, sizeof(info), &info,
          &required) == STATUS_SUCCESS && info.PlatformFlags == HAL_PLATFORM_DISABLE_UC_MAIN_MEMORY);

    KeLowerIrql(DISPATCH_LEVEL);
    check_system_ptes();
    PHYSICAL_ADDRESS physical;
    physical.QuadPart = block->Platform.GicDistributor + 0xfe8;
    CHECK(!MmMapIoSpace(physical, sizeof(ULONG), MmCached));
    CHECK(!MmMapIoSpace(physical, sizeof(ULONG), MmWriteCombined));
    CHECK(!MmMapIoSpace(physical, 2 * PAGE_SIZE, MmNonCached));
    CHECK(!MmMapIoSpace(physical, 0, MmNonCached));
    CHECK(!MmMapIoSpace(physical, (SIZE_T)-1, MmNonCached));
    PHYSICAL_ADDRESS ram;
    ram.QuadPart = block->SharedPagePfn << PAGE_SHIFT;
    CHECK(!MmMapIoSpace(ram, PAGE_SIZE, MmNonCached));
    CHECK(MmArm64TryUnmapIoSpace((PVOID)stats.GicDistributorVa, PAGE_SIZE) == STATUS_INVALID_HANDLE);

    PVOID alias = MmMapIoSpace(physical, sizeof(ULONG), MmNonCached);
    CHECK((ULONG_PTR)alias == end - 3 * PAGE_SIZE + 0xfe8);
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)alias) == (ULONG64)physical.QuadPart);
    ULONG64 par = MmArm64QueryTranslation((ULONG_PTR)alias, TRUE);
    CHECK(!(par & 1) && ((par >> 56) == 0 || (par >> 56) == 4));
    CHECK((par & 0xff00000000000180ULL) ==
          (MmArm64QueryTranslation(physical.QuadPart, TRUE) & 0xff00000000000180ULL));
    CHECK(MiPteHeader.Count == 3);
    CHECK(MmTotalFreeSystemPtes[SystemPteSpace] == MM_BOOT_IO_PAGES - 3);
    PPTE_TRACKER tracker = CONTAINING_RECORD(MiPteHeader.ListHead.Flink, PTE_TRACKER, ListEntry);
    CHECK(tracker->SystemVa == alias && tracker->PteAddress == MmArm64IoPte(alias) &&
          tracker->Offset == 0xfe8 && tracker->Length == sizeof(ULONG) &&
          tracker->Count == 1 && tracker->Page == ((ULONG64)physical.QuadPart >> PAGE_SHIFT) &&
          tracker->CallingAddress && !tracker->Mdl);
    ULONG pid = *(volatile ULONG *)alias;
    CHECK(((pid >> 4) & 15) == 2);
    CHECK(pid == *(volatile ULONG *)(stats.GicDistributorVa + 0xfe8));
    ULONG64 nx = check_execute_denied((ULONG_PTR)alias);
    CHECK(MmArm64TryUnmapIoSpace(alias, PAGE_SIZE) == STATUS_INVALID_HANDLE);
    MmUnmapIoSpace(alias, sizeof(ULONG));
    CHECK(MiPteHeader.Count == 2 && MmArm64TryUnmapIoSpace(alias, sizeof(ULONG)) == STATUS_INVALID_HANDLE);
    XpCheckStackFault((ULONG_PTR)alias);
    CHECK(MmMapIoSpace(physical, sizeof(ULONG), MmNonCached) == alias);
    MmUnmapIoSpace(alias, sizeof(ULONG));
    CHECK(MiPteHeader.Count == 2 && HalArm64ReadPriorityMask() != 0);
    CHECK(MmTotalFreeSystemPtes[SystemPteSpace] == MM_BOOT_IO_PAGES - 2);
    KIRQL old;
    KeRaiseIrql(HIGH_LEVEL, &old);
    CHECK(old == DISPATCH_LEVEL);

    XpTestReport("Boot I/O evidence gicd="); XpTestHex(stats.GicDistributorVa);
    XpTestReport(" gicc="); XpTestHex(stats.GicCpuInterfaceVa);
    XpTestReport(" alias="); XpTestHex((ULONG_PTR)alias);
    XpTestReport(" tracked="); XpTestHex(MiPteHeader.Count);
    XpTestReport(" attr="); XpTestHex(par >> 56);
    XpTestReport(" nx_esr="); XpTestHex(nx); XpTestReport("\n");
    XpTestReport("PASS: original I/O tracker and cache override, TTBR1 GIC mappings and MMIO release\n");
}

static VOID check_nonpaged_pool_pages(const MM_ARM64_BOOT_TABLES *tables)
{
    const ULONG_PTR base = MM_BOOT_POOL_BASE;
    const ULONG pages = MM_BOOT_POOL_PAGES;
    const SIZE_T capacity = pages * PAGE_SIZE;
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    CHECK(KeGetCurrentIrql() == HIGH_LEVEL);
    CHECK(!MiAllocatePoolPages(NonPagedPool, PAGE_SIZE, 0));
    XpCheckStackFault(base);
    KeLowerIrql(DISPATCH_LEVEL);
    CHECK(!MiAllocatePoolPages(PagedPool, PAGE_SIZE, 0));
    CHECK(!MiAllocatePoolPages(NonPagedPool, PAGE_SIZE, 1));
    CHECK(!MiAllocatePoolPages(NonPagedPool, 0, 0));
    CHECK(!MiAllocatePoolPages(NonPagedPool, capacity + 1, 0));
    CHECK(!MiAllocatePoolPages(NonPagedPool, (SIZE_T)-1, 0));

    PVOID first = MiAllocatePoolPages(NonPagedPool, PAGE_SIZE + 1, 0);
    PVOID second = MiAllocatePoolPages(NonPagedPool, PAGE_SIZE, 0);
    PVOID tail = MiAllocatePoolPages(NonPagedPool, (pages - 3) * PAGE_SIZE, 0);
    CHECK((ULONG_PTR)first == base && (ULONG_PTR)second == base + 2 * PAGE_SIZE);
    CHECK((ULONG_PTR)tail == base + 3 * PAGE_SIZE);
    CHECK(!MiAllocatePoolPages(NonPagedPool, 1, 0));
    CHECK(!MiFreePoolPages((PVOID)(base + 1)));
    CHECK(!MiFreePoolPages((PVOID)(base + PAGE_SIZE)));
    CHECK(!MiFreePoolPages((PVOID)(base + capacity)));
    MM_ARM64_BOOT_PFN_COUNTS full = MmArm64BootFrameCounts();
    CHECK(full.Allocated == before.Allocated + pages && full.Free + pages == before.Free);
    for (ULONG i = 0; i < pages; ++i) {
        ULONG_PTR va = base + i * PAGE_SIZE;
        ULONG64 physical = MmArm64QueryPhysicalAddress(va);
        CHECK(physical && physical != ~0ULL && physical != va && !(physical & (PAGE_SIZE - 1)));
        const MMPFN *pfn = MI_PFN_ELEMENT(physical >> PAGE_SHIFT);
        CHECK(pfn->u3.e1.PageLocation == ActiveAndValid && pfn->u3.e2.ReferenceCount == 1 &&
              pfn->u2.ShareCount == 1 && !pfn->u4.KernelStack &&
              pfn->PteAddress == (PMMPTE)&tables->NonPagedPoolPtes[i] &&
              pfn->u4.PteFrame == ((ULONG_PTR)pfn->PteAddress >> PAGE_SHIFT));
        volatile ULONG64 *memory = (PVOID)va;
        CHECK(*memory == 0);
        *memory = 0x706f6f6c00000000ULL + i;
    }
    /* A second pass detects any unintended mapping aliases between pool pages. */
    for (ULONG i = 0; i < pages; ++i)
        CHECK(*(volatile ULONG64 *)(base + i * PAGE_SIZE) == 0x706f6f6c00000000ULL + i);
    ULONG64 nx = check_execute_denied(base);
    CHECK(MiFreePoolPages(first) == 2);
    CHECK(!MiFreePoolPages(first));
    XpCheckStackFault(base);
    XpCheckStackFault(base + PAGE_SIZE);
    CHECK(*(volatile ULONG64 *)second == 0x706f6f6c00000002ULL);
    CHECK(!MiAllocatePoolPages(NonPagedPool, 3 * PAGE_SIZE, 0));
    CHECK(MiFreePoolPages(second) == 1);
    PVOID merged = MiAllocatePoolPages(NonPagedPool, 3 * PAGE_SIZE, 0);
    CHECK((ULONG_PTR)merged == base);
    for (ULONG i = 0; i < 3; ++i)
        CHECK(*(volatile ULONG64 *)(base + i * PAGE_SIZE) == 0);
    CHECK(MiFreePoolPages(merged) == 3);
    CHECK(MiFreePoolPages(tail) == pages - 3);
    XpCheckStackFault(base + capacity - PAGE_SIZE);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(after.Free == before.Free && after.Allocated == before.Allocated);
    KIRQL previous;
    KeRaiseIrql(HIGH_LEVEL, &previous);
    CHECK(previous == DISPATCH_LEVEL);
    XpTestReport("Nonpaged pool evidence base="); XpTestHex(base);
    XpTestReport(" pages="); XpTestHex(pages);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_full="); XpTestHex(full.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free);
    XpTestReport(" nx_esr="); XpTestHex(nx); XpTestReport("\n");
    XpTestReport("PASS: TTBR1 nonpaged pool page allocation, exhaustion, NX, release faults and reuse\n");
}
static BOOLEAN firmware_free_ram(const XP_EFI_MEMORY_DESCRIPTOR *efi)
{
    return efi->Type==7 && (efi->Attribute&8) &&
        !(efi->Attribute&(XP_EFI_MEMORY_RUNTIME|0x70000));
}

static VOID check_boot_pfn_map(PXP_ARM64_BOOT_BLOCK block)
{
    ULONG64 raw_total=0, raw_free=0, best_count=0;
    PFN_NUMBER best_pfn=0;
    for (ULONG_PTR off=0;off<block->MapSize;off+=block->DescriptorSize) {
        const XP_EFI_MEMORY_DESCRIPTOR *efi=(PVOID)(block->Map+off);
        raw_total+=efi->NumberOfPages;
        if (!firmware_free_ram(efi)) continue;
        raw_free+=efi->NumberOfPages;
        PFN_NUMBER base=efi->PhysicalStart>>PAGE_SHIFT;
        if (base && (efi->NumberOfPages>best_count ||
            (efi->NumberOfPages==best_count && base<best_pfn))) {
            best_count=efi->NumberOfPages;
            best_pfn=base;
        }
    }
    ULONG taken=block->BootPfnPages;
    CHECK(taken>1 && best_count>=taken && block->SharedPagePfn==best_pfn);
    CHECK(raw_free>taken && block->FreePages==raw_free-taken);
    CHECK(block->ReservedPages==raw_total-raw_free+taken);
    ULONG expected_descriptors=(ULONG)(block->MapSize/block->DescriptorSize)+(best_count>taken);
    CHECK(block->DescriptorCount==expected_descriptors);

    ULONG visited=0, reservations=0;
    ULONG64 total=0, free_pages=0, previous_end=0;
    ULONG runs=0;
    PFN_NUMBER managed=0, previous_managed_end=0;
    CHECK(MmPhysicalMemoryBlock==(PVOID)(ULONG_PTR)((best_pfn+1)<<PAGE_SHIFT));
    ULONG64 pool_end=block->Tables.PoolBase+((ULONG64)block->Tables.PoolPages<<PAGE_SHIFT);
    ULONG64 arena_end=block->Tables.StackArena+((ULONG64)MM_BOOT_STACK_ARENA_PAGES<<PAGE_SHIFT);
    PLIST_ENTRY head=&block->Nt.MemoryDescriptorListHead;
    for (PLIST_ENTRY link=head->Flink;link!=head;link=link->Flink) {
        CHECK(visited++<block->DescriptorCount);
        PMEMORY_ALLOCATION_DESCRIPTOR nt=CONTAINING_RECORD(link,MEMORY_ALLOCATION_DESCRIPTOR,ListEntry);
        ULONG64 first=nt->BasePage, limit=first+nt->PageCount;
        CHECK(nt->PageCount && first>=previous_end);
        previous_end=limit;
        BOOLEAN matched=FALSE;
        for (ULONG_PTR off=0;off<block->MapSize;off+=block->DescriptorSize) {
            const XP_EFI_MEMORY_DESCRIPTOR *efi=(PVOID)(block->Map+off);
            ULONG64 raw_first=efi->PhysicalStart>>PAGE_SHIFT;
            ULONG64 raw_limit=raw_first+efi->NumberOfPages;
            if (first<raw_first || limit>raw_limit) continue;
            if (raw_first==best_pfn) {
                CHECK(firmware_free_ram(efi));
                if (first==best_pfn) {
                    CHECK(nt->MemoryType==LoaderMemoryData && nt->PageCount==taken);
                    ++reservations;
                } else {
                    CHECK(first==best_pfn+taken && limit==raw_limit && nt->MemoryType==LoaderFree);
                }
            } else {
                CHECK(first==raw_first && limit==raw_limit);
            }
            if (nt->MemoryType==LoaderFree) CHECK(firmware_free_ram(efi));
            matched=TRUE;
            break;
        }
        CHECK(matched);
        total+=nt->PageCount;
        if (nt->MemoryType==LoaderFree || nt->MemoryType==LoaderLoadedProgram ||
            nt->MemoryType==LoaderMemoryData) {
            if (!runs || first!=previous_managed_end) {
                if (runs) {
                    const PHYSICAL_MEMORY_RUN *prior=&MmPhysicalMemoryBlock->Run[runs-1];
                    CHECK(prior->BasePage+prior->PageCount==previous_managed_end);
                }
                CHECK(runs<MmPhysicalMemoryBlock->NumberOfRuns &&
                      MmPhysicalMemoryBlock->Run[runs].BasePage==first);
                ++runs;
            }
            const PHYSICAL_MEMORY_RUN *run=&MmPhysicalMemoryBlock->Run[runs-1];
            CHECK(limit<=run->BasePage+run->PageCount);
            previous_managed_end=limit;
            managed+=nt->PageCount;
        }
        if (nt->MemoryType==LoaderFree) {
            free_pages+=nt->PageCount;
            ULONG64 begin=first<<PAGE_SHIFT, end=limit<<PAGE_SHIFT;
            CHECK((ULONG_PTR)block<begin || (ULONG_PTR)block>=end);
            CHECK(block->Nt.KernelStack<=begin || block->StackAllocationBase>=end);
            CHECK(pool_end<=begin || block->Tables.PoolBase>=end);
            CHECK(arena_end<=begin || block->Tables.StackArena>=end);
        }
    }
    CHECK(visited==block->DescriptorCount && reservations==1);
    CHECK(total==raw_total && free_pages==block->FreePages && free_pages!=0);
    CHECK(runs>0 && runs==MmPhysicalMemoryBlock->NumberOfRuns);
    const PHYSICAL_MEMORY_RUN *last=&MmPhysicalMemoryBlock->Run[runs-1];
    CHECK(last->BasePage+last->PageCount==previous_managed_end);
    CHECK(managed==MmPhysicalMemoryBlock->NumberOfPages && managed==MmNumberOfPhysicalPages);
    MM_ARM64_BOOT_PFN_COUNTS counts=MmArm64BootFrameCounts();
    CHECK(counts.Present==managed && counts.Free+counts.Inaccessible==free_pages &&
          !counts.Allocated);
    CHECK(MmArm64ReturnBootFrame(block,best_pfn)==STATUS_INVALID_HANDLE);
    PFN_NUMBER rejected=~0ULL;
    CHECK(MmArm64ReserveBootPages(block,1,&rejected)==STATUS_INVALID_DEVICE_STATE && rejected==~0ULL);

    XpTestReport("Boot PFN evidence shared=");XpTestHex(best_pfn);
    XpTestReport(" source_count=");XpTestHex(best_count);
    XpTestReport(" reserved=");XpTestHex(taken);
    XpTestReport(" original_free=");XpTestHex(raw_free);
    XpTestReport(" remaining_free=");XpTestHex(free_pages);
    XpTestReport(" managed=");XpTestHex(managed);
    XpTestReport(" runs=");XpTestHex(runs);
    XpTestReport(" allocatable=");XpTestHex(counts.Free);
    XpTestReport(" inaccessible=");XpTestHex(counts.Inaccessible);XpTestReport("\n");
    XpTestReport("PASS: original XP physical memory runs and live PFN database match the final loader map\n");
}

static VOID check_mmpfn_database(PXP_ARM64_BOOT_BLOCK block)
{
    ULONG_PTR base = (ULONG_PTR)MmPfnDatabase;
    CHECK(base == MM_BOOT_PFN_DATABASE_BASE && MmLowestPhysicalPage > 0);
    const LIST_ENTRY *head = &block->Nt.MemoryDescriptorListHead;
    const LIST_ENTRY *cursor = head->Flink;
    PFN_NUMBER first, limit = 0, mapped = 0;
    PFN_NUMBER backing = block->SharedPagePfn + block->BootPfnPages - block->BootPfnMetadataPages;
    ULONG_PTR first_backing = backing << PAGE_SHIFT;
    while (MmArm64NextBootPfnPageRange(head, &cursor, &first, &limit)) {
        for (PFN_NUMBER page = first; page < limit; ++page, ++backing)
            CHECK(MmArm64QueryPhysicalAddress(base + (page << PAGE_SHIFT)) ==
                  (backing << PAGE_SHIFT));
        mapped += limit - first;
    }
    CHECK(mapped == block->BootPfnMetadataPages &&
          backing == block->SharedPagePfn + block->BootPfnPages);
    XpCheckTranslationFault(base, 2); /* Entire absent-prefix L3 table. */
    XpCheckStackFault(first_backing); /* Retired low alias. */
    ULONG64 nx = check_execute_denied((ULONG_PTR)MI_PFN_ELEMENT(MmLowestPhysicalPage));
    const MMPFN *shared = MI_PFN_ELEMENT(block->SharedPagePfn);
    CHECK(shared->u3.e1.PageLocation == ActiveAndValid && shared->u3.e2.ReferenceCount == 1);
    PFN_NUMBER visited = 0, previous = MM_EMPTY_LIST;
    for (PFN_NUMBER page = MmFreePageListHead.Flink; page != MM_EMPTY_LIST;) {
        CHECK(++visited <= MmAvailablePages);
        const MMPFN *pfn = MI_PFN_ELEMENT(page);
        CHECK(pfn->u2.Blink == previous && pfn->u3.e1.PageLocation == FreePageList &&
              !pfn->u3.e2.ReferenceCount && !pfn->PteAddress);
        if (previous != MM_EMPTY_LIST) CHECK(previous < page);
        previous = page;
        page = pfn->u1.Flink;
    }
    CHECK(previous == MmFreePageListHead.Blink &&
          visited == MmAvailablePages && visited == MmFreePageListHead.Total);
    XpTestReport("MMPFN evidence base="); XpTestHex(base);
    XpTestReport(" mapped="); XpTestHex(mapped);
    XpTestReport(" first_pfn="); XpTestHex(MmLowestPhysicalPage);
    XpTestReport(" last_pfn="); XpTestHex(MmHighestPhysicalPage);
    XpTestReport(" free_list="); XpTestHex(visited);
    XpTestReport(" nx_esr="); XpTestHex(nx); XpTestReport("\n");
    XpTestReport("PASS: sparse high MMPFN database, original primary free list and occupied backing\n");
}
static VOID check_original_system_init(VOID)
{
    CHECK(IsListEmpty(&KeBugCheckCallbackListHead) &&
          IsListEmpty(&KeBugCheckReasonCallbackListHead) && !KeBugCheckCallbackLock);
    CHECK(IsListEmpty(&KiProfileListHead) && IsListEmpty(&KiProfileSourceListHead) && !KiProfileLock);
    CHECK(KiTimerExpireDpc.DeferredRoutine==KiTimerExpiration &&
          !KiTimerExpireDpc.DeferredContext && !KiTimerExpireDpc.Lock);
    CHECK(KiSwapEvent.Header.Type==EventSynchronizationObject &&
          !KiSwapEvent.Header.SignalState && IsListEmpty(&KiSwapEvent.Header.WaitListHead));
    CHECK(IsListEmpty(&KiWaitListHead) && !KiProcessInSwapListHead.Next &&
          !KiProcessOutSwapListHead.Next && !KiStackInSwapListHead.Next);
    for (ULONG i=0;i<MAXIMUM_PRIORITY;++i)
        CHECK(IsListEmpty(&KiDispatcherReadyListHead[i]));
    for (ULONG i=0;i<TIMER_TABLE_SIZE;++i)
        CHECK(IsListEmpty(&KiTimerTableListHead[i]));
    for (ULONG i=0;i<NUMBER_SERVICE_TABLES;++i)
        CHECK(!KeServiceDescriptorTable[i].Base && !KeServiceDescriptorTable[i].Limit &&
              !KeServiceDescriptorTableShadow[i].Base && !KeServiceDescriptorTableShadow[i].Limit);
    XpTestReport("PASS: original XP KiInitSystem initialized the dispatcher and global timer DPC\n");
}
static VOID check_loaded_kernel(PXP_ARM64_BOOT_BLOCK block)
{
    const XP_ARM64_BOOT_IMAGE *image=&block->KernelImage;
    const IMAGE_NT_HEADERS64 *nt=(PVOID)((ULONG_PTR)&__ImageBase+__ImageBase.e_lfanew);
    CHECK((ULONG_PTR)&__ImageBase==image->VirtualBase &&
          nt->OptionalHeader.ImageBase==image->PreferredBase && image->VirtualBase!=image->PreferredBase);
    CHECK(nt->Signature==IMAGE_NT_SIGNATURE && nt->OptionalHeader.Subsystem==IMAGE_SUBSYSTEM_NATIVE);
    CHECK(image->FileSize>0 && image->Relocations>0 && image->Size==nt->OptionalHeader.SizeOfImage);
    CHECK(image->EntryRva==nt->OptionalHeader.AddressOfEntryPoint);
    CHECK(image->VirtualBase+image->EntryRva==(ULONG_PTR)KiArm64EnterBoot);
    CHECK(block->Pcr.TrapHandler==KiArm64DispatchBootTrap);
    PLIST_ENTRY head=&block->Nt.LoadOrderListHead;
    CHECK(head->Flink==&block->KernelModule.InLoadOrderLinks && head->Blink==head->Flink);
    PKLDR_DATA_TABLE_ENTRY module=CONTAINING_RECORD(head->Flink,KLDR_DATA_TABLE_ENTRY,InLoadOrderLinks);
    CHECK(module->InLoadOrderLinks.Flink==head && module->InLoadOrderLinks.Blink==head);
    CHECK((ULONG_PTR)module->DllBase==image->VirtualBase && module->EntryPoint==(PVOID)KiArm64EnterBoot &&
          module->SizeOfImage==image->Size && module->LoadCount==1);
    CHECK(module->BaseDllName.Length==sizeof(L"ntoskrnl.exe")-sizeof(WCHAR));
    CHECK(module->BaseDllName.Buffer[0]==L'n' && module->BaseDllName.Buffer[11]==L'e');

    BOOLEAN owned=FALSE;
    head=&block->Nt.MemoryDescriptorListHead;
    for (PLIST_ENTRY entry=head->Flink;entry!=head;entry=entry->Flink) {
        PMEMORY_ALLOCATION_DESCRIPTOR range=CONTAINING_RECORD(entry,MEMORY_ALLOCATION_DESCRIPTOR,ListEntry);
        ULONG64 start=(ULONG64)range->BasePage<<PAGE_SHIFT;
        ULONG64 end=start+((ULONG64)range->PageCount<<PAGE_SHIFT);
        if (image->Base>=start && image->Base+image->Size<=end) {
            CHECK(range->MemoryType==LoaderLoadedProgram);
            owned=TRUE;
            break;
        }
    }
    CHECK(owned);
    CHECK(MmArm64QueryPhysicalAddress(image->VirtualBase)==image->Base);
    CHECK(MmArm64QueryPhysicalAddress(image->Base)==~0ULL);
    CHECK((MmArm64QueryTranslation(image->VirtualBase,TRUE)&0x7f)==0x1f);
    CHECK((MmArm64QueryTranslation(image->VirtualBase+image->EntryRva,TRUE)&0x7f)==0x1f);
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)&map_test)==
          image->Base+((ULONG_PTR)&map_test-image->VirtualBase));
    CHECK(!(MmArm64QueryTranslation((ULONG_PTR)&map_test,TRUE)&1));

    ULONG_PTR pc, vbar;
    __asm__ volatile("adr %0, .\nmrs %1, vbar_el1" : "=r"(pc), "=r"(vbar));
    CHECK(pc>=image->VirtualBase && pc-image->VirtualBase<image->Size);
    CHECK(vbar==(ULONG_PTR)KiArm64VectorTable && vbar>=image->VirtualBase &&
          vbar-image->VirtualBase<image->Size);
    XpTestReport("Native PE execution pc=");XpTestHex(pc);
    XpTestReport(" vbar=");XpTestHex(vbar);XpTestReport("\n");
    XpTestReport("Native PE evidence base=");XpTestHex(image->VirtualBase);
    XpTestReport(" physical=");XpTestHex(image->Base);
    XpTestReport(" preferred=");XpTestHex(image->PreferredBase);
    XpTestReport(" entry=");XpTestHex(image->VirtualBase+image->EntryRva);
    XpTestReport(" file=");XpTestHex(image->FileSize);
    XpTestReport(" image=");XpTestHex(image->Size);
    XpTestReport(" dir64=");XpTestHex(image->Relocations);XpTestReport("\n");
    XpTestReport("PASS: separate native NT PE loaded from the boot volume and entered after firmware exit\n");
}
__declspec(noreturn) VOID KiArm64BootContinuation(PXP_ARM64_BOOT_BLOCK block, NTSTATUS status)
{
    CHECK(block != NULL);
    XpTestReport("Boot status=");XpTestHex((ULONG)status);
    XpTestReport(" HAL status=");XpTestHex((ULONG)block->HalStatus);XpTestReport("\n");
    CHECK(status == STATUS_SUCCESS);
    check_loaded_kernel(block);
    check_original_system_init();
    check_map_rules();
    XpCheckClockContracts();
    XpTestReport("RTC evidence utc=");XpTestHex(block->Time.Utc.QuadPart);
    XpTestReport(" timezone=");XpTestHex((USHORT)block->Time.Rtc.TimeZone);
    XpTestReport(" daylight=");XpTestHex(block->Time.Rtc.Daylight);
    XpTestReport(" resolution=");XpTestHex(block->Time.Capabilities.Resolution);
    XpTestReport(" call_counter=");XpTestHex(block->Time.CounterAfter-block->Time.CounterBefore);
    XpTestReport("\n");
    XpTestReport("ACPI platform status="); XpTestHex((ULONG)block->PlatformStatus);
    XpTestReport(" GICD="); XpTestHex(block->Platform.GicDistributor);
    XpTestReport(" GICC="); XpTestHex(block->Platform.GicCpuInterface);
    XpTestReport(" timer="); XpTestHex(block->Platform.TimerIrq);
    XpTestReport("\n");
    CHECK(block->ExitAttempted && block->BootServicesExited && block->ExitAttempts >= 2);
    CHECK(block->ExitAttempts==2);
    CHECK(KeLoaderBlock == &block->Nt && KeNumberProcessors == 1);
    CHECK(KeGetPcr() == &block->Pcr && KeGetCurrentPrcb() == &block->Pcr.Prcb);
    CHECK(KeGetCurrentThread() == NULL && block->Nt.Thread == 0);
    CHECK(KeGetCurrentNode() == &block->InitialNode && block->InitialNode.ProcessorMask == 1);
    CHECK(block->Tables.Active && block->Tables.UsedPages >= block->Tables.CopiedPages);
    CHECK(block->Tables.CopiedPages != 0 && block->Tables.UsedPages <= MM_BOOT_TABLE_PAGES);
    ULONG64 ttbr0, ttbr1, tcr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    CHECK(ttbr0 == block->Tables.Root && ttbr0 != block->Tables.OriginalTtbr0);
    CHECK(ttbr1==block->Tables.KernelRoot && ttbr1!=ttbr0 && ttbr1!=block->Tables.OriginalTtbr1);
    CHECK(!(tcr & ((1ULL<<38)|(3ULL<<22))) && tcr==block->Tables.ActiveTcr);
    CHECK(((tcr>>16)&63)==16 && ((tcr>>24)&63)==((tcr>>8)&63));
    CHECK(((tcr >> 30) & 3) == 2); /* A57 TG1 erratum condition. */
    ULONG64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    CHECK(sctlr == block->Tables.OriginalSctlr);
    CHECK(block->Tables.SwitchSctlr == (sctlr & ~1ULL));
    ULONG_PTR transition=block->Tables.RootSwitchAddress;
    CHECK(transition && !(transition&127) && block->Tables.RootSwitchSize &&
          block->Tables.RootSwitchSize<=128 && MmArm64QueryPhysicalAddress(transition)==transition);
    XpTestReport("PASS: bounded MMU-off root transition, SCTLR restore and A57 TG1 constraint\n");
    ULONG64 pool_end = block->Tables.PoolBase + ((ULONG64)block->Tables.PoolPages << PAGE_SHIFT);
    ULONG64 used_pool_end=block->Tables.PoolBase+((ULONG64)block->Tables.UsedPages<<PAGE_SHIFT);
    CHECK(ttbr1>=block->Tables.PoolBase && ttbr1<used_pool_end && !(ttbr1&(PAGE_SIZE-1)));
    /* No child table in either active tree refers back to firmware memory. */
    for (ULONG page = 0; page < block->Tables.UsedPages; ++page) {
        ULONG level = block->Tables.Levels[page];
        const ULONG64 *table = (PVOID)(ULONG_PTR)(block->Tables.PoolBase + ((ULONG64)page << PAGE_SHIFT));
        if (level == 3) continue;
        for (ULONG i = 0; i < 512; ++i) {
            if ((table[i] & 3) != 3) continue;
            ULONG64 address = table[i] & 0x0000fffffffff000ULL;
            CHECK(address >= block->Tables.PoolBase && address < used_pool_end);
        }
    }
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)block) == (ULONG_PTR)block);
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)KiArm64BootContinuation)==
          block->KernelImage.Base+((ULONG_PTR)KiArm64BootContinuation-block->KernelImage.VirtualBase));
    CHECK(MmArm64QueryPhysicalAddress(block->StackAllocationBase) == ~0ULL);
    CHECK(*block->Tables.GuardPte == 0);
    __asm__ volatile(".globl XpGuardRead\nXpGuardRead:\nldr x9, [%0]"
                     :: "r"(block->StackAllocationBase) : "x9", "memory");
    CHECK(block->Pcr.TrapCount == 1);
    MmArm64SetBootGuardPresent(&block->Tables, TRUE);
    CHECK(MmArm64QueryPhysicalAddress(block->StackAllocationBase) == block->StackAllocationBase);
    volatile ULONG64 *guard = (PVOID)(ULONG_PTR)block->StackAllocationBase;
    CHECK(*guard == 0);
    *guard = 0x716f776e65647074ULL;
    CHECK(*guard == 0x716f776e65647074ULL);
    MmArm64SetBootGuardPresent(&block->Tables, FALSE);
    CHECK(MmArm64QueryPhysicalAddress(block->StackAllocationBase) == ~0ULL);
    XpTestReport("PASS: owned TTBR0 tree, real guard-page fault, mapping restore and re-protection\n");
    ULONG_PTR sp; ULONG64 daif, vbar;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
    CHECK((daif & 0x3c0) == 0x3c0 && vbar == (ULONG_PTR)KiArm64VectorTable);
    CHECK(sp >= block->StackBase && sp < block->Nt.KernelStack);
    CHECK(block->FirmwareStack<block->StackBase || block->FirmwareStack>=block->Nt.KernelStack);
    CHECK(block->InitialProcess.Pcb.Header.Type == ProcessObject);
    CHECK(block->InitialProcess.Pcb.Header.Size == sizeof(KPROCESS) / sizeof(LONG));
    CHECK(IsListEmpty(&block->InitialProcess.Pcb.Header.WaitListHead));
    CHECK(IsListEmpty(&block->InitialProcess.Pcb.ThreadListHead));
    CHECK(IsListEmpty(&block->InitialProcess.Pcb.ReadyListHead));
    CHECK(IsListEmpty(&block->InitialProcess.Pcb.ProfileListHead));
    CHECK(block->InitialProcess.Pcb.StackCount == MAXSHORT);
    CHECK(block->InitialProcess.Pcb.Affinity == 1 && block->InitialProcess.Pcb.BasePriority == 0);
    CHECK(block->InitialProcess.Pcb.State == ProcessInMemory && block->InitialProcess.Pcb.ThreadQuantum != 0);
    CHECK(block->InitialProcess.Pcb.DirectoryTableBase[0] ==
          (block->Pcr.Prcb.ProcessorState.SpecialRegisters.Ttbr0El1 & 0x0000fffffffff000ULL));
    CHECK(block->InitialProcess.Pcb.DirectoryTableBase[1] == ttbr1);
    CHECK(KeTickCount.QuadPart == 0); /* No timer is running yet. */

    check_boot_pfn_map(block);
    __asm__ volatile(".globl XpPostExitBrk\nXpPostExitBrk:\nbrk #0x71" ::: "memory");
    CHECK(block->Pcr.TrapCount == 2);
    check_mmpfn_database(block);
    XpCheckStackFault(block->KernelImage.Base);
    XpCheckStackFault((block->KernelImage.Base+block->KernelImage.EntryRva)&~7ULL);
    XpTestReport("PASS: native kernel executes through TTBR1 with the lower image aliases retired\n");
    ULONG64 kernel_data_nx=check_execute_denied((ULONG_PTR)&map_test);
    XpTestReport("Native PE data NX ESR=");XpTestHex(kernel_data_nx);XpTestReport("\n");
    check_high_shared_page(block);
    XpCheckUserSpaces(block);
    check_boot_io(block);
    check_nonpaged_pool_pages(&block->Tables);
    XpCheckBootStacks(&block->Tables);
    XpCheckKernelContexts(block);
    XpCheckUnwind(block);
    XpCheckScopeExceptions(block);
    XpCheckThreadContextBoundary(block);
    XpCheckHardwareInterrupts(block);
    XpTestReport("PASS: real UEFI exit/retry, reserved map, independent stack, XP process initialization and post-exit EL1 trap\n");
    XpRunOriginalThreads(block);
}
