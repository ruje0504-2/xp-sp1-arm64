/* UEFI 2.10 Loaded Image DeviceHandle and Simple File System. Raw file
 * bytes go to the NT PE loader, without invoking firmware LoadImage. */
#include "pe.h"

typedef struct {
    ULONG Revision;
    PVOID ParentHandle;
    XP_EFI_SYSTEM_TABLE *SystemTable;
    PVOID DeviceHandle;
} XP_EFI_LOADED_IMAGE_PREFIX;

typedef struct _XP_EFI_FILE XP_EFI_FILE;
typedef struct _XP_EFI_FILE_SYSTEM XP_EFI_FILE_SYSTEM;
struct _XP_EFI_FILE {
    ULONG64 Revision;
    XP_EFI_STATUS (*Open)(XP_EFI_FILE *, XP_EFI_FILE **, PWCHAR, ULONG64, ULONG64);
    XP_EFI_STATUS (*Close)(XP_EFI_FILE *);
    PVOID Delete;
    XP_EFI_STATUS (*Read)(XP_EFI_FILE *, PULONG_PTR, PVOID);
    PVOID Write;
    XP_EFI_STATUS (*GetPosition)(XP_EFI_FILE *, PULONG64);
    XP_EFI_STATUS (*SetPosition)(XP_EFI_FILE *, ULONG64);
};
struct _XP_EFI_FILE_SYSTEM {
    ULONG64 Revision;
    XP_EFI_STATUS (*OpenVolume)(XP_EFI_FILE_SYSTEM *, XP_EFI_FILE **);
};

_Static_assert(offsetof(XP_EFI_LOADED_IMAGE_PREFIX,DeviceHandle)==24, "UEFI device handle");
_Static_assert(offsetof(XP_EFI_FILE,Read)==32 && offsetof(XP_EFI_FILE,SetPosition)==56, "UEFI file");
static const GUID loaded_image_id={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const GUID simple_fs_id={0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const GUID device_path_id={0x09576e91,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
WCHAR BlArm64KernelFilePath[] = L"\\ntoskrnl.exe";

static XP_EFI_STATUS capture_boot_volume(PVOID device, XP_EFI_BOOT_SERVICES_PREFIX *services,
                                         XP_ARM64_BOOT_VOLUME *boot)
{
    PVOID interface = NULL;
    XP_EFI_STATUS status = services->HandleProtocol(device, &device_path_id, &interface);
    if (status) return status;
    boot->DevicePathBytes = BlArm64EncodeArcDevicePath(interface, sizeof(boot->DevicePath), boot->ArcName);
    if (!boot->DevicePathBytes) return XP_EFI_UNSUPPORTED;
    RtlCopyMemory(boot->DevicePath, interface, boot->DevicePathBytes);

    const WCHAR *name = BlArm64KernelFilePath;
    const WCHAR *last_separator = name;
    for (const WCHAR *p = name; *p; ++p)
        if (*p == L'\\') last_separator = p;
    ULONG length = (ULONG)(last_separator - name) + 1;
    if (name[0] != L'\\' || !name[length] || length >= sizeof(boot->NtDirectory))
        return XP_EFI_UNSUPPORTED;
    for (ULONG i = 0; i < length; ++i) {
        if (name[i] < L' ' || name[i] > L'~') return XP_EFI_UNSUPPORTED;
        boot->NtDirectory[i] = (CHAR)name[i];
    }
    boot->NtDirectory[length] = 0;
    return 0;
}

static XP_EFI_STATUS read_file(XP_EFI_FILE *file, UCHAR *buffer, ULONG64 size)
{
    while (size) {
        ULONG_PTR got=(ULONG_PTR)size;
        XP_EFI_STATUS status=file->Read(file,&got,buffer);
        if (status) return status;
        if (!got || got>size) return XP_EFI_LOAD_ERROR;
        size-=got;
        buffer+=got;
    }
    return 0;
}

XP_EFI_STATUS BlArm64ReadKernel(PVOID image, XP_EFI_BOOT_SERVICES_PREFIX *services,
                                PXP_ARM64_BOOT_IMAGE kernel, XP_ARM64_BOOT_VOLUME *boot)
{
    PVOID interface=NULL;
    XP_EFI_STATUS status=services->HandleProtocol(image,&loaded_image_id,&interface);
    if (status) return status;
    XP_EFI_LOADED_IMAGE_PREFIX *loaded=interface;
    if (!loaded || loaded->Revision<0x1000 || !loaded->DeviceHandle) return XP_EFI_UNSUPPORTED;
    PVOID device = loaded->DeviceHandle;
    XP_ARM64_BOOT_VOLUME discovered = {0};
    status = capture_boot_volume(device, services, &discovered);
    if (status) return status;
    status=services->HandleProtocol(device,&simple_fs_id,&interface);
    if (status) return status;
    XP_EFI_FILE_SYSTEM *volume=interface;
    if (!volume || volume->Revision<0x10000 || !volume->OpenVolume) return XP_EFI_UNSUPPORTED;
    XP_EFI_FILE *root=NULL;
    status=volume->OpenVolume(volume,&root);
    if (status) return status;
    if (!root || !root->Open || !root->Close) return XP_EFI_UNSUPPORTED;

    XP_EFI_FILE *file=NULL;
    status=root->Open(root,&file,BlArm64KernelFilePath,1,0); /* EFI_FILE_MODE_READ */
    XP_EFI_STATUS cleanup=root->Close(root);
    if (status) return status;
    if (!file || !file->Close) return XP_EFI_UNSUPPORTED;
    if (cleanup || !file->Read || !file->GetPosition || !file->SetPosition) {
        file->Close(file);
        return cleanup ? cleanup : XP_EFI_UNSUPPORTED;
    }

    ULONG64 size=0, buffer=0;
    ULONG_PTR pages=0;
    BOOLEAN allocated=FALSE;
    status=file->SetPosition(file,~0ULL); /* End of file */
    if (!status) status=file->GetPosition(file,&size);
    if (!status && (!size || size>XP_BOOT_IMAGE_LIMIT)) status=XP_EFI_LOAD_ERROR;
    if (!status) status=file->SetPosition(file,0);
    if (!status) {
        pages=(ULONG_PTR)((size+PAGE_SIZE-1)>>PAGE_SHIFT);
        status=services->AllocatePages(0,XP_EFI_LOADER_DATA,pages,&buffer);
        if (!status) {
            allocated=TRUE;
            status=read_file(file,(PVOID)(ULONG_PTR)buffer,size);
        }
    }
    cleanup=file->Close(file);
    if (!status) status=cleanup;
    if (!status) status=BlArm64LoadNativePe(services,(PVOID)(ULONG_PTR)buffer,size,kernel);
    if (allocated) {
        cleanup=services->FreePages(buffer,pages);
        if (!status && cleanup) {
            services->FreePages(kernel->Base,kernel->Size>>PAGE_SHIFT);
            RtlZeroMemory(kernel,sizeof(*kernel));
            status=cleanup;
        }
    }
    if (!status) *boot = discovered;
    return status;
}
