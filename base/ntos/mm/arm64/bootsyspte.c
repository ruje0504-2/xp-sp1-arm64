/* ARM64 system PTE hardware and ownership around the original XP free list. */
#include "bootsyspte.h"

static KSPIN_LOCK system_pte_lock;
static USHORT reservations[MM_BOOT_IO_PAGES];

static BOOLEAN allowed(VOID)
{
    if (KeNumberProcessors != 1) return FALSE;
    KIRQL irql = KeGetCurrentIrql();
    if (irql <= DISPATCH_LEVEL) return TRUE;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return irql == HIGH_LEVEL && (daif & 0x3c0) == 0x3c0;
}

static ULONG64 lock_system_ptes(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    KSPIN_LOCK expected = 0;
    if (!__atomic_compare_exchange_n(&system_pte_lock, &expected, 1, FALSE,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        KeBugCheckEx(SPIN_LOCK_ALREADY_OWNED, (ULONG_PTR)&system_pte_lock, expected, 0, 0);
    return daif;
}

static VOID unlock_system_ptes(ULONG64 daif)
{
    __atomic_store_n(&system_pte_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}

PVOID MmArm64SystemPteVa(PMMPTE pte)
{
    return (PVOID)(MM_BOOT_IO_BASE + ((ULONG_PTR)(pte - MmSystemPteBase) << PAGE_SHIFT));
}

VOID MmArm64SyncSystemPtes(PMMPTE start, ULONG pages)
{
    for (ULONG i = 0; i < pages; ++i)
        __asm__ volatile("dc cvac, %0" :: "r"(start + i) : "memory");
    __asm__ volatile("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
}

VOID MiInitializeSystemPtes(PMMPTE start, ULONG pages, MMSYSTEM_PTE_POOL_TYPE pool)
{
    if (!allowed() || MmSystemPteBase || !start || ((ULONG_PTR)start & (PAGE_SIZE - 1)) ||
        !pages || pages > MM_BOOT_IO_PAGES || pool != SystemPteSpace)
        KeBugCheckEx(SYSTEM_PTE_MISUSE, 0, (ULONG_PTR)start, pages, pool);
    ULONG64 daif = lock_system_ptes();
    MiArm64InitializeSystemPteList(start, pages, pool);
    MmArm64SyncSystemPtes(start, pages);
    unlock_system_ptes(daif);
}

PMMPTE MiReserveAlignedSystemPtes(ULONG pages, MMSYSTEM_PTE_POOL_TYPE pool, ULONG alignment)
{
    if (!MmSystemPteBase || !allowed() || !pages || pages > MmTotalSystemPtes ||
        pool != SystemPteSpace || alignment > MM_BOOT_IO_PAGES * PAGE_SIZE ||
        (alignment && (alignment & (alignment - 1))))
        return NULL;
    ULONG64 daif = lock_system_ptes();
    PMMPTE start = MiArm64FindSystemPtes(pages, pool, alignment);
    if (start) {
        ULONG index = (ULONG)(start - MmSystemPteBase);
        if (start < MmSystemPtesStart[pool] || start > MmSystemPtesEnd[pool] ||
            pages > (ULONG)(MmSystemPtesEnd[pool] - start + 1) || reservations[index])
            KeBugCheckEx(SYSTEM_PTE_MISUSE, 1, (ULONG_PTR)start, pages, index);
        /* The former free-list headers are invalid hardware descriptors.
         * Clear them before handing the reserved PTEs to the caller. */
        RtlZeroMemory(start, pages * sizeof(*start));
        reservations[index] = (USHORT)pages;
    }
    unlock_system_ptes(daif);
    return start;
}

PMMPTE MiReserveSystemPtes(ULONG pages, MMSYSTEM_PTE_POOL_TYPE pool)
{
    return MiReserveAlignedSystemPtes(pages, pool, 0);
}

NTSTATUS MmArm64TryReleaseSystemPtes(PMMPTE start, ULONG pages, MMSYSTEM_PTE_POOL_TYPE pool)
{
    if (!MmSystemPteBase || !allowed() || pool != SystemPteSpace)
        return STATUS_INVALID_DEVICE_STATE;
    ULONG_PTR offset = (ULONG_PTR)start - (ULONG_PTR)MmSystemPteBase;
    if (!pages || offset >= MmTotalSystemPtes * sizeof(*start) || offset % sizeof(*start))
        return STATUS_INVALID_PARAMETER;
    ULONG index = (ULONG)(offset / sizeof(*start));
    ULONG64 daif = lock_system_ptes();
    if (reservations[index] != pages) {
        unlock_system_ptes(daif);
        return STATUS_INVALID_HANDLE;
    }
    RtlZeroMemory(start, pages * sizeof(*start));
    /* Retire active hardware entries before writing invalid list metadata
     * into the same PTEs or exposing the range for another owner. */
    MmArm64SyncSystemPtes(start, pages);
    MiArm64CoalesceSystemPtes(start, pages, pool);
    reservations[index] = 0;
    unlock_system_ptes(daif);
    return STATUS_SUCCESS;
}

VOID MiReleaseSystemPtes(PMMPTE start, ULONG pages, MMSYSTEM_PTE_POOL_TYPE pool)
{
    NTSTATUS status = MmArm64TryReleaseSystemPtes(start, pages, pool);
    if (!NT_SUCCESS(status))
        KeBugCheckEx(SYSTEM_PTE_MISUSE, 2, (ULONG_PTR)start, pages, status);
}
