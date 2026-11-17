/* Resident I/O system PTEs. The whitelist is established by the boot HAL
 * from ACPI and verified against the actual Device translation. */
#include "bootio_private.h"

#define PA_MASK 0x0000fffffffff000ULL
#define DAIF_MASK 0x3c0ULL

typedef struct _BOOT_IO_DEVICE {
    ULONG64 Physical;
    ULONG64 Descriptor;
} BOOT_IO_DEVICE;

typedef struct _BOOT_IO_MAPPING {
    PTE_TRACKER Tracker;
    BOOLEAN Pinned;
} BOOT_IO_MAPPING;

static volatile ULONG64 *io_ptes;
static BOOT_IO_DEVICE devices[2];
static BOOT_IO_MAPPING mappings[MM_BOOT_IO_PAGES];

static ULONG64 mask_exceptions(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    return daif;
}
static VOID restore_exceptions(ULONG64 daif)
{
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}

static ULONG64 lock_io(VOID)
{
    ULONG64 daif = mask_exceptions();
    KSPIN_LOCK expected = 0;
    if (!__atomic_compare_exchange_n(&MiPteTrackerLock, &expected, 1, FALSE,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        KeBugCheckEx(SPIN_LOCK_ALREADY_OWNED, (ULONG_PTR)&MiPteTrackerLock, expected, 0, 0);
    return daif;
}

static VOID unlock_io(ULONG64 daif)
{
    __atomic_store_n(&MiPteTrackerLock, 0, __ATOMIC_RELEASE);
    restore_exceptions(daif);
}

PVOID MmArm64IoPte(PVOID address)
{
    return (PVOID)&io_ptes[((ULONG_PTR)address - MM_BOOT_IO_BASE) >> PAGE_SHIFT];
}
PVOID MmArm64IoVa(PVOID pte)
{
    return MmArm64SystemPteVa(pte);
}

NTSTATUS HalArm64QueryBootPlatformInformation(HAL_QUERY_INFORMATION_CLASS information_class,
                                               ULONG length, PVOID output, PULONG returned)
{
    if (information_class != HalPlatformInformation) return STATUS_INVALID_INFO_CLASS;
    if (!returned) return STATUS_INVALID_PARAMETER;
    *returned = sizeof(HAL_PLATFORM_INFORMATION);
    if (!output || length < *returned) return STATUS_BUFFER_TOO_SMALL;

    /* The current HAL retains a WB direct view of installed RAM. Its
     * mapping policy consequently forbids an uncached RAM alias. */
    ULONG64 par = MmArm64QueryTranslation(KI_USER_SHARED_DATA, TRUE);
    if ((par & 1) || (par >> 56) != 255 || ((par >> 7) & 3) != 3)
        return STATUS_INVALID_DEVICE_STATE;
    ((PHAL_PLATFORM_INFORMATION)output)->PlatformFlags = HAL_PLATFORM_DISABLE_UC_MAIN_MEMORY;
    return STATUS_SUCCESS;
}

static ULONG64 device_descriptor(ULONG64 physical)
{
    if (!physical || (physical & ~PA_MASK)) return 0;
    ULONG64 par = MmArm64QueryTranslation(physical, TRUE);
    ULONG64 attr = par >> 56;
    if ((par & 1) || (par & PA_MASK) != physical || (attr != 0 && attr != 4))
        return 0;
    ULONG64 mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    for (ULONG i = 0; i < 8; ++i) {
        if (((mair >> (i * 8)) & 255) != attr) continue;
        ULONG64 shareability = (par & (3ULL << 7)) << 1;
        ULONG64 nonsecure = (par & (1ULL << 9)) >> 4;
        return physical | (i << 2) | shareability | nonsecure | (1ULL << 10) |
            (1ULL << 53) | (1ULL << 54) | 3;
    }
    return 0;
}

static ULONG64 lookup_device(ULONG64 physical)
{
    for (ULONG i = 0; i < sizeof(devices) / sizeof(*devices); ++i)
        if (devices[i].Physical == physical) return devices[i].Descriptor;
    return 0;
}

NTSTATUS MmArm64InitializeBootIo(PXP_ARM64_BOOT_BLOCK block)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (!block || io_ptes || !block->Tables.Active || !block->Tables.IoPtes ||
        KeNumberProcessors != 1 || KeGetCurrentIrql() != HIGH_LEVEL ||
        (daif & DAIF_MASK) != DAIF_MASK)
        return STATUS_INVALID_DEVICE_STATE;
    ULONG64 distributor = block->Platform.GicDistributor;
    ULONG64 cpu = block->Platform.GicCpuInterface;
    ULONG64 distributor_pte = device_descriptor(distributor), cpu_pte = device_descriptor(cpu);
    if (!distributor_pte || !cpu_pte || distributor == cpu) return STATUS_NOT_SUPPORTED;
    for (ULONG i = 0; i < MM_BOOT_IO_PAGES; ++i)
        if (block->Tables.IoPtes[i]) return STATUS_CONFLICTING_ADDRESSES;
    MiInitializeCacheOverrides();
    if (MiPlatformCacheAttributes[MmNonCached] != MiCached ||
        MiPlatformCacheAttributes[MmMaximumCacheType + MmNonCached] != MiNonCached)
        return STATUS_INVALID_DEVICE_STATE;
    MmTrackPtes = 1;
    MiInitializeIoTrackers();
    devices[0] = (BOOT_IO_DEVICE){distributor, distributor_pte};
    devices[1] = (BOOT_IO_DEVICE){cpu, cpu_pte};
    io_ptes = block->Tables.IoPtes;
    MiInitializeSystemPtes((PMMPTE)io_ptes, MM_BOOT_IO_PAGES, SystemPteSpace);
    return STATUS_SUCCESS;
}

static PVOID map_io(ULONG64 address, SIZE_T bytes, MEMORY_CACHING_TYPE cache,
                    BOOLEAN pinned, PVOID caller)
{
    ULONG offset = (ULONG)address & (PAGE_SIZE - 1);
    ULONG64 physical = address - offset;
    if (!io_ptes || KeNumberProcessors != 1 || !bytes ||
        bytes > MM_BOOT_IO_PAGES * PAGE_SIZE - offset || (physical & ~PA_MASK) ||
        cache != MmNonCached ||
        MiPlatformCacheAttributes[MmMaximumCacheType + cache] != MiNonCached)
        return NULL;
    ULONG pages = (ULONG)((offset + bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
    for (ULONG i = 0; i < pages; ++i)
        if (!lookup_device(physical + i * PAGE_SIZE)) return NULL;

    ULONG64 daif = lock_io();
    PMMPTE start = MiReserveSystemPtes(pages, SystemPteSpace);
    if (!start) {
        unlock_io(daif);
        return NULL;
    }
    ULONG first = (ULONG)(start - MmSystemPteBase);
    for (ULONG i = 0; i < pages; ++i)
        io_ptes[first + i] = lookup_device(physical + i * PAGE_SIZE);
    MmArm64SyncSystemPtes(start, pages);
    PVOID va = (PUCHAR)MmArm64SystemPteVa(start) + offset;

    /* The original MmMapIoSpace presents a temporary MDL to its tracker.
     * The saved tracker has no persistent MDL for an I/O space mapping. */
    struct {
        MDL Mdl;
        PFN_NUMBER Page;
    } request = {0};
    request.Mdl.MappedSystemVa = va;
    request.Mdl.StartVa = (PVOID)(ULONG_PTR)physical;
    request.Mdl.ByteOffset = offset;
    request.Mdl.ByteCount = (ULONG)bytes;
    request.Page = physical >> PAGE_SHIFT;
    BOOT_IO_MAPPING *mapping = &mappings[first];
    MiInsertPteTracker(&mapping->Tracker, &request.Mdl, pages, caller, NULL);
    mapping->Tracker.Mdl = NULL;
    mapping->Pinned = pinned;
    unlock_io(daif);
    return va;
}

static NTSTATUS unmap_io(PVOID address, SIZE_T bytes, BOOLEAN allow_pinned)
{
    ULONG_PTR offset = (ULONG_PTR)address - MM_BOOT_IO_BASE;
    if (!io_ptes || offset >= MM_BOOT_IO_PAGES * PAGE_SIZE || !bytes)
        return STATUS_INVALID_PARAMETER;
    ULONG first = (ULONG)(offset >> PAGE_SHIFT);
    ULONG64 daif = lock_io();
    BOOT_IO_MAPPING *mapping = &mappings[first];
    if (mapping->Tracker.SystemVa != address || mapping->Tracker.Length != bytes ||
        (mapping->Pinned && !allow_pinned)) {
        unlock_io(daif);
        return STATUS_INVALID_HANDLE;
    }
    ULONG pages = (ULONG)mapping->Tracker.Count;
    PMMPTE start = MmSystemPteBase + first;
    MiArm64RemoveIoPteTracker(start, pages);
    MiReleaseSystemPtes(start, pages, SystemPteSpace);
    RtlZeroMemory(mapping, sizeof(*mapping));
    unlock_io(daif);
    return STATUS_SUCCESS;
}

PVOID MmMapIoSpace(PHYSICAL_ADDRESS physical, SIZE_T bytes, MEMORY_CACHING_TYPE cache)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) return NULL;
    return map_io(physical.QuadPart, bytes, cache & 0xff, FALSE, _ReturnAddress());
}

NTSTATUS MmArm64TryUnmapIoSpace(PVOID address, SIZE_T bytes)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL || KeNumberProcessors != 1)
        return STATUS_INVALID_DEVICE_STATE;
    return unmap_io(address, bytes, FALSE);
}

VOID MmUnmapIoSpace(PVOID address, SIZE_T bytes)
{
    NTSTATUS status = MmArm64TryUnmapIoSpace(address, bytes);
    if (!NT_SUCCESS(status))
        KeBugCheckEx(SYSTEM_PTE_MISUSE, 6, (ULONG_PTR)address, bytes, status);
}

NTSTATUS MmArm64MapBootGic(const HAL_ARM64_BOOT_PLATFORM *physical,
                           HAL_ARM64_BOOT_PLATFORM *mapped)
{
    if (!physical || !mapped || KeGetCurrentIrql() != HIGH_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    PVOID distributor = map_io(physical->GicDistributor, PAGE_SIZE, MmNonCached, TRUE,
                               _ReturnAddress());
    if (!distributor) return STATUS_INSUFFICIENT_RESOURCES;
    PVOID cpu = map_io(physical->GicCpuInterface, PAGE_SIZE, MmNonCached, TRUE, _ReturnAddress());
    if (!cpu) {
        unmap_io(distributor, PAGE_SIZE, TRUE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *mapped = *physical;
    mapped->GicDistributor = (ULONG_PTR)distributor;
    mapped->GicCpuInterface = (ULONG_PTR)cpu;
    return STATUS_SUCCESS;
}

VOID MmArm64UnmapBootGic(const HAL_ARM64_BOOT_PLATFORM *mapped)
{
    if (!NT_SUCCESS(unmap_io((PVOID)(ULONG_PTR)mapped->GicCpuInterface, PAGE_SIZE, TRUE)) ||
        !NT_SUCCESS(unmap_io((PVOID)(ULONG_PTR)mapped->GicDistributor, PAGE_SIZE, TRUE)))
        KeBugCheckEx(SYSTEM_PTE_MISUSE, 6, mapped->GicDistributor, mapped->GicCpuInterface, 0);
}
