#include <ntos.h>
#include <ntimage.h>
#include "bootframes_private.h"
#define PA_MASK 0x0000fffffffff000ULL
#define PTE_VALID 1ULL
#define PTE_TABLE 2ULL
#define PTE_ATTR_INDEX (7ULL << 2)
#define PTE_NONSECURE (1ULL << 5)
#define PTE_EL0_ACCESS (1ULL << 6)
#define PTE_READ_ONLY (1ULL << 7)
#define PAR_NONSECURE (1ULL << 9)
#define PTE_INNER_SHAREABLE (3ULL << 8)
#define PTE_ACCESS_FLAG (1ULL << 10)
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_CONTIGUOUS (1ULL << 52)
#define TCR_TTBR1_FIELDS (0xffffULL << 16)
#define TCR_TBI1 (1ULL << 38)

static BOOLEAN identity_page(ULONG64 address)
{
    return !(address & 4095) && MmArm64QueryPhysicalAddress(address) == address &&
           MmArm64QueryPhysicalAddress(address + 4095) == address + 4095;
}
static ULONG64 *allocate_table(PMM_ARM64_BOOT_TABLES state, ULONG level)
{
    if (state->UsedPages >= state->PoolPages) return NULL;
    ULONG index = state->UsedPages++;
    state->Levels[index] = (UCHAR)level;
    ULONG64 *table = (PVOID)(ULONG_PTR)(state->PoolBase + ((ULONG64)index << PAGE_SHIFT));
    RtlZeroMemory(table, PAGE_SIZE);
    return table;
}
static ULONG64 *copy_table(PMM_ARM64_BOOT_TABLES state, ULONG64 source,
                           ULONG level, ULONG count)
{
    if (!identity_page(source) || (source >= state->PoolBase &&
        source < state->PoolBase + ((ULONG64)state->PoolPages << PAGE_SHIFT)))
        return NULL;
    for (ULONG i = 0; i < state->UsedPages; ++i)
        if (state->Sources[i] == source)
            return state->Levels[i] == level ?
                (PVOID)(ULONG_PTR)(state->PoolBase + ((ULONG64)i << PAGE_SHIFT)) : NULL;
    ULONG index = state->UsedPages;
    ULONG64 *copy = allocate_table(state, level);
    if (!copy) return NULL;
    state->Sources[index] = source;
    ++state->CopiedPages;
    const ULONG64 *original = (PVOID)(ULONG_PTR)source;
    for (ULONG i = 0; i < count; ++i) {
        ULONG64 entry = original[i];
        if ((entry & 3) == 3 && level < 3) {
            ULONG64 *child = copy_table(state, entry & PA_MASK, level + 1, 512);
            if (!child) return NULL;
            entry = (entry & ~PA_MASK) | (ULONG_PTR)child;
        } else if ((entry & PTE_VALID) &&
                   (level == 0 || (level == 3 && !(entry & PTE_TABLE)))) {
            return NULL; /* Reserved descriptor form, not a valid mapping. */
        }
        copy[i] = entry;
    }
    return copy;
}
static volatile ULONG64 *page_entry(PMM_ARM64_BOOT_TABLES state, ULONG64 address)
{
    if (address >= (1ULL << (64 - (state->OriginalTcr & 63)))) return NULL;
    ULONG64 *table = (PVOID)(ULONG_PTR)state->Root;
    for (ULONG level = state->StartLevel; level <= 3; ++level) {
        ULONG shift = 12 + (3 - level) * 9;
        ULONG index = (ULONG)((address >> shift) & 511);
        ULONG64 value = table[index];
        if (!(value & PTE_VALID) || (value & PTE_CONTIGUOUS)) return NULL;
        if (level == 3) {
            if ((value & PA_MASK) != address) return NULL;
            return &table[index];
        }
        if (value & PTE_TABLE) {
            table = (PVOID)(ULONG_PTR)(value & PA_MASK);
            continue;
        }
        /* Split a block OFFLINE, preserving every leaf permission/attribute.
         * No live break-before-make window can unmap the executing code. */
        ULONG64 base = value & PA_MASK;
        if (base & ((1ULL << shift) - 1)) return NULL;
        ULONG64 *child = allocate_table(state, level + 1);
        if (!child) return NULL;
        ULONG next_shift = shift - 9;
        ULONG64 attributes = value & ~(PA_MASK | PTE_TABLE);
        for (ULONG j = 0; j < 512; ++j)
            child[j] = attributes | (base + ((ULONG64)j << next_shift)) |
                       (level == 2 ? PTE_TABLE : 0);
        table[index] = (ULONG_PTR)child | 3;
        table = child;
    }
    return NULL;
}
static ULONG64 *upper_leaf_table(PMM_ARM64_BOOT_TABLES state, ULONG64 va)
{
    ULONG64 *table=(PVOID)(ULONG_PTR)state->KernelRoot;
    for (ULONG level=0;level<3;++level) {
        ULONG index=(ULONG)((va>>(39-level*9))&511);
        if (!(table[index]&PTE_VALID)) {
            ULONG64 *child=allocate_table(state,level+1);
            if (!child) return NULL;
            table[index]=(ULONG_PTR)child|PTE_VALID|PTE_TABLE;
        }
        table=(PVOID)(ULONG_PTR)(table[index]&PA_MASK);
    }
    return table;
}
static NTSTATUS map_upper_page(PMM_ARM64_BOOT_TABLES state, ULONG64 va, ULONG64 descriptor)
{
    ULONG64 *table=upper_leaf_table(state,va);
    if (!table) return STATUS_INSUFFICIENT_RESOURCES;
    ULONG index=(ULONG)((va>>PAGE_SHIFT)&511);
    if (table[index]) return STATUS_CONFLICTING_ADDRESSES;
    table[index]=descriptor;
    return STATUS_SUCCESS;
}

/* Run from the loader's identity interval. Materialize the native PE only
 * in the upper tree and retire its lower physical alias offline. */
static NTSTATUS map_boot_image(PMM_ARM64_BOOT_TABLES state,const XP_ARM64_BOOT_IMAGE *image,
                               ULONG64 limit)
{
    if (image->VirtualBase!=XP_ARM64_KERNEL_LOAD_VA || !image->Base || image->Base>=limit || !image->Size ||
        image->Size>XP_BOOT_IMAGE_LIMIT || image->Size>limit-image->Base ||
        ((image->Base | image->Size)&(PAGE_SIZE-1)) || !image->HeaderSize ||
        image->HeaderSize>image->Size || !image->SectionCount ||
        image->SectionCount>XP_BOOT_IMAGE_SECTIONS)
        return STATUS_INVALID_PARAMETER;
    ULONG header_end=(image->HeaderSize+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    ULONG section_end=header_end;
    for (ULONG i=0;i<image->SectionCount;++i) {
        const XP_ARM64_IMAGE_SECTION *section=&image->Sections[i];
        if (section->Rva<section_end || (section->Rva&(PAGE_SIZE-1)) ||
            !section->Size || section->Rva>=image->Size ||
            section->Size>image->Size-section->Rva ||
            !(section->Characteristics&IMAGE_SCN_MEM_READ) ||
            ((section->Characteristics&IMAGE_SCN_MEM_EXECUTE) &&
             (section->Characteristics&IMAGE_SCN_MEM_WRITE)))
            return STATUS_INVALID_PARAMETER;
        section_end=section->Rva+((section->Size+PAGE_SIZE-1)&~(PAGE_SIZE-1));
    }
    if (section_end!=image->Size) return STATUS_INVALID_PARAMETER;
    ULONG index=0;
    for (ULONG rva=0;rva<image->Size;rva+=PAGE_SIZE) {
        ULONG64 physical=image->Base+rva;
        volatile ULONG64 *pte=page_entry(state,physical);
        if (!pte) return STATUS_NOT_SUPPORTED;
        ULONG flags=0;
        if (rva<header_end) {
            flags=IMAGE_SCN_MEM_READ;
        } else {
            const XP_ARM64_IMAGE_SECTION *section=&image->Sections[index];
            if (rva>=section->Rva) {
                flags=section->Characteristics;
                if (rva+PAGE_SIZE>=section->Rva+section->Size) ++index;
            }
        }
        if (flags) {
            ULONG64 par=MmArm64QueryTranslation(physical,FALSE);
            if ((par&1) || (par&PA_MASK)!=physical || (par>>56)!=255 || ((par>>7)&3)!=3)
                return STATUS_NOT_SUPPORTED;
            ULONG64 value=physical|(*pte&PTE_ATTR_INDEX)|PTE_INNER_SHAREABLE|PTE_ACCESS_FLAG|
                PTE_UXN|PTE_VALID|PTE_TABLE;
            if (par&PAR_NONSECURE) value|=PTE_NONSECURE;
            if (!(flags&IMAGE_SCN_MEM_WRITE)) value|=PTE_READ_ONLY;
            if (!(flags&IMAGE_SCN_MEM_EXECUTE)) value|=PTE_PXN;
            NTSTATUS status=map_upper_page(state,image->VirtualBase+rva,value);
            if (!NT_SUCCESS(status)) return status;
        }
        *pte=0;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS map_kernel_shared_page(PMM_ARM64_BOOT_TABLES state, ULONG64 physical)
{
    volatile ULONG64 *identity_pte=page_entry(state,physical);
    if (!identity_pte) return STATUS_NOT_SUPPORTED;
    ULONG64 original=MmArm64QueryTranslation(physical,TRUE);
    ULONG64 mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    ULONG64 attributes=*identity_pte & PTE_ATTR_INDEX;
    if ((original&1) || (original&PA_MASK)!=physical || (original>>56)!=255 ||
        ((original>>7)&3)!=3 || ((mair >> ((attributes >> 2)*8)) & 255)!=255)
        return STATUS_NOT_SUPPORTED;

    ULONG64 security=(original & PAR_NONSECURE) ? PTE_NONSECURE : 0;
    ULONG64 descriptor=attributes|PTE_INNER_SHAREABLE|PTE_ACCESS_FLAG|
        PTE_PXN|PTE_UXN|PTE_VALID|PTE_TABLE;
    NTSTATUS status=map_upper_page(state,KI_USER_SHARED_DATA,physical|security|descriptor);
    if (!NT_SUCCESS(status)) return status;
    *identity_pte=0;
    /* Reserve empty leaf tables for both pools while building TTBR1. */
    state->NonPagedPoolPtes=upper_leaf_table(state,MM_BOOT_POOL_BASE);
    if (!state->NonPagedPoolPtes) return STATUS_INSUFFICIENT_RESOURCES;
    state->NonPagedPoolDescriptor=descriptor;
    state->PagedPoolPtes=upper_leaf_table(state,MM_BOOT_PAGED_POOL_BASE);
    if (!state->PagedPoolPtes) return STATUS_INSUFFICIENT_RESOURCES;
    state->IoPtes=upper_leaf_table(state,MM_BOOT_IO_BASE);
    if (!state->IoPtes) return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
}

static NTSTATUS map_boot_pfn_database(PXP_ARM64_BOOT_BLOCK block)
{
    PMM_ARM64_BOOT_TABLES state = &block->Tables;
    if (!block->BootPfnMetadataPages || block->BootPfnMetadataPages >= block->BootPfnPages)
        return STATUS_INVALID_PARAMETER;
    PFN_NUMBER backing_limit = block->SharedPagePfn + block->BootPfnPages;
    PFN_NUMBER backing = backing_limit - block->BootPfnMetadataPages;
    const LIST_ENTRY *head = &block->Nt.MemoryDescriptorListHead;
    const LIST_ENTRY *cursor = head->Flink;
    PFN_NUMBER first, limit = 0;
    while (MmArm64NextBootPfnPageRange(head, &cursor, &first, &limit)) {
        if (limit - first > backing_limit - backing) return STATUS_INVALID_DEVICE_STATE;
        for (PFN_NUMBER page = first; page < limit; ++page, ++backing) {
            ULONG64 physical = backing << PAGE_SHIFT;
            volatile ULONG64 *identity = page_entry(state, physical);
            if (!identity) return STATUS_NOT_SUPPORTED;
            ULONG64 par = MmArm64QueryTranslation(physical, TRUE);
            if ((par & 1) || (par & PA_MASK) != physical || (par >> 56) != 255 ||
                ((par >> 7) & 3) != 3)
                return STATUS_NOT_SUPPORTED;
            ULONG64 descriptor = physical | state->NonPagedPoolDescriptor;
            if (par & PAR_NONSECURE) descriptor |= PTE_NONSECURE;
            NTSTATUS status = map_upper_page(state,
                MM_BOOT_PFN_DATABASE_BASE + (page << PAGE_SHIFT), descriptor);
            if (!NT_SUCCESS(status)) return status;
            *identity = 0;
        }
    }
    return backing == backing_limit ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
}

static NTSTATUS map_boot_display(PXP_ARM64_BOOT_BLOCK block)
{
    XP_ARM64_BOOT_DISPLAY *display = &block->Display;
    PMM_ARM64_BOOT_TABLES tables = &block->Tables;
    if (!display->PhysicalBase) return STATUS_SUCCESS;
    ULONG64 pages = (display->Bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    /* Reserve the active mode's span only. Validate existing RAM attributes
     * before replacing any aliases in the cloned tables. */
    for (ULONG64 page = 0; page < pages; ++page) {
        ULONG64 pa = display->PhysicalBase + (page << PAGE_SHIFT);
        ULONG64 par = MmArm64QueryTranslation(pa, TRUE);
        if ((par & 1) || (par & PA_MASK) != pa || (par >> 56) != 255 || ((par >> 7) & 3) != 3)
            return STATUS_SUCCESS;
    }
    for (ULONG64 page = 0; page < pages; ++page) {
        ULONG64 offset = page << PAGE_SHIFT;
        ULONG64 pa = display->PhysicalBase + offset;
        volatile ULONG64 *pte = page_entry(tables, pa);
        if (!pte) return STATUS_INSUFFICIENT_RESOURCES;
        ULONG64 descriptor = pa | (*pte & (PTE_ATTR_INDEX | PTE_NONSECURE)) |
            PTE_INNER_SHAREABLE | PTE_ACCESS_FLAG | PTE_PXN | PTE_UXN | PTE_VALID | PTE_TABLE;
        NTSTATUS status = map_upper_page(tables, MM_BOOT_DISPLAY_BASE + offset, descriptor);
        if (!NT_SUCCESS(status)) return status;
        *pte = 0;
    }
    display->VirtualBase = MM_BOOT_DISPLAY_BASE;
    return STATUS_SUCCESS;
}

static VOID clean_tables(PMM_ARM64_BOOT_TABLES state)
{
    ULONG64 ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    ULONG_PTR line = 4ULL << ((ctr >> 16) & 15);
    ULONG_PTR end = state->PoolBase + ((ULONG64)state->UsedPages << PAGE_SHIFT);
    for (ULONG_PTR at = state->PoolBase; at < end; at += line)
        __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}
NTSTATUS MmArm64TakeBootMappings(PXP_ARM64_BOOT_BLOCK block)
{
    if (!block) return STATUS_INVALID_PARAMETER;
    PMM_ARM64_BOOT_TABLES state = &block->Tables;
    ULONG64 guard = block->StackAllocationBase, shared = block->SharedPagePfn << PAGE_SHIFT;
    const XP_ARM64_BOOT_IMAGE *image = &block->KernelImage;
    if (state->Active || state->UsedPages || !state->PoolBase ||
        !state->PoolPages || state->PoolPages > MM_BOOT_TABLE_PAGES ||
        ((state->PoolBase | guard | shared) & 4095) || !shared)
        return STATUS_INVALID_PARAMETER;
    ULONG64 sctlr, daif, level;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(state->OriginalTcr));
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(state->OriginalTtbr0));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(state->OriginalTtbr1));
    ULONG bits = 64 - (ULONG)(state->OriginalTcr & 63);
    state->OriginalSctlr = sctlr;
    if (level != 4 || !(sctlr & 1) || (sctlr & (1ULL << 25)) ||
        (daif & 0x3c0) != 0x3c0 ||
        bits < 25 || bits > 48 || (state->OriginalTcr & ((3ULL << 14) | (1ULL << 7))) ||
        ((state->OriginalTcr >> 32) & 7) > 5 ||
        (state->OriginalTtbr0 & 0xffe))
        return STATUS_NOT_SUPPORTED;
    /* The live entry/stack/PCR remain in the lower identity region while
     * the owned upper region is built offline. */
    ULONG_PTR sp, vbar;
    __asm__ volatile("mov %0, sp\nmrs %1, vbar_el1" : "=r"(sp), "=r"(vbar));
    ULONG64 limit = 1ULL << bits;
    ULONG64 pool_bytes = (ULONG64)state->PoolPages << PAGE_SHIFT;
    if (state->PoolBase >= limit || pool_bytes > limit - state->PoolBase)
        return STATUS_NOT_SUPPORTED;
    ULONG_PTR live[] = {(ULONG_PTR)MmArm64TakeBootMappings, sp, (ULONG_PTR)state,
                        vbar, vbar+2047,
                        (ULONG_PTR)MmArm64SwitchBootRoot,
                        (ULONG_PTR)MmArm64SwitchBootRootEnd - 1};
    for (ULONG i = 0; i < sizeof(live) / sizeof(live[0]); ++i)
        if (live[i] >= limit || MmArm64QueryPhysicalAddress(live[i]) != live[i])
            return STATUS_NOT_SUPPORTED;
    for (ULONG i = 0; i < state->PoolPages; ++i)
        if (!identity_page(state->PoolBase + ((ULONG64)i << PAGE_SHIFT)))
            return STATUS_NOT_SUPPORTED;
    ULONG levels = (bits - 12 + 8) / 9;
    state->StartLevel = 4 - levels;
    state->RootEntries = 1u << (bits - 12 - (levels - 1) * 9);
    ULONG64 *root = copy_table(state, state->OriginalTtbr0 & PA_MASK,
                              state->StartLevel, state->RootEntries);
    if (!root) return STATUS_INSUFFICIENT_RESOURCES;
    state->Root = (ULONG_PTR)root;
    state->GuardAddress = guard;
    state->GuardPte = page_entry(state, guard);
    if (!state->GuardPte) return STATUS_NOT_SUPPORTED;
    state->GuardDescriptor = *state->GuardPte;
    *state->GuardPte = 0;
    ULONG64 arena_bytes = (ULONG64)MM_BOOT_STACK_ARENA_PAGES << PAGE_SHIFT;
    if (!state->StackArena || (state->StackArena & 4095) ||
        state->StackArena >= limit || arena_bytes > limit - state->StackArena)
        return STATUS_INVALID_PARAMETER;
    for (ULONG slot = 0; slot < MM_BOOT_STACK_COUNT; ++slot) {
        for (ULONG page = 0; page < MM_BOOT_STACK_STRIDE; ++page) {
            ULONG64 address = state->StackArena +
                ((ULONG64)(slot * MM_BOOT_STACK_STRIDE + page) << PAGE_SHIFT);
            volatile ULONG64 *pte = page_entry(state, address);
            if (!pte) return STATUS_NOT_SUPPORTED;
            state->Stacks[slot].Ptes[page] = pte;
            ULONG64 par=MmArm64QueryTranslation(address,TRUE);
            if ((par&1) || (par&PA_MASK)!=address || (par>>56)!=255 || ((par>>7)&3)!=3)
                return STATUS_NOT_SUPPORTED;
            state->Stacks[slot].Descriptors[page] =
                (*pte & ~(PA_MASK|PTE_EL0_ACCESS|PTE_READ_ONLY|PTE_NONSECURE))|PTE_PXN|PTE_UXN;
            *pte = 0; /* No stack backing is exposed until allocated. */
        }
    }
    ULONG64 *upper=allocate_table(state,0);
    if (!upper) return STATUS_INSUFFICIENT_RESOURCES;
    state->KernelRoot=(ULONG_PTR)upper;
    NTSTATUS status=map_boot_image(state,image,limit);
    if (!NT_SUCCESS(status)) return status;
    status=map_kernel_shared_page(state,shared);
    if (!NT_SUCCESS(status)) return status;
    status=map_boot_display(block);
    if (!NT_SUCCESS(status)) return status;
    status=map_boot_pfn_database(block);
    if (!NT_SUCCESS(status)) return status;
    RtlInitializeBitMap(&state->StackBitmap, state->StackBits, MM_BOOT_STACK_COUNT);
    RtlClearAllBits(&state->StackBitmap);
    state->StacksPrepared = TRUE;
    clean_tables(state);
    /* T1SZ=16 and TG1=4KiB. Use TTBR0's ASID, enable upper walks without
     * top-byte tags, and copy IRGN/ORGN/SH from the existing lower walk. */
    ULONG64 walk_attributes=(state->OriginalTcr >> 8) & 63;
    state->ActiveTcr=(state->OriginalTcr & ~(TCR_TTBR1_FIELDS | TCR_TBI1)) |
        (16ULL << 16) | (walk_attributes << 24) | (2ULL << 30);
    /* Even though the new tree was built offline, block-size changes must
     * not coexist with cached old mappings. Pause translation in the bounded
     * identity assembly interval and finish TLBI before restoring SCTLR.M. */
    state->RootSwitchAddress=(ULONG_PTR)MmArm64SwitchBootRoot;
    state->RootSwitchSize=(ULONG_PTR)MmArm64SwitchBootRootEnd-state->RootSwitchAddress;
    state->SwitchSctlr = MmArm64SwitchBootRoot(state->Root,state->ActiveTcr,state->KernelRoot);
    state->Active = TRUE;
    return STATUS_SUCCESS;
}
