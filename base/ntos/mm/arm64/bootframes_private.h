#ifndef XP_ARM64_BOOT_FRAMES_PRIVATE_H
#define XP_ARM64_BOOT_FRAMES_PRIVATE_H
#include "bootframes.h"
#include "xp-mmpfn.h"

/* Reserve VA in the NT64 physically-addressable region, above the native PE. */
#define MM_BOOT_PFN_DATABASE_BASE 0xfffff88000000000ULL
#define MM_EMPTY_LIST ((PFN_NUMBER)-1)
#define MI_PFN_ELEMENT(index) (&MmPfnDatabase[index])
extern PMMPFN MmPfnDatabase;
extern MMPFNLIST MmFreePageListHead;
extern PFN_COUNT MmAvailablePages;
extern PFN_NUMBER MmSystemLockPagesCount;
extern ULONG MmReferenceCountCheck;
extern PFN_NUMBER MmLowestPhysicalPage, MmHighestPhysicalPage;
NTSTATUS MmArm64PinBootFrame(PFN_NUMBER Frame, volatile ULONG64 *Pte, BOOLEAN Write);
BOOLEAN MmArm64BootFramePinned(PFN_NUMBER Frame);
VOID MmArm64UnpinBootFrame(PFN_NUMBER Frame);
VOID MiArm64InsertBootFreePage(PFN_NUMBER PageFrameIndex);
VOID MiArm64RemoveBootFreePage(PFN_NUMBER Page);

static const BOOLEAN boot_frame_types[LoaderMaximum]={
    [LoaderFree]=TRUE,
    [LoaderLoadedProgram]=TRUE,
    [LoaderMemoryData]=TRUE,
};

PFN_NUMBER MiPagesInLoaderBlock(PLOADER_PARAMETER_BLOCK LoaderBlock, PBOOLEAN IncludeType);

static FORCEINLINE SIZE_T BootFrameDescriptorBytes(VOID)
{
    SIZE_T bytes=FIELD_OFFSET(PHYSICAL_MEMORY_DESCRIPTOR,Run)+
        XP_ARM64_BOOT_DESCRIPTOR_CAPACITY*sizeof(PHYSICAL_MEMORY_RUN);
    return (bytes+PAGE_SIZE-1)&~(SIZE_T)(PAGE_SIZE-1);
}

/* Adjacent PFN ranges may occupy the same metadata page. Visit each page
 * containing an included MMPFN once, leaving full pages in address holes unmapped. */
static FORCEINLINE BOOLEAN MmArm64NextBootPfnPageRange(
    const LIST_ENTRY *head, const LIST_ENTRY **cursor, PPFN_NUMBER first, PPFN_NUMBER limit)
{
    while (*cursor != head) {
        const MEMORY_ALLOCATION_DESCRIPTOR *range = CONTAINING_RECORD(
            *cursor, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        *cursor = (*cursor)->Flink;
        if ((ULONG)range->MemoryType >= LoaderMaximum || !boot_frame_types[range->MemoryType])
            continue;
        PFN_NUMBER start = ((PFN_NUMBER)range->BasePage * sizeof(MMPFN)) >> PAGE_SHIFT;
        PFN_NUMBER end = (((PFN_NUMBER)range->BasePage + range->PageCount) * sizeof(MMPFN) +
                          PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (end <= *limit) continue;
        *first = start < *limit ? *limit : start;
        *limit = end;
        return TRUE;
    }
    return FALSE;
}
#endif
