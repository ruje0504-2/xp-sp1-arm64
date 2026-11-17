/* Install a self-contained native PE at its preferred low VA before the
 * first thread is published. File is a stable, resident kernel buffer. */
#include "../../../boot/arm64/pevalidate.h"
#include <ntverp.h>

static NTSTATUS map_image_bytes(MM_ARM64_USER_SPACE *space, ULONG_PTR address,
                                const UCHAR *source, ULONG bytes, ULONG size, ULONG protection)
{
    ULONG mapped = 0;
    NTSTATUS status = STATUS_SUCCESS;
    while (mapped < size) {
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        ULONG_PTR page = address + mapped;
        status = MmArm64MapUserPages(space, page, PAGE_SIZE, PAGE_READWRITE);
        if (NT_SUCCESS(status)) {
            ULONG copy = mapped < bytes ? min(bytes - mapped, PAGE_SIZE) : 0;
            if (copy) status = MmArm64CopyUserBuffer(space, page, (PVOID)(source + mapped), copy, TRUE);
            mapped += PAGE_SIZE;
            if (NT_SUCCESS(status) && protection != PAGE_READWRITE)
                status = MmArm64ProtectUserPages(space, page, PAGE_SIZE, protection);
        }
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        if (!NT_SUCCESS(status)) break;
    }
    if (!NT_SUCCESS(status) && mapped) {
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        if (!NT_SUCCESS(MmArm64UnmapUserPages(space, address, mapped)))
            KeBugCheckEx(MEMORY_MANAGEMENT, 0xa644, address, mapped, 0);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    }
    return status;
}

static NTSTATUS install_user_image(MM_ARM64_USER_SPACE *space, const UCHAR *file,
                                   const IMAGE_NT_HEADERS64 *nt, const XP_ARM64_BOOT_IMAGE *image,
                                   ULONG_PTR peb_address, PPEB peb)
{
    ULONG_PTR base = image->PreferredBase;
    NTSTATUS status = map_image_bytes(space, base, file, image->HeaderSize, image->HeaderSize, PAGE_READONLY);
    if (!NT_SUCCESS(status)) return status;
    const IMAGE_SECTION_HEADER *sections = (const VOID *)(nt + 1);
    ULONG loaded = 0;
    for (; loaded < image->SectionCount; ++loaded) {
        const XP_ARM64_IMAGE_SECTION *s = &image->Sections[loaded];
        ULONG protection = s->Characteristics & IMAGE_SCN_MEM_WRITE ? PAGE_READWRITE :
                           s->Characteristics & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ : PAGE_READONLY;
        ULONG bytes = min(s->Size, sections[loaded].SizeOfRawData);
        status = map_image_bytes(space, base + s->Rva, bytes ? file + sections[loaded].PointerToRawData : NULL,
                                 bytes, s->Size, protection);
        if (!NT_SUCCESS(status)) break;
    }
    if (NT_SUCCESS(status))
        status = map_image_bytes(space, peb_address, (const UCHAR *)peb, sizeof(*peb), PAGE_SIZE, PAGE_READWRITE);
    if (NT_SUCCESS(status)) return status;

    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    while (loaded) {
        const XP_ARM64_IMAGE_SECTION *s = &image->Sections[--loaded];
        if (!NT_SUCCESS(MmArm64UnmapUserPages(space, base + s->Rva, rounded_page(s->Size))))
            KeBugCheckEx(MEMORY_MANAGEMENT, 0xa644, base, s->Rva, 0);
    }
    if (!NT_SUCCESS(MmArm64UnmapUserPages(space, base, rounded_page(image->HeaderSize))))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0xa644, base, image->HeaderSize, 0);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return status;
}

NTSTATUS PsArm64InitializeProcessImage(PEPROCESS process, const VOID *file, SIZE_T size,
                                       ULONG_PTR peb_address, PULONG_PTR entry)
{
    if (!entry) return STATUS_INVALID_PARAMETER;
    *entry = 0;
    if (!process || !file || !peb_address || BYTE_OFFSET(peb_address) || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(process);
    if (!space) return STATUS_INVALID_DEVICE_STATE;
    XP_ARM64_BOOT_IMAGE image = {0};
    const IMAGE_NT_HEADERS64 *nt = validate_headers(file, size, &image, TRUE);
    if (!nt || !within(image.PreferredBase, image.Size, 0x80000000ULL)) return STATUS_INVALID_IMAGE_FORMAT;
    if (peb_address >= image.PreferredBase && peb_address < image.PreferredBase + image.Size)
        return STATUS_CONFLICTING_ADDRESSES;
    C_ASSERT(sizeof(PEB) < PAGE_SIZE);
    PPEB PebBase = ExAllocatePoolWithTag(NonPagedPool, sizeof(PEB), 'bPsP');
    if (!PebBase) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(PebBase, sizeof(*PebBase));
    PebBase->Mutant = (HANDLE)-1;
    PebBase->ImageBaseAddress = (PVOID)image.PreferredBase;
    PebBase->NumberOfProcessors = KeNumberProcessors;
    PebBase->BeingDebugged = process->DebugPort != NULL;
    PebBase->NtGlobalFlag = NtGlobalFlag;
    PebBase->CriticalSectionTimeout.QuadPart = (LONGLONG)MmCritsectTimeoutSeconds * -10000000;
    PebBase->HeapSegmentReserve = MmHeapSegmentReserve;
    PebBase->HeapSegmentCommit = MmHeapSegmentCommit;
    PebBase->HeapDeCommitTotalFreeThreshold = MmHeapDeCommitTotalFreeThreshold;
    PebBase->HeapDeCommitFreeBlockThreshold = MmHeapDeCommitFreeBlockThreshold;
    PebBase->MaximumNumberOfHeaps = (PAGE_SIZE - sizeof(PEB)) / sizeof(PVOID);
    PebBase->ProcessHeaps = (PVOID *)(peb_address + sizeof(PEB));
    PebBase->OSMajorVersion = VER_PRODUCTMAJORVERSION;
    PebBase->OSMinorVersion = VER_PRODUCTMINORVERSION;
    PebBase->OSBuildNumber = VER_PRODUCTBUILD;
    PebBase->OSPlatformId = 2;
    PIMAGE_LOAD_CONFIG_DIRECTORY config = NULL; /* Unsupported directories fail PE validation. */
    MI_INIT_PEB_FROM_IMAGE(nt, config);
    PETHREAD current = PsGetCurrentThread();
    PspLockProcessExclusive(process, current);
    NTSTATUS status;
    if (process->Flags & PS_PROCESS_FLAGS_PROCESS_DELETE) status = STATUS_PROCESS_IS_TERMINATING;
    else if (process->Peb || process->ActiveThreads) status = STATUS_INVALID_DEVICE_STATE;
    else {
        status = install_user_image(space, file, nt, &image, peb_address, PebBase);
        if (NT_SUCCESS(status)) {
            process->SectionBaseAddress = (PVOID)image.PreferredBase;
            process->Peb = (PPEB)peb_address;
            *entry = image.PreferredBase + image.EntryRva;
        }
    }
    PspUnlockProcessExclusive(process, current);
    ExFreePool(PebBase);
    return status;
}
