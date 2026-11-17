#include <ntos.h>
#include "userspace.h"
#include "bootframes_private.h"
#include "bootpool.h"

#define ADDRESS_MASK 0x0000fffffffff000ULL
#define USER_ACCESS (1ULL << 6)
#define READ_ONLY (1ULL << 7)
#define NON_GLOBAL (1ULL << 11)
#define USER_XN (1ULL << 54)
#define USER_LIMIT 0x80000000ULL

PMMPTE MmSharedUserDataPte;

BOOLEAN MmArm64InitializeSharedUserPte(VOID)
{
    if (!KeLoaderBlock || MmSharedUserDataPte || KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;
    PXP_ARM64_BOOT_BLOCK boot = CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt);
    ULONG64 par = MmArm64QueryTranslation(KI_USER_SHARED_DATA, TRUE);
    if ((par & 1) || (par & ADDRESS_MASK) != (boot->SharedPagePfn << PAGE_SHIFT) ||
        (par >> 56) != 255 || ((par >> 7) & 3) != 3) return FALSE;
    PMMPTE pte = ExAllocatePoolWithTag(PagedPool, sizeof(*pte), '  mM');
    if (!pte) return FALSE;
    pte->u.Long = (par & ADDRESS_MASK) | boot->Tables.NonPagedPoolDescriptor |
                  ((par & (1ULL << 9)) >> 4) | USER_ACCESS | READ_ONLY | NON_GLOBAL | USER_XN;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    MI_PFN_ELEMENT(boot->SharedPagePfn)->OriginalPte.u.Long = MM_ARM64_KERNEL_DEMAND_ZERO_PTE;
    MmSharedUserDataPte = pte;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return TRUE;
}

static BOOLEAN boot_context(PXP_ARM64_BOOT_BLOCK boot)
{
    ULONG64 daif, root;
    __asm__ volatile("mrs %0, daif\nmrs %1, ttbr0_el1" : "=r"(daif), "=r"(root));
    return boot && boot->Tables.Active && KeNumberProcessors == 1 &&
           (daif & 0x3c0) == 0x3c0 && (root & ADDRESS_MASK) == boot->Tables.Root;
}

static NTSTATUS allocate_page(MM_ARM64_USER_SPACE *space, PULONG64 physical)
{
    PFN_NUMBER frame;
    NTSTATUS status = MmArm64TakeBootFrame(space, &frame);
    if (NT_SUCCESS(status)) {
        *physical = frame << PAGE_SHIFT;
        RtlZeroMemory((PVOID)*physical, PAGE_SIZE);
    }
    return status;
}

static VOID release_page(MM_ARM64_USER_SPACE *space, ULONG64 physical)
{
    NTSTATUS status = MmArm64ReturnBootFrame(space, physical >> PAGE_SHIFT);
    if (!NT_SUCCESS(status))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, physical, (ULONG_PTR)space, status);
}

static ULONG slot(ULONG_PTR address, ULONG level)
{
    return (address >> (PAGE_SHIFT + (3 - level) * 9)) & 511;
}

/* Allocate a missing subtree privately. Publish each parent only after all
 * descendants exist, so resource failure leaves the old tree intact. */
static NTSTATUS leaf(MM_ARM64_USER_SPACE *space, ULONG64 table_pa, ULONG level,
                      ULONG_PTR address, BOOLEAN create, PULONG64 *result)
{
    PULONG64 entry = (PULONG64)table_pa + slot(address, level);
    if (level == 3) {
        *result = entry;
        return STATUS_SUCCESS;
    }
    if (*entry)
        return leaf(space, *entry & ADDRESS_MASK, level + 1, address, create, result);
    if (!create) return STATUS_NOT_MAPPED_VIEW;
    ULONG64 child;
    NTSTATUS status = allocate_page(space, &child);
    if (!NT_SUCCESS(status)) return status;
    status = leaf(space, child, level + 1, address, TRUE, result);
    if (NT_SUCCESS(status)) *entry = child | 3;
    else release_page(space, child);
    return status;
}

static NTSTATUS lookup(MM_ARM64_USER_SPACE *space, ULONG_PTR address, PULONG64 *entry)
{
    NTSTATUS status = leaf(space, space->Root, space->Boot->Tables.StartLevel, address, FALSE, entry);
    return NT_SUCCESS(status) && **entry ? STATUS_SUCCESS : STATUS_NOT_MAPPED_VIEW;
}

static BOOLEAN valid_range(ULONG_PTR address, SIZE_T size)
{
    return address >= 0x10000 && address < USER_LIMIT && size && size <= USER_LIMIT - address;
}

static BOOLEAN mutable_range(ULONG_PTR address, SIZE_T size)
{
    return valid_range(address, size) && !((address | size) & (PAGE_SIZE - 1)) &&
        (address + size <= MM_ARM64_USER_SHARED_VA || address >= MM_ARM64_USER_SHARED_VA + PAGE_SIZE);
}

static BOOLEAN valid_protection(ULONG protection)
{
    return protection == PAGE_READONLY || protection == PAGE_READWRITE ||
           protection == PAGE_EXECUTE_READ;
}

static VOID sync_code(ULONG64 physical)
{
    ULONG64 ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    ULONG line = 4u << ((ctr >> 16) & 15);
    for (ULONG i = 0; i < PAGE_SIZE; i += line)
        __asm__ volatile("dc cvau, %0" :: "r"(physical+i) : "memory");
    /* Invalidate every I-cache alias on the sole CPU. */
    __asm__ volatile("dsb ish\nic iallu\ndsb ish\nisb" ::: "memory");
}

static ULONG64 descriptor(MM_ARM64_USER_SPACE *space, ULONG64 pa, ULONG64 par, ULONG protection)
{
    ULONG64 value = (space->Boot->Tables.NonPagedPoolDescriptor & ~USER_XN) |
        pa | ((par & (1ULL << 9)) >> 4) | USER_ACCESS | NON_GLOBAL;
    if (protection != PAGE_READWRITE) value |= READ_ONLY;
    if (protection != PAGE_EXECUTE_READ) value |= USER_XN;
    return value;
}

NTSTATUS MmArm64CreateUserSpace(MM_ARM64_USER_SPACE *space, PXP_ARM64_BOOT_BLOCK boot)
{
    if (!space || space->Root || !boot_context(boot)) return STATUS_INVALID_DEVICE_STATE;
    if ((64 - (boot->Tables.ActiveTcr & 63)) < 31) return STATUS_NOT_SUPPORTED;
    ULONG64 par = MmArm64QueryTranslation(KI_USER_SHARED_DATA, TRUE);
    ULONG64 shared = boot->SharedPagePfn << PAGE_SHIFT;
    if ((par & 1) || (par & ADDRESS_MASK) != shared) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = allocate_page(space, &space->Root);
    if (!NT_SUCCESS(status)) return status;
    space->Boot = boot;
    PULONG64 entry;
    status = leaf(space, space->Root, boot->Tables.StartLevel, MM_ARM64_USER_SHARED_VA, TRUE, &entry);
    if (!NT_SUCCESS(status)) {
        release_page(space, space->Root);
        RtlZeroMemory(space, sizeof(*space));
        return status;
    }
    *entry = MmSharedUserDataPte ? MmSharedUserDataPte->u.Long : descriptor(space, shared, par, PAGE_READONLY);
    return STATUS_SUCCESS;
}

/* Return whether this table became empty. Walk only branches in [start,end). */
static BOOLEAN remove_range(MM_ARM64_USER_SPACE *space, ULONG64 table_pa, ULONG level,
                            ULONG_PTR base, ULONG_PTR start, ULONG_PTR end)
{
    PULONG64 table = (PULONG64)table_pa;
    ULONG_PTR span = 1ULL << (PAGE_SHIFT + (3 - level) * 9);
    BOOLEAN empty = TRUE;
    for (ULONG i = 0; i < 512; ++i) {
        if (!table[i]) continue;
        ULONG_PTR va = base + i * span;
        if (start < va + span && va < end) {
            ULONG64 pa = table[i] & ADDRESS_MASK;
            if (level == 3 || remove_range(space, pa, level+1, va, start, end)) {
                table[i] = 0;
                if (level != 3 || va != MM_ARM64_USER_SHARED_VA) release_page(space, pa);
            }
        }
        if (table[i]) empty = FALSE;
    }
    return empty;
}

VOID MmArm64DestroyUserSpace(MM_ARM64_USER_SPACE *space)
{
    if (!space || !space->Root) return;
    if (!boot_context(space->Boot))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0x37, (ULONG_PTR)space, space->Root, 0);
    remove_range(space, space->Root, space->Boot->Tables.StartLevel, 0, 0, USER_LIMIT);
    release_page(space, space->Root);
    RtlZeroMemory(space, sizeof(*space));
}

NTSTATUS MmArm64MapUserPages(MM_ARM64_USER_SPACE *space, ULONG_PTR base, SIZE_T bytes, ULONG protection)
{
    if (!space || !space->Root || !boot_context(space->Boot)) return STATUS_INVALID_DEVICE_STATE;
    if (!mutable_range(base, bytes) || !valid_protection(protection)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR va;
    for (va = base; va < base + bytes; va += PAGE_SIZE) {
        PULONG64 entry;
        if (NT_SUCCESS(lookup(space, va, &entry))) {
            status = STATUS_CONFLICTING_ADDRESSES;
            break;
        }
        ULONG64 pa;
        status = allocate_page(space, &pa);
        if (!NT_SUCCESS(status)) break;
        status = leaf(space, space->Root, space->Boot->Tables.StartLevel, va, TRUE, &entry);
        if (!NT_SUCCESS(status)) {
            release_page(space, pa);
            break;
        }
        if (protection == PAGE_EXECUTE_READ) sync_code(pa);
        *entry = descriptor(space, pa, MmArm64QueryTranslation(pa, TRUE), protection);
        MmArm64RecordBootMapping(space, pa >> PAGE_SHIFT, entry, FALSE);
    }
    if (!NT_SUCCESS(status) && va != base)
        remove_range(space, space->Root, space->Boot->Tables.StartLevel, 0, base, va);
    return status;
}

static NTSTATUS check_pages(MM_ARM64_USER_SPACE *space, ULONG_PTR base, SIZE_T bytes, BOOLEAN write)
{
    if (!space || !space->Root || !boot_context(space->Boot)) return STATUS_INVALID_DEVICE_STATE;
    if (!valid_range(base, bytes)) return STATUS_INVALID_PARAMETER;
    for (ULONG_PTR va = base & ~(ULONG_PTR)(PAGE_SIZE-1); va < base + bytes; va += PAGE_SIZE) {
        PULONG64 entry;
        NTSTATUS status = lookup(space, va, &entry);
        if (!NT_SUCCESS(status)) return status;
        if (write && (*entry & READ_ONLY)) return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS MmArm64ProtectUserPages(MM_ARM64_USER_SPACE *space, ULONG_PTR base, SIZE_T bytes, ULONG protection)
{
    if (!mutable_range(base, bytes) || !valid_protection(protection)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = check_pages(space, base, bytes, FALSE);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG_PTR va = base; va < base + bytes; va += PAGE_SIZE) {
        PULONG64 entry;
        lookup(space, va, &entry);
        ULONG64 flags = *entry & ~(READ_ONLY | USER_XN);
        if (protection == PAGE_EXECUTE_READ) sync_code(flags & ADDRESS_MASK);
        else flags |= USER_XN;
        if (protection != PAGE_READWRITE) flags |= READ_ONLY;
        *entry = flags;
    }
    return STATUS_SUCCESS;
}

NTSTATUS MmArm64UnmapUserPages(MM_ARM64_USER_SPACE *space, ULONG_PTR base, SIZE_T bytes)
{
    if (!mutable_range(base, bytes)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = check_pages(space, base, bytes, FALSE);
    if (!NT_SUCCESS(status)) return status;
    remove_range(space, space->Root, space->Boot->Tables.StartLevel, 0, base, base + bytes);
    return STATUS_SUCCESS;
}

NTSTATUS MmArm64CopyUserBuffer(MM_ARM64_USER_SPACE *space, ULONG_PTR address,
                               PVOID buffer, SIZE_T bytes, BOOLEAN to_user)
{
    if (!buffer) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = check_pages(space, address, bytes, to_user);
    if (!NT_SUCCESS(status)) return status;
    PUCHAR cursor = buffer;
    while (bytes) {
        PULONG64 entry;
        lookup(space, address, &entry);
        ULONG offset = address & (PAGE_SIZE-1);
        SIZE_T count = min(bytes, PAGE_SIZE - offset);
        PUCHAR alias = (PUCHAR)(*entry & ADDRESS_MASK);
        if ((address & ~(ULONG_PTR)(PAGE_SIZE-1)) == MM_ARM64_USER_SHARED_VA)
            alias = (PUCHAR)KI_USER_SHARED_DATA;
        if (to_user) RtlCopyMemory(alias + offset, cursor, count);
        else RtlCopyMemory(cursor, alias + offset, count);
        address += count;
        cursor += count;
        bytes -= count;
    }
    return STATUS_SUCCESS;
}
