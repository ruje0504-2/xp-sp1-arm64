/* UEFI 2.10 GOP linear framebuffer handoff. The firmware owns the mode;
 * the kernel receives only reservation, geometry and pixel format. */
#include "boot.h"

typedef struct {
    ULONG Version, Width, Height, PixelFormat;
    ULONG PixelMasks[4];
    ULONG PixelsPerScanLine;
} XP_EFI_GOP_INFO;
typedef struct {
    ULONG MaxMode, Mode;
    const XP_EFI_GOP_INFO *Info;
    ULONG_PTR SizeOfInfo;
    ULONG64 FrameBufferBase;
    ULONG_PTR FrameBufferSize;
} XP_EFI_GOP_MODE;
typedef struct {
    PVOID QueryMode, SetMode, Blt;
    const XP_EFI_GOP_MODE *Mode;
} XP_EFI_GOP;

_Static_assert(sizeof(XP_EFI_GOP_INFO) == 36 && offsetof(XP_EFI_GOP_MODE, FrameBufferBase) == 24,
               "UEFI GOP mode geometry");

VOID BlArm64CaptureDisplay(XP_EFI_BOOT_SERVICES_PREFIX *services, XP_ARM64_BOOT_DISPLAY *display)
{
    static const GUID gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
    RtlZeroMemory(display, sizeof(*display));
    if (services->Hdr.HeaderSize < sizeof(XP_EFI_PROTOCOL_SERVICES)) return;
    XP_EFI_LOCATE_PROTOCOL locate = ((XP_EFI_PROTOCOL_SERVICES *)services)->LocateProtocol;
    const XP_EFI_GOP *gop = NULL;
    if (!locate || locate(&gop_guid, NULL, (PVOID *)&gop) || !gop || !gop->Mode) return;
    const XP_EFI_GOP_MODE *mode = gop->Mode;
    const XP_EFI_GOP_INFO *info = mode->Info;
    if (!info || mode->Mode >= mode->MaxMode || mode->SizeOfInfo < sizeof(*info) ||
        !mode->FrameBufferBase || (mode->FrameBufferBase & (PAGE_SIZE - 1)) ||
        !info->Width || !info->Height || info->PixelsPerScanLine < info->Width ||
        (info->PixelFormat != XP_DISPLAY_RGBX && info->PixelFormat != XP_DISPLAY_BGRX))
        return;
    ULONG64 pixels = (ULONG64)info->PixelsPerScanLine * info->Height;
    if (pixels > MM_BOOT_DISPLAY_LIMIT / sizeof(ULONG) ||
        pixels > mode->FrameBufferSize / sizeof(ULONG))
        return;
    display->PhysicalBase = mode->FrameBufferBase;
    display->Bytes = pixels * sizeof(ULONG);
    display->Width = info->Width;
    display->Height = info->Height;
    display->Stride = info->PixelsPerScanLine;
    display->Format = info->PixelFormat;
}

/* Map only an explicit reserved RAM descriptor. A free or unclassified
 * aperture must never become a writable framebuffer alias. */
BOOLEAN BlArm64DisplayReserved(PXP_ARM64_BOOT_BLOCK block)
{
    const XP_ARM64_BOOT_DISPLAY *display = &block->Display;
    for (ULONG i = 0; i < block->DescriptorCount; ++i) {
        const XP_EFI_MEMORY_DESCRIPTOR *entry =
            (const XP_EFI_MEMORY_DESCRIPTOR *)(block->Map + i * block->DescriptorSize);
        ULONG64 size = entry->NumberOfPages << PAGE_SHIFT;
        if (entry->Type == 0 && !(entry->Attribute & XP_EFI_MEMORY_RUNTIME) &&
            display->PhysicalBase >= entry->PhysicalStart) {
            ULONG64 offset = display->PhysicalBase - entry->PhysicalStart;
            if (offset < size && display->Bytes <= size - offset) return TRUE;
        }
    }
    return FALSE;
}
