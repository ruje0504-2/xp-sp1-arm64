#include "pe.h"
#include "../../ntos/mm/arm64/bootframes.h"

typedef VOID (*XP_ARM64_KERNEL_ENTRY)(PXP_ARM64_BOOT_BLOCK, ULONG_PTR);
static CHAR load_options[] = XP_ARM64_NATIVE_LOAD_OPTIONS;

static VOID publish_kernel_module(PXP_ARM64_BOOT_BLOCK block)
{
    const XP_ARM64_BOOT_IMAGE *image=&block->KernelImage;
    PKLDR_DATA_TABLE_ENTRY module=&block->KernelModule;
    module->DllBase=(PVOID)(ULONG_PTR)image->VirtualBase;
    module->EntryPoint=(PVOID)(ULONG_PTR)(image->VirtualBase+image->EntryRva);
    module->SizeOfImage=image->Size;
    module->CheckSum=image->CheckSum;
    PWCHAR base_name = BlArm64KernelFilePath;
    PWCHAR end = base_name;
    for (; *end; ++end)
        if (*end == L'\\') base_name = end + 1;
    USHORT full_bytes = (USHORT)((end - BlArm64KernelFilePath) * sizeof(WCHAR));
    USHORT base_bytes = (USHORT)((end - base_name) * sizeof(WCHAR));
    module->BaseDllName = (UNICODE_STRING){base_bytes, base_bytes + sizeof(WCHAR), base_name};
    module->FullDllName = (UNICODE_STRING){full_bytes, full_bytes + sizeof(WCHAR), BlArm64KernelFilePath};
    module->LoadCount=1;
    InsertTailList(&block->Nt.LoadOrderListHead,&module->InLoadOrderLinks);
}
XP_EFI_STATUS BlArm64Boot(PVOID image, XP_EFI_SYSTEM_TABLE *system,
                          PXP_ARM64_BOOT_BLOCK block)
{
    if (!image || !system || !block || block->KernelImage.Base ||
        block->ExitAttempted || block->StackAllocationBase || block->Tables.PoolBase || block->Tables.StackArena ||
        ((ULONG_PTR)block & 63) ||
        system->Hdr.Signature != 0x5453595320494249ULL ||
        system->Hdr.HeaderSize < sizeof(*system))
        return XP_EFI_INVALID_PARAMETER;
    XP_EFI_BOOT_SERVICES_PREFIX *services = system->BootServices;
    if (!services || services->Hdr.Signature != 0x56524553544f4f42ULL ||
        services->Hdr.HeaderSize < sizeof(*services) || !services->AllocatePages ||
        !services->FreePages || !services->GetMemoryMap || !services->ExitBootServices || !services->HandleProtocol)
        return XP_EFI_INVALID_PARAMETER;
    ULONG64 level, spsel, tcr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    __asm__ volatile("mrs %0, SPSel" : "=r"(spsel));
    __asm__ volatile("mrs %0, TCR_EL1" : "=r"(tcr));
    /* EL2 transition and non-4KiB handoff are not implemented yet. */
    if (level != 4 || spsel != 1 || ((tcr >> 14) & 3) != 0)
        return XP_EFI_UNSUPPORTED;
    block->Size = sizeof(*block);
    block->Version = XP_ARM64_BOOT_VERSION;
    block->ExitAttempted = block->BootServicesExited = FALSE;
    block->ExitAttempts = 0;
    block->InitializationStatus = STATUS_PENDING;
    block->HalStatus = STATUS_PENDING;
    block->PlatformStatus = BlArm64DiscoverAcpiPlatform(system, &block->Platform);
    if (!NT_SUCCESS(BlArm64CaptureTime(system, &block->Time)))
        return XP_EFI_UNSUPPORTED;
    InitializeListHead(&block->Nt.LoadOrderListHead);
    InitializeListHead(&block->Nt.BootDriverListHead);
    block->Nt.LoadOptions = load_options;
    block->StackBase = 0;
    block->StackSize = KERNEL_STACK_SIZE;
    XP_EFI_STATUS status = services->AllocatePages(0, XP_EFI_LOADER_DATA,
                           block->StackSize / PAGE_SIZE + 1, &block->StackAllocationBase);
    if (status) {
        block->StackAllocationBase = 0;
        return status;
    }
    block->StackBase = block->StackAllocationBase + PAGE_SIZE;
    block->Tables.PoolPages = MM_BOOT_TABLE_PAGES;
    status = services->AllocatePages(0, XP_EFI_LOADER_DATA, block->Tables.PoolPages, &block->Tables.PoolBase);
    if (status) {
        block->Tables.PoolBase = 0;
        XP_EFI_STATUS cleanup = services->FreePages(block->StackAllocationBase,
                                                    block->StackSize / PAGE_SIZE + 1);
        if (cleanup) return cleanup;
        block->StackAllocationBase = block->StackBase = 0;
        return status;
    }
    status = services->AllocatePages(0, XP_EFI_LOADER_DATA, MM_BOOT_STACK_ARENA_PAGES, &block->Tables.StackArena);
    if (status) {
        block->Tables.StackArena = 0;
        XP_EFI_STATUS cleanup = services->FreePages(block->Tables.PoolBase, block->Tables.PoolPages);
        if (cleanup) return cleanup;
        block->Tables.PoolBase = 0;
        cleanup = services->FreePages(block->StackAllocationBase, block->StackSize / PAGE_SIZE + 1);
        if (cleanup) return cleanup;
        block->StackAllocationBase = block->StackBase = 0;
        return status;
    }
    RtlZeroMemory((PVOID)(ULONG_PTR)block->StackAllocationBase, block->StackSize + PAGE_SIZE);
    block->Nt.KernelStack = block->StackBase + block->StackSize;
    block->Nt.Prcb = (ULONG_PTR)&block->Pcr.Prcb;
    block->Nt.Process = (ULONG_PTR)&block->InitialProcess.Pcb;
    block->Nt.Thread = 0;

    status=BlArm64ReadKernel(image,services,&block->KernelImage,&block->Volume);
    if (status) goto cleanup;
    block->Nt.ArcBootDeviceName = block->Volume.ArcName;
    block->Nt.NtBootPathName = block->Volume.NtDirectory;
    publish_kernel_module(block);
    XP_ARM64_KERNEL_ENTRY entry=(XP_ARM64_KERNEL_ENTRY)block->KernelModule.EntryPoint;

    BlArm64CaptureDisplay(services, &block->Display);
    for (ULONG attempt = 0; attempt < 4; ++attempt) {
        block->MapSize = sizeof(block->Map);
        status = services->GetMemoryMap(&block->MapSize, block->Map, &block->MapKey,
                                        &block->DescriptorSize, &block->DescriptorVersion);
        if (status) break;
        if (!NT_SUCCESS(BlArm64ConvertMemoryMap(block))) {
            status = XP_EFI_UNSUPPORTED;
            break;
        }
        if (block->Display.PhysicalBase && !BlArm64DisplayReserved(block))
            RtlZeroMemory(&block->Display, sizeof(block->Display));
        /* No firmware call/allocation between map snapshot and exit. */
        __asm__ volatile("mov %0, sp" : "=r"(block->FirmwareStack));
        block->ExitAttempted = TRUE;
        ++block->ExitAttempts;
        status = services->ExitBootServices(image, block->MapKey);
        if (!status) {
            block->BootServicesExited = TRUE;
            __asm__ volatile("msr daifset, #15\nisb" ::: "memory");
            /* This code still executes at the loader's identity address.
             * Build both roots before calling into the high native PE. */
            NTSTATUS init=MmArm64PrepareBootFrames(block);
            if (NT_SUCCESS(init)) init=MmArm64TakeBootMappings(block);
            if (!NT_SUCCESS(init)) {
                block->InitializationStatus=init;
                return XP_EFI_ABORTED;
            }
            /* The PE's data-cache lines were cleaned via the staging VA.
             * Invalidate local instruction aliases before entering at its
             * new VA. No other CPU participates in the boot handoff. */
            __asm__ volatile("ic iallu\ndsb ish\nisb" ::: "memory");
            entry(block,block->Nt.KernelStack);
            for (;;) __asm__ volatile("wfe");
        }
        if (status != XP_EFI_INVALID_PARAMETER) break;
        /* Following the first exit attempt, use only memory services. */
    }
cleanup:
    if (!block->ExitAttempted) {
        XP_EFI_STATUS cleanup = 0;
        if (block->KernelImage.Base) {
            cleanup=services->FreePages(block->KernelImage.Base,block->KernelImage.Size>>PAGE_SHIFT);
            if (cleanup) return cleanup;
            RtlZeroMemory(&block->KernelImage,sizeof(block->KernelImage));
            RtlZeroMemory(&block->KernelModule,sizeof(block->KernelModule));
            RtlZeroMemory(&block->Volume,sizeof(block->Volume));
            block->Nt.ArcBootDeviceName = block->Nt.NtBootPathName = NULL;
            InitializeListHead(&block->Nt.LoadOrderListHead);
        }
        if (block->Tables.StackArena) {
            cleanup = services->FreePages(block->Tables.StackArena, MM_BOOT_STACK_ARENA_PAGES);
            if (cleanup) return cleanup;
            block->Tables.StackArena = 0;
        }
        cleanup = services->FreePages(block->Tables.PoolBase, block->Tables.PoolPages);
        if (cleanup) return cleanup;
        block->Tables.PoolBase = 0;
        cleanup = services->FreePages(block->StackAllocationBase, block->StackSize / PAGE_SIZE + 1);
        if (cleanup) return cleanup;
        block->StackAllocationBase = block->StackBase = 0;
    }
    /* After ExitAttempted, the caller must stop, never return to firmware. */
    return status ? status : XP_EFI_ABORTED;
}
