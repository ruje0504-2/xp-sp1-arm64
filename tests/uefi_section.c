#include "xp-mm-section.h"
#include "../base/ntos/mm/arm64/bootmap.h"
#include "../base/ntos/ob/arm64/bootobject.h"
#include "../base/ntos/mm/arm64/userspace.h"
#include "uefi_test.h"

static ULONG check_physical_section(VOID)
{
    POBJECT_TYPE type = MmSectionObjectType;
    CHECK(type && type->TypeInfo.PoolType == PagedPool && type->TypeInfo.UseDefaultObject &&
          type->TypeInfo.DefaultPagedPoolCharge == sizeof(SECTION) + sizeof(OBJECT_HEADER) + sizeof(OBJECT_HEADER_NAME_INFO) &&
          type->TypeInfo.ValidAccessMask == (SECTION_ALL_ACCESS | SYNCHRONIZE) &&
          type->TypeInfo.DeleteProcedure && type->TotalNumberOfObjects == 1 && !type->TotalNumberOfHandles);
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\ObjectTypes\\Section");
    PVOID found;
    CHECK(ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE, ObpTypeObjectType,
          &found) == STATUS_SUCCESS && found == type);
    ObDereferenceObject(found);

    RtlInitUnicodeString(&name, L"\\Device\\PhysicalMemory");
    HANDLE handle;
    CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_CASE_INSENSITIVE, type,
          SECTION_QUERY, &handle) == STATUS_SUCCESS);
    PSECTION object;
    CHECK(ObReferenceObjectByHandle(handle, SECTION_QUERY, type, KernelMode, (PVOID *)&object, NULL) ==
          STATUS_SUCCESS);
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
    PSEGMENT segment = object->Segment;
    PCONTROL_AREA area = segment->ControlArea;
    CHECK((header->Flags & OB_FLAG_PERMANENT_OBJECT) && header->SecurityDescriptor &&
          object->InitialPageProtection == PAGE_READWRITE && !object->u.LongFlags &&
          area->Segment == segment && area->u.Flags.PhysicalMemory && !area->FilePointer &&
          area->NumberOfSectionReferences == 1 && !area->NumberOfMappedViews &&
          !segment->TotalNumberOfPtes && !segment->PrototypePte);
    ULONG64 tcr;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    const ULONG widths[] = {32, 36, 40, 42, 44, 48};
    ULONG encoding = (tcr >> 32) & 7;
    CHECK(encoding < sizeof(widths) / sizeof(*widths));
    ULONG bits = widths[encoding];
    SECTION_BASIC_INFORMATION info;
    SIZE_T bytes = 0;
    CHECK(NtQuerySection(handle, SectionBasicInformation, &info, sizeof(info) - 1, &bytes) ==
          STATUS_INFO_LENGTH_MISMATCH && !bytes);
    CHECK(NtQuerySection(handle, SectionBasicInformation, &info, sizeof(info), &bytes) ==
          STATUS_SUCCESS && bytes == sizeof(info) && !info.BaseAddress && !info.AllocationAttributes &&
          info.MaximumSize.QuadPart == (LONGLONG)((1ULL << bits) - 1));
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS && !header->HandleCount);
    ObDereferenceObject(object);
    return bits;
}

static ULONG check_reserved_section(VOID)
{
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    LARGE_INTEGER size = {0};
    HANDLE handle;
    CHECK(NtCreateSection(&handle, SECTION_ALL_ACCESS, &attributes, &size,
          PAGE_READWRITE, SEC_RESERVE, NULL) == STATUS_INVALID_PARAMETER_4 && !handle);
    size.QuadPart = PAGE_SIZE;
    CHECK(NtCreateSection(&handle, SECTION_ALL_ACCESS, &attributes, &size,
          PAGE_READWRITE, SEC_COMMIT, NULL) == STATUS_NOT_SUPPORTED && !handle);

    const ULONG pages = 1026;
    size.QuadPart = (LONGLONG)(pages - 1) * PAGE_SIZE + 1;
    CHECK(NtCreateSection(&handle, SECTION_ALL_ACCESS, &attributes, &size,
          PAGE_READWRITE, SEC_RESERVE, NULL) == STATUS_SUCCESS && (LONG_PTR)handle < 0);
    PSECTION section;
    CHECK(ObReferenceObjectByHandle(handle, SECTION_MAP_READ, MmSectionObjectType, KernelMode,
          (PVOID *)&section, NULL) == STATUS_SUCCESS);
    PSEGMENT segment = section->Segment;
    PCONTROL_AREA area = segment->ControlArea;
    PSUBSECTION subsection = (PSUBSECTION)(area + 1);
    CHECK(area->Segment == segment && area->NumberOfSectionReferences == 1 &&
          area->NumberOfUserReferences == 1 && area->u.Flags.Reserve && !area->FilePointer &&
          !area->NumberOfPfnReferences && !area->NumberOfMappedViews &&
          segment->TotalNumberOfPtes == pages && segment->NonExtendedPtes == pages &&
          subsection->ControlArea == area && subsection->SubsectionBase == segment->PrototypePte &&
          subsection->PtesInSubsection == pages &&
          subsection->u.SubsectionFlags.Protection == segment->SegmentPteTemplate.u.Soft.Protection &&
          !segment->SegmentPteTemplate.u.Soft.Valid && segment->SegmentPteTemplate.u.Soft.Protection &&
          !segment->NumberOfCommittedPages);
    ULONG_PTR first = (ULONG_PTR)segment->PrototypePte, last = (ULONG_PTR)&segment->PrototypePte[pages - 1];
    CHECK(MmArm64QueryPhysicalAddress(first) != ~0ULL && MmArm64QueryPhysicalAddress(last) != ~0ULL);
    for (ULONG i = 0; i < pages; ++i) CHECK(!segment->PrototypePte[i].u.Long);

    HANDLE second;
    CHECK(ObArm64CreateBootHandle(section, SECTION_QUERY, TRUE, &second) == STATUS_SUCCESS);
    ObDereferenceObject(section);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);
    SECTION_BASIC_INFORMATION info;
    CHECK(NtQuerySection(second, SectionBasicInformation, &info, sizeof(info), NULL) == STATUS_SUCCESS &&
          !info.BaseAddress && info.AllocationAttributes == SEC_RESERVE &&
          info.MaximumSize.QuadPart == (LONGLONG)pages * PAGE_SIZE);
    CHECK(ObArm64CloseBootObjectHandle(second) == STATUS_SUCCESS &&
          MmSectionObjectType->TotalNumberOfObjects == 1 && !MmSectionObjectType->TotalNumberOfHandles &&
          MmArm64QueryPhysicalAddress(first) == ~0ULL && MmArm64QueryPhysicalAddress(last) == ~0ULL);
    return pages;
}

static ULONG check_segment_worker(VOID)
{
    PEPROCESS system = PsInitialSystemProcess;
    CHECK(IsListEmpty(&system->PhysicalVadList));
    PETHREAD worker = NULL;
    PLIST_ENTRY head = &system->ThreadListHead;
    for (PLIST_ENTRY link = head->Flink; link != head; link = link->Flink) {
        PETHREAD candidate = CONTAINING_RECORD(link, ETHREAD, ThreadListEntry);
        if (candidate->StartAddress != (PVOID)MiDereferenceSegmentThread) continue;
        CHECK(!worker);
        worker = candidate;
    }
    CHECK(worker && worker->ThreadsProcess == system && worker->Cid.UniqueThread &&
          worker->MemoryMaker && worker->Tcb.State == Waiting && worker->Tcb.WaitReason == WrVirtualMemory &&
          worker->Tcb.Priority == LOW_REALTIME_PRIORITY + 2);
    PKWAIT_BLOCK first = worker->Tcb.WaitBlockList;
    CHECK(first && first->Object == &MmDereferenceSegmentHeader.Semaphore &&
          first->NextWaitBlock->Object == &MmUnusedSegmentCleanup &&
          first->NextWaitBlock->NextWaitBlock == first && IsListEmpty(&MmDereferenceSegmentHeader.ListHead));
    ULONG before = worker->Tcb.ContextSwitches;
    CHECK(!KeSetEvent(&MmUnusedSegmentCleanup, 0, FALSE));
    LARGE_INTEGER delay = {.QuadPart = -10000};
    ULONGLONG deadline = KeQueryInterruptTime() + 10000000;
    while (worker->Tcb.ContextSwitches == before || worker->Tcb.State != Waiting) {
        CHECK(KeQueryInterruptTime() < deadline);
        CHECK(KeDelayExecutionThread(KernelMode, FALSE, &delay) == STATUS_SUCCESS);
    }
    CHECK(!KeReadStateEvent(&MmUnusedSegmentCleanup) && worker->Tcb.WaitBlockList == first);
    XpTestReport(" worker_tid="); XpTestHex((ULONG_PTR)worker->Cid.UniqueThread);
    return worker->Tcb.ContextSwitches - before;
}

typedef struct {
    ULONG64 ReadPar, WritePar;
    ULONG Value;
} USER_PROBE;

VOID XpProbeUserSpace(ULONG64 Root, ULONG_PTR Address, ULONG Store, USER_PROBE *Probe);

static VOID check_low_user_pages(VOID)
{
    MM_ARM64_USER_SPACE first = {0}, second = {0};
    PXP_ARM64_BOOT_BLOCK boot = CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    CHECK(MmArm64CreateUserSpace(&first, boot) == STATUS_SUCCESS);
    CHECK(MmArm64CreateUserSpace(&second, boot) == STATUS_SUCCESS);
    CHECK(MmArm64MapUserPages(&first, 0x10000, 2 * PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS);
    CHECK(MmArm64MapUserPages(&second, 0x10000, 2 * PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS);
    ULONG64 a = first.Root, b = second.Root;
    CHECK(a != b);
    USER_PROBE x, y;
    XpProbeUserSpace(a, 0x10004, 0x12345678, &x);
    XpProbeUserSpace(b, 0x10004, 0x76543210, &y);
    const ULONG64 mask = 0x0000fffffffff000ULL;
    CHECK(!(x.ReadPar & 1) && !(x.WritePar & 1) && !(y.ReadPar & 1) && !(y.WritePar & 1) &&
          (x.ReadPar & mask) != (y.ReadPar & mask) && !x.Value && !y.Value);
    XpProbeUserSpace(a, 0x10004, 0, &x);
    XpProbeUserSpace(b, 0x10004, 0, &y);
    CHECK(x.Value == 0x12345678 && y.Value == 0x76543210);
    XpProbeUserSpace(a, 0x11ffc, 1, &x);
    CHECK(!(x.WritePar & 1) && x.Value == 0);
    XpProbeUserSpace(a, 0x12000, 0, &x);
    CHECK((x.ReadPar & 1) && (x.WritePar & 1));
    XpProbeUserSpace(b, KI_USER_SHARED_DATA, 0, &x);
    CHECK((x.ReadPar & 1) && (x.WritePar & 1));
    ULONG offset = FIELD_OFFSET(KUSER_SHARED_DATA, ImageNumberLow);
    XpProbeUserSpace(a, MM_ARM64_USER_SHARED_VA + offset, 0, &x);
    XpProbeUserSpace(b, MM_ARM64_USER_SHARED_VA + offset, 0, &y);
    CHECK(!(x.ReadPar & 1) && !(y.ReadPar & 1) && (x.WritePar & 1) && (y.WritePar & 1) &&
          (x.ReadPar & mask) == MmArm64QueryPhysicalAddress(KI_USER_SHARED_DATA) &&
          (y.ReadPar & mask) == (x.ReadPar & mask) && x.Value == 0xaa64aa64 && x.Value == y.Value);
    MmArm64DestroyUserSpace(&second);
    MmArm64DestroyUserSpace(&first);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    XpTestReport("User space evidence root_a="); XpTestHex(a);
    XpTestReport(" root_b="); XpTestHex(b);
    XpTestReport(" shared_pa="); XpTestHex(x.ReadPar & mask);
    XpTestReport(" shared_value="); XpTestHex(x.Value); XpTestReport("\n");
    XpTestReport("PASS: isolated low ARM64 user pages and read-only shared data via unprivileged accesses\n");
}

VOID XpCheckBootSectionPhase1(VOID)
{
    check_low_user_pages();
    ULONG bits = check_physical_section();
    ULONG ptes = check_reserved_section();
    XpTestReport("Section Phase 1 evidence pa_bits="); XpTestHex(bits);
    XpTestReport(" prototype_ptes="); XpTestHex(ptes);
    ULONG wakes = check_segment_worker();
    XpTestReport(" worker_wakes="); XpTestHex(wakes); XpTestReport("\n");
    XpTestReport("PASS: original XP Section initialization, physical descriptor and reserved segment rundown\n");
}
