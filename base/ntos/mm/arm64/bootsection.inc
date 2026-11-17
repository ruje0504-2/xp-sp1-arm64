/* Native dependencies of the selected XP section routines. */
#define ExAcquireSpinLock(Lock, OldIrql) (*(OldIrql) = KeAcquireSpinLockRaiseToDpc(Lock))
#define ExReleaseSpinLock KeReleaseSpinLock

static KSPIN_LOCK section_lock;
static ULONG physical_address_bits;

static BOOLEAN MiArm64BootBbtUnused(PLOADER_PARAMETER_BLOCK loader)
{
    const LIST_ENTRY *head = &loader->MemoryDescriptorListHead;
    for (const LIST_ENTRY *link = head->Flink; link != head; link = link->Flink) {
        if (CONTAINING_RECORD(link, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry)->MemoryType == LoaderBBTMemory)
            return FALSE;
    }
    return TRUE;
}

static ULONG MiArm64PhysicalAddressBits(VOID)
{
    static const UCHAR address_bits[] = {32, 36, 40, 42, 44, 48};
    ULONG64 tcr, mmfr0;
    __asm__ volatile("mrs %0, tcr_el1\nmrs %1, id_aa64mmfr0_el1" : "=r"(tcr), "=r"(mmfr0));
    ULONG ips = (tcr >> 32) & 7;
    ULONG parange = mmfr0 & 15;
    if (ips >= sizeof(address_bits) || ips > parange) return 0;
    return address_bits[ips];
}

static VOID MiArm64UnsupportedSection(PVOID object, ULONG_PTR detail)
{
    KeBugCheckEx(MEMORY_MANAGEMENT, 0xa640, (ULONG_PTR)object, detail, 0);
}

/* No prototype PTE can be mapped or paged out before the view and paging
 * paths are online. Check the whole array before releasing its pool. */
static VOID MiArm64ValidateSegmentDeletion(PSEGMENT segment)
{
    PCONTROL_AREA area = segment->ControlArea;
    if (!area->u.Flags.BeingDeleted || area->Segment != segment ||
        area->NumberOfSectionReferences || area->NumberOfUserReferences ||
        area->NumberOfMappedViews || area->NumberOfPfnReferences || area->FilePointer ||
        area->WaitingForDeletion || area->DereferenceList.Flink ||
        segment->WritableUserReferences || segment->NumberOfCommittedPages || segment->ExtendInfo)
        MiArm64UnsupportedSection(segment, area->u.LongFlags);
    if (area->u.Flags.PhysicalMemory) return;

    PSUBSECTION subsection = (PSUBSECTION)(area + 1);
    if (area->NumberOfSubsections != 1 || !area->u.Flags.Reserve || area->u.Flags.Commit ||
        subsection->ControlArea != area || subsection->NextSubsection ||
        subsection->SubsectionBase != segment->PrototypePte ||
        subsection->PtesInSubsection != segment->NonExtendedPtes ||
        segment->NonExtendedPtes != segment->TotalNumberOfPtes)
        MiArm64UnsupportedSection(segment, area->u.LongFlags);

    for (ULONG i = 0; i < segment->NonExtendedPtes; ++i)
        if (segment->PrototypePte[i].u.Long) MiArm64UnsupportedSection(segment, i);
}

static VOID MiArm64ValidateUnusedLists(VOID)
{
    if (!IsListEmpty(&MmUnusedSegmentList) || !IsListEmpty(&MmUnusedSubsectionList) ||
        MmUnusedSegmentForceFree)
        MiArm64UnsupportedSection(&MmUnusedSegmentList, MmUnusedSegmentForceFree);
}
