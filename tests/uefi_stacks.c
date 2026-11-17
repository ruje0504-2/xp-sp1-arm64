#include "../base/boot/arm64/boot.h"
#include "uefi_test.h"
#include "../base/ntos/mm/arm64/bootframes_private.h"
static MM_ARM64_BOOT_STACK handles[MM_BOOT_STACK_COUNT];
static ULONG64 expected_fault;
static ULONG fault_count, expected_translation_status;
extern VOID XpReadBootStack(ULONG_PTR Address);
extern const UCHAR XpBootStackReadFault[];

BOOLEAN XpHandleBootStackFault(PKTRAP_FRAME frame)
{
    if (frame->Pc != (ULONG_PTR)XpBootStackReadFault) return FALSE;
    CHECK(((frame->Esr >> 26) & 63) == 0x25 &&
          (frame->Esr & 63) == expected_translation_status);
    CHECK(!(frame->Esr & (1ULL << 6)) && frame->Far == expected_fault);
    ++fault_count;
    frame->Pc += 4;
    return TRUE;
}
VOID XpCheckTranslationFault(ULONG_PTR address, ULONG level)
{
    expected_fault = address;
    expected_translation_status = 4 + level;
    ULONG old_count = fault_count;
    XpReadBootStack(address);
    CHECK(fault_count == old_count + 1);
}
VOID XpCheckStackFault(ULONG_PTR address)
{
    XpCheckTranslationFault(address, 3);
}
VOID XpCheckBootStacks(PMM_ARM64_BOOT_TABLES tables)
{
    CHECK(tables->StacksPrepared);
    MM_ARM64_BOOT_PFN_COUNTS before=MmArm64BootFrameCounts();
    PFN_NUMBER observed[MM_BOOT_STACK_COUNT*MM_BOOT_STACK_DATA_PAGES];
    ULONG frames=0;
    for (ULONG i = 0; i < MM_BOOT_STACK_COUNT; ++i) {
        ULONG_PTR base = tables->StackArena + ((ULONG64)i * MM_BOOT_STACK_STRIDE << PAGE_SHIFT);
        CHECK(MmArm64QueryPhysicalAddress(base) == ~0ULL);
        CHECK(MmArm64QueryPhysicalAddress(base + PAGE_SIZE) == ~0ULL);
        CHECK(MmArm64AllocateBootStack(tables, &handles[i]) == STATUS_SUCCESS);
        CHECK(handles[i].Limit == base + PAGE_SIZE && handles[i].Top == base + MM_BOOT_STACK_STRIDE * PAGE_SIZE);
        CHECK(MmArm64QueryPhysicalAddress(base) == ~0ULL);
        MM_ARM64_BOOT_STACK_SLOT *slot=&tables->Stacks[handles[i].Index];
        for (ULONG page=0;page<MM_BOOT_STACK_DATA_PAGES;++page) {
            ULONG_PTR va=handles[i].Limit+page*PAGE_SIZE;
            PFN_NUMBER frame=slot->Frames[page];
            CHECK(frame && va!=(frame<<PAGE_SHIFT));
            CHECK(MmArm64QueryPhysicalAddress(va)==(frame<<PAGE_SHIFT));
            CHECK(MmArm64BootFrameOwnedBy(frame,slot));
            const MMPFN *pfn = MI_PFN_ELEMENT(frame);
            CHECK(pfn->u3.e1.PageLocation == ActiveAndValid && pfn->u3.e2.ReferenceCount == 1 &&
                  pfn->u2.ShareCount == 1 && pfn->u4.KernelStack &&
                  pfn->PteAddress == (PMMPTE)slot->Ptes[page + 1] &&
                  pfn->u4.PteFrame == ((ULONG_PTR)pfn->PteAddress >> PAGE_SHIFT));
            for (ULONG previous=0;previous<frames;++previous) CHECK(observed[previous]!=frame);
            observed[frames++]=frame;
        }
        volatile ULONG64 *memory = (PVOID)handles[i].Limit;
        for (ULONG j = 0; j < KERNEL_STACK_SIZE / sizeof(ULONG64); ++j) {
            CHECK(memory[j] == 0);
            memory[j] = ((ULONG64)i << 32) | j | 0x1234000000000000ULL;
        }
    }
    CHECK(RtlNumberOfSetBits(&tables->StackBitmap) == MM_BOOT_STACK_COUNT);
    MM_ARM64_BOOT_PFN_COUNTS full=MmArm64BootFrameCounts();
    CHECK(full.Allocated==before.Allocated+frames && full.Free+frames==before.Free);
    CHECK(MmArm64ReturnBootFrame(handles,observed[0])==STATUS_INVALID_HANDLE);
    CHECK(MmArm64ReturnBootFrame(&tables->Stacks[handles[0].Index],observed[0])==STATUS_DEVICE_BUSY);
    MM_ARM64_BOOT_STACK extra;
    CHECK(MmArm64AllocateBootStack(tables, &extra) == STATUS_INSUFFICIENT_RESOURCES);
    XpCheckStackFault(handles[0].Limit - PAGE_SIZE);
    MM_ARM64_BOOT_STACK stale = handles[0], bad = handles[0];
    bad.Top += 16;
    CHECK(MmArm64FreeBootStack(tables, &bad) == STATUS_INVALID_HANDLE);
    CHECK(MmArm64BindBootStack(tables, &handles[0], &expected_fault) == STATUS_SUCCESS);
    CHECK(MmArm64BindBootStack(tables, &handles[0], &fault_count) == STATUS_DEVICE_BUSY);
    CHECK(MmArm64FreeBootStack(tables, &handles[0]) == STATUS_DEVICE_BUSY);
    CHECK(MmArm64UnbindBootStack(tables, &handles[0], &fault_count) == STATUS_INVALID_HANDLE);
    CHECK(MmArm64UnbindBootStack(tables, &handles[0], &expected_fault) == STATUS_SUCCESS);
    CHECK(MmArm64FreeBootStack(tables, &handles[0]) == STATUS_SUCCESS);
    XpCheckStackFault(stale.Limit);
    CHECK(MmArm64FreeBootStack(tables, &stale) == STATUS_INVALID_HANDLE);
    CHECK(MmArm64AllocateBootStack(tables, &handles[0]) == STATUS_SUCCESS);
    CHECK(handles[0].Top == stale.Top && handles[0].Generation != stale.Generation);
    CHECK(MmArm64FreeBootStack(tables, &stale) == STATUS_INVALID_HANDLE);
    volatile ULONG64 *memory = (PVOID)handles[0].Limit;
    for (ULONG i = 0; i < KERNEL_STACK_SIZE / sizeof(ULONG64); ++i) CHECK(memory[i] == 0);
    for (ULONG i = 0; i < MM_BOOT_STACK_COUNT; ++i) {
        ULONG_PTR address = handles[i].Limit;
        CHECK(MmArm64FreeBootStack(tables, &handles[i]) == STATUS_SUCCESS);
        CHECK(MmArm64QueryPhysicalAddress(address) == ~0ULL);
    }
    CHECK(RtlNumberOfClearBits(&tables->StackBitmap) == MM_BOOT_STACK_COUNT);
    MM_ARM64_BOOT_PFN_COUNTS after=MmArm64BootFrameCounts();
    CHECK(after.Free==before.Free && after.Allocated==before.Allocated);
    for (ULONG i=0;i<frames;++i) {
        for (ULONG j=0;j<MM_BOOT_STACK_COUNT;++j)
            CHECK(!MmArm64BootFrameOwnedBy(observed[i],&tables->Stacks[j]));
        const MMPFN *pfn = MI_PFN_ELEMENT(observed[i]);
        CHECK(pfn->u3.e1.PageLocation == FreePageList && !pfn->u3.e2.ReferenceCount &&
              !pfn->PteAddress && !pfn->u4.EntireFrame);
    }
    XpTestReport("Stack PFN evidence frames=");XpTestHex(frames);
    XpTestReport(" free_before=");XpTestHex(before.Free);
    XpTestReport(" free_after=");XpTestHex(after.Free);XpTestReport("\n");
    XpTestReport("PASS: real boot-stack allocation, exhaustion, ownership, stale rejection, retirement faults and zeroed reuse\n");
}
