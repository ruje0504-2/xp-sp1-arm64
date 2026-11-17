/* Resident software memory DCs. Handle fields follow the XP GDI handle
 * layout; the native service profile owns its private, typed table. */
#define GDI_FIRST_INDEX 11u
#define GDI_SLOT_COUNT 256u
#define GDI_STOCK_BITMAP ((1u << 23) | (5u << 16) | 10u)
#define GDI_SRCCOPY 0x00cc0020u
#define GDI_CLR_INVALID 0xffffffffu

typedef enum { GdiMemoryDc = 1, GdiBitmap = 5 } ARM64_GDI_TYPE;
typedef struct _ARM64_USER_WINDOW ARM64_USER_WINDOW;

typedef struct {
    LONG Left, Top, Right, Bottom;
} ARM64_USER_RECT;

typedef struct {
    RTL_BITMAP Bits;
    ARM64_USER_RECT Bounds;
    BOOLEAN Erase;
} ARM64_PAINT_REGION;

typedef struct {
    LONG Width, Height;
    ULONG SelectedDc;
    ULONG Pixels[1];
} ARM64_GDI_BITMAP;

typedef struct {
    ULONG BitmapHandle;
    ARM64_GDI_BITMAP *Bitmap;
    PKTHREAD DisplayOwner;
    ARM64_CONSOLE_DESKTOP *Desktop;
    ARM64_USER_WINDOW *Window;
    BOOLEAN ReadScreen, Paint;
} ARM64_GDI_DC;

typedef struct {
    PULONG Pixels;
    LONG Width, Height, Stride;
    BOOLEAN Rgbx, Display;
    ARM64_USER_WINDOW *Window;
    const ARM64_PAINT_REGION *Clip;
} ARM64_GDI_RASTER;

typedef struct {
    PEPROCESS Owner;
    PEPROCESS_QUOTA_BLOCK Quota;
    PVOID Object;
    SIZE_T Bytes;
    ULONG Handle;
    UCHAR Generation;
    ARM64_GDI_TYPE Type;
} ARM64_GDI_ENTRY;

static EX_PUSH_LOCK gdi_lock;
static ARM64_GDI_ENTRY gdi_objects[GDI_SLOT_COUNT];

static const XP_ARM64_BOOT_DISPLAY *boot_display(VOID)
{
    return &CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt)->Display;
}

static VOID flush_display(const VOID *buffer, SIZE_T size)
{
    ULONG64 ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    ULONG_PTR line = 4ULL << ((ctr >> 16) & 15);
    ULONG_PTR end = (ULONG_PTR)buffer + size;
    for (ULONG_PTR at = (ULONG_PTR)buffer & ~(line - 1); at < end; at += line)
        __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static VOID initialize_console_display(VOID)
{
    const XP_ARM64_BOOT_DISPLAY *display = boot_display();
    if (!display->VirtualBase) return;
    PVOID pixels = (PVOID)display->VirtualBase;
    RtlZeroMemory(pixels, display->Bytes);
    flush_display(pixels, display->Bytes);
}

static VOID gdi_enter(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&gdi_lock);
}

static VOID gdi_leave(VOID)
{
    ExReleasePushLockExclusive(&gdi_lock);
    KeLeaveCriticalRegion();
}

static ARM64_GDI_ENTRY *gdi_lookup(ULONG_PTR handle, ARM64_GDI_TYPE type)
{
    ULONG value = (ULONG)handle;
    if (handle != value && handle != (ULONG_PTR)(LONG_PTR)(LONG)value) return NULL;
    ULONG index = value & 0xffffu;
    if (index < GDI_FIRST_INDEX || index - GDI_FIRST_INDEX >= GDI_SLOT_COUNT) return NULL;
    ARM64_GDI_ENTRY *entry = &gdi_objects[index - GDI_FIRST_INDEX];
    return entry->Owner == PsGetCurrentProcess() && entry->Handle == value &&
           (!type || entry->Type == type) ? entry : NULL;
}

static ULONG gdi_allocate(ARM64_GDI_TYPE type, SIZE_T bytes, PVOID *result)
{
    for (ULONG slot = 0; slot < GDI_SLOT_COUNT; ++slot) {
        ARM64_GDI_ENTRY *entry = &gdi_objects[slot];
        if (entry->Owner) continue;
        PEPROCESS process = PsGetCurrentProcess();
        PEPROCESS_QUOTA_BLOCK quota = PsChargeSharedPoolQuota(process, 0, bytes);
        if (!quota) return 0;
        PVOID object = ExAllocatePoolWithTag(NonPagedPool, bytes, 'idGA');
        if (!object) {
            PsReturnSharedPoolQuota(quota, 0, bytes);
            return 0;
        }
        RtlZeroMemory(object, bytes);
        entry->Owner = process;
        entry->Quota = quota;
        entry->Object = *result = object;
        entry->Bytes = bytes;
        entry->Type = type;
        ++entry->Generation;
        entry->Handle = ((ULONG)entry->Generation << 24) | ((ULONG)type << 16) | (slot + GDI_FIRST_INDEX);
        return entry->Handle;
    }
    return 0;
}

static VOID gdi_free(ARM64_GDI_ENTRY *entry)
{
    if (entry->Type == GdiMemoryDc) {
        ARM64_GDI_DC *dc = entry->Object;
        if (dc->Bitmap) dc->Bitmap->SelectedDc = 0;
    }
    ExFreePool(entry->Object);
    PsReturnSharedPoolQuota(entry->Quota, 0, entry->Bytes);
    entry->Object = NULL;
    entry->Owner = NULL;
}

static ULONG swap_red_blue(ULONG color)
{
    return ((color & 0xffu) << 16) | (color & 0xff00u) | ((color >> 16) & 0xffu);
}

static ULONG acquire_user_dc(PKTHREAD thread, ARM64_CONSOLE_DESKTOP *desktop,
                            ARM64_USER_WINDOW *window, BOOLEAN read_screen, BOOLEAN paint)
{
    ARM64_GDI_DC *dc;
    ULONG handle = gdi_allocate(GdiMemoryDc, sizeof(*dc), (PVOID *)&dc);
    if (handle) {
        dc->DisplayOwner = thread;
        dc->Desktop = desktop;
        dc->Window = window;
        dc->ReadScreen = read_screen;
        dc->Paint = paint;
    }
    return handle;
}

static VOID discard_window_posts(ARM64_USER_THREAD *thread, ULONG_PTR hwnd);

#include "bootwindow.inc"
#include "bootpaint.inc"
#include "bootmsg.inc"
#include "bootinput.inc"

VOID ExpArm64ReleaseGdiObjects(PEPROCESS process)
{
    gdi_enter();
    for (ULONG slot = 0; slot < USER_CLASS_LIMIT; ++slot)
        if (user_classes[slot].Owner == process) user_classes[slot].Owner = NULL;
    /* Release selection references before freeing any bitmaps. */
    for (ULONG slot = 0; slot < GDI_SLOT_COUNT; ++slot) {
        ARM64_GDI_ENTRY *entry = &gdi_objects[slot];
        if (entry->Owner == process && entry->Type == GdiMemoryDc) gdi_free(entry);
    }
    for (ULONG slot = 0; slot < GDI_SLOT_COUNT; ++slot) {
        ARM64_GDI_ENTRY *entry = &gdi_objects[slot];
        if (entry->Owner == process) gdi_free(entry);
    }
    gdi_leave();
}

VOID ExpArm64ReleaseThreadUserObjects(PKTHREAD thread)
{
    gdi_enter();
    for (ULONG slot = 0; slot < USER_WINDOW_LIMIT; ++slot)
        if (user_windows[slot].Owner == thread) window_destroy(&user_windows[slot]);
    for (ULONG slot = 0; slot < GDI_SLOT_COUNT; ++slot) {
        ARM64_GDI_ENTRY *entry = &gdi_objects[slot];
        if (entry->Owner && entry->Type == GdiMemoryDc &&
            ((ARM64_GDI_DC *)entry->Object)->DisplayOwner == thread)
            gdi_free(entry);
    }
    gdi_leave();
}

ULONG ExpArm64CountGdiObjects(PEPROCESS process)
{
    ULONG count = 0;
    gdi_enter();
    for (ULONG slot = 0; slot < GDI_SLOT_COUNT; ++slot)
        count += gdi_objects[slot].Owner == process;
    gdi_leave();
    return count;
}

ULONG ExpArm64CountUserObjects(PEPROCESS process)
{
    ULONG count = 0;
    gdi_enter();
    for (ULONG slot = 0; slot < USER_CLASS_LIMIT; ++slot)
        count += user_classes[slot].Owner == process;
    for (ULONG slot = 0; slot < USER_WINDOW_LIMIT; ++slot)
        count += user_windows[slot].Owner && ((PETHREAD)user_windows[slot].Owner)->ThreadsProcess == process;
    gdi_leave();
    return count;
}

static ULONG_PTR gdi_create_bitmap(WIN32_ARGS)
{
    LONG width = (LONG)a, height = (LONG)b;
    if (width <= 0 || height <= 0 || (ULONG)c != 1 || (ULONG)d != 32) return 0;
    /* In the 32 bpp API bitmap format every DWORD is a full pixel and
     * the word-aligned and DWORD-aligned scanline lengths coincide. */
    ULONG64 bytes = (ULONG64)width * height * sizeof(ULONG);
    if (bytes > MAXLONG - FIELD_OFFSET(ARM64_GDI_BITMAP, Pixels)) return 0;
    gdi_enter();
    ARM64_GDI_BITMAP *bitmap;
    ULONG handle = gdi_allocate(GdiBitmap, FIELD_OFFSET(ARM64_GDI_BITMAP, Pixels) + bytes, (PVOID *)&bitmap);
    if (handle) {
        bitmap->Width = width;
        bitmap->Height = height;
        if (e && !NT_SUCCESS(copy_user_input(e, bitmap->Pixels, bytes))) {
            gdi_free(gdi_lookup(handle, GdiBitmap));
            handle = 0;
        }
    }
    gdi_leave();
    return handle;
}

static ULONG_PTR gdi_create_dc(WIN32_ARGS)
{
    gdi_enter();
    ARM64_GDI_DC *dc;
    ULONG handle = !a || gdi_lookup(a, GdiMemoryDc) ? gdi_allocate(GdiMemoryDc, sizeof(*dc), (PVOID *)&dc) : 0;
    if (handle) dc->BitmapHandle = GDI_STOCK_BITMAP;
    gdi_leave();
    return handle;
}

static ULONG_PTR gdi_select_bitmap(WIN32_ARGS)
{
    ULONG previous = 0;
    gdi_enter();
    ARM64_GDI_ENTRY *entry = gdi_lookup(a, GdiMemoryDc);
    ARM64_GDI_ENTRY *surface = gdi_lookup(b, GdiBitmap);
    if (entry && !((ARM64_GDI_DC *)entry->Object)->DisplayOwner && (surface || b == GDI_STOCK_BITMAP)) {
        ARM64_GDI_DC *dc = entry->Object;
        ARM64_GDI_BITMAP *bitmap = surface ? surface->Object : NULL;
        if (!bitmap || !bitmap->SelectedDc || bitmap->SelectedDc == entry->Handle) {
            previous = dc->BitmapHandle;
            if (dc->Bitmap) dc->Bitmap->SelectedDc = 0;
            dc->Bitmap = bitmap;
            dc->BitmapHandle = (ULONG)b;
            if (bitmap) bitmap->SelectedDc = entry->Handle;
        }
    }
    gdi_leave();
    return previous;
}

static ULONG_PTR user_get_dc(WIN32_ARGS)
{
    PKTHREAD thread = KeGetCurrentThread();
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&binding_lock);
    ARM64_USER_THREAD *user = thread->Win32Thread;
    ARM64_USER_PROCESS *process = PsGetCurrentProcess()->Win32Process;
    ARM64_CONSOLE_DESKTOP *desktop = user ? user->Desktop : NULL;
    BOOLEAN screen = !a && desktop && process && desktop_is_presented(desktop) && boot_display()->VirtualBase;
    BOOLEAN read_screen = screen && (process->StationAccess & WINSTA_READSCREEN);
    ExReleasePushLockShared(&binding_lock);
    KeLeaveCriticalRegion();
    if (!desktop || (!a && !screen)) return 0;

    gdi_enter();
    ARM64_USER_WINDOW *window = a ? window_lookup(a) : NULL;
    ULONG handle = screen || window ? acquire_user_dc(thread, desktop, window, read_screen, FALSE) : 0;
    gdi_leave();
    return handle;
}

static ULONG_PTR user_call_one_param(WIN32_ARGS)
{
    if (b == XP_SFI_POSTQUITMESSAGE) return post_quit_message((ULONG)a);
    if (b != XP_SFI_RELEASEDC) return 0;
    gdi_enter();
    ARM64_GDI_ENTRY *entry = gdi_lookup(a, GdiMemoryDc);
    const ARM64_GDI_DC *dc = entry ? entry->Object : NULL;
    BOOLEAN allowed = dc && !dc->Paint && dc->DisplayOwner == KeGetCurrentThread();
    if (allowed) gdi_free(entry);
    gdi_leave();
    return allowed;
}

static BOOLEAN gdi_dc_raster(ULONG_PTR handle, BOOLEAN write, ARM64_GDI_RASTER *result)
{
    ARM64_GDI_ENTRY *entry = gdi_lookup(handle, GdiMemoryDc);
    if (!entry) return FALSE;
    const ARM64_GDI_DC *dc = entry->Object;
    if (dc->DisplayOwner) {
        PKTHREAD current = KeGetCurrentThread();
        const ARM64_USER_THREAD *user = current->Win32Thread;
        if (dc->DisplayOwner != current || !user || dc->Desktop != user->Desktop) return FALSE;
        ARM64_USER_WINDOW *window = dc->Window;
        if (window) {
            *result = (ARM64_GDI_RASTER){window->Pixels, window->Width, window->Height, window->Width,
                                         FALSE, FALSE, window, dc->Paint ? &window->Painting : NULL};
        } else {
            if (!write && !dc->ReadScreen) return FALSE;
            const XP_ARM64_BOOT_DISPLAY *display = boot_display();
            *result = (ARM64_GDI_RASTER){(PULONG)display->VirtualBase, display->Width, display->Height,
                                         display->Stride, display->Format == XP_DISPLAY_RGBX, TRUE, NULL, NULL};
        }
    } else {
        ARM64_GDI_BITMAP *bitmap = dc->Bitmap;
        if (!bitmap) return FALSE;
        *result = (ARM64_GDI_RASTER){bitmap->Pixels, bitmap->Width, bitmap->Height, bitmap->Width, FALSE, FALSE, NULL, NULL};
    }
    return TRUE;
}

static ULONG gdi_pixel(ULONG_PTR handle, LONG x, LONG y, BOOLEAN write, ULONG color)
{
    ULONG result = GDI_CLR_INVALID;
    gdi_enter();
    ARM64_GDI_RASTER raster;
    if (gdi_dc_raster(handle, write, &raster) && x >= 0 && y >= 0 && x < raster.Width && y < raster.Height &&
        (!raster.Clip || RtlCheckBit(&raster.Clip->Bits, (ULONG)y * raster.Width + x))) {
        PULONG pixel = raster.Pixels + (SIZE_T)y * raster.Stride + x;
        if (write) {
            *pixel = raster.Rgbx ? color & 0xffffffu : swap_red_blue(color);
            if (raster.Display) flush_display(pixel, sizeof(*pixel));
            if (raster.Window && raster.Window->Visible) present_window(raster.Window, x, y, 1, 1);
            result = color & 0xffffffu;
        } else result = raster.Rgbx ? *pixel & 0xffffffu : swap_red_blue(*pixel);
    }
    gdi_leave();
    return result;
}

static ULONG_PTR gdi_set_pixel(WIN32_ARGS)
{
    return gdi_pixel(a, (LONG)b, (LONG)c, TRUE, (ULONG)d);
}

static ULONG_PTR gdi_get_pixel(WIN32_ARGS)
{
    return gdi_pixel(a, (LONG)b, (LONG)c, FALSE, 0);
}

static ULONG_PTR gdi_get_bitmap_bits(WIN32_ARGS)
{
    SIZE_T bytes = 0;
    gdi_enter();
    ARM64_GDI_ENTRY *entry = gdi_lookup(a, GdiBitmap);
    if (entry) {
        ARM64_GDI_BITMAP *bitmap = entry->Object;
        bytes = entry->Bytes - FIELD_OFFSET(ARM64_GDI_BITMAP, Pixels);
        if (c) {
            bytes = min(bytes, (ULONG)b);
            if (bytes && !NT_SUCCESS(copy_user_buffer(c, bitmap->Pixels, bytes, TRUE))) bytes = 0;
        }
    }
    gdi_leave();
    return (NTSTATUS)bytes;
}

static ULONG_PTR gdi_delete_object(WIN32_ARGS)
{
    if (a == GDI_STOCK_BITMAP) return TRUE;
    gdi_enter();
    ARM64_GDI_ENTRY *entry = gdi_lookup(a, 0);
    BOOLEAN allowed = entry && (entry->Type == GdiBitmap ? !((ARM64_GDI_BITMAP *)entry->Object)->SelectedDc
                                                      : !((ARM64_GDI_DC *)entry->Object)->DisplayOwner);
    if (allowed) gdi_free(entry);
    gdi_leave();
    return allowed;
}

static VOID copy_paint_span(const ARM64_GDI_RASTER *dst, PULONG to, const ULONG *from,
                            ULONG first, LONG count, LONG step, BOOLEAN swap)
{
    for (LONG offset = step > 0 ? 0 : count - 1; offset >= 0 && offset < count; offset += step)
        if (RtlCheckBit(&dst->Clip->Bits, first + offset))
            to[offset] = swap ? swap_red_blue(from[offset]) : from[offset];
}

/* Clip the matching source and destination spans before calculating pointers.
 * Width and height, after clipping, fit inside both surfaces. */
static VOID gdi_copy_rect(const ARM64_GDI_RASTER *dst, LONG dx, LONG dy, LONG width, LONG height,
                          const ARM64_GDI_RASTER *src, LONG sx, LONG sy)
{
    LONGLONG left = max(0, max(-(LONGLONG)dx, -(LONGLONG)sx));
    LONGLONG top = max(0, max(-(LONGLONG)dy, -(LONGLONG)sy));
    LONGLONG right = min(width, min((LONGLONG)dst->Width - dx, (LONGLONG)src->Width - sx));
    LONGLONG bottom = min(height, min((LONGLONG)dst->Height - dy, (LONGLONG)src->Height - sy));
    if (dst->Clip) {
        const ARM64_USER_RECT *bounds = &dst->Clip->Bounds;
        left = max(left, (LONGLONG)bounds->Left - dx);
        top = max(top, (LONGLONG)bounds->Top - dy);
        right = min(right, (LONGLONG)bounds->Right - dx);
        bottom = min(bottom, (LONGLONG)bounds->Bottom - dy);
    }
    if (left >= right || top >= bottom) return;
    BOOLEAN same = dst->Pixels == src->Pixels;
    LONG step = same && dy > sy ? -1 : 1;
    for (LONGLONG y = step > 0 ? top : bottom - 1; top <= y && y < bottom; y += step) {
        PULONG to = dst->Pixels + ((dy + y) * dst->Stride + dx + left);
        PULONG from = src->Pixels + ((sy + y) * src->Stride + sx + left);
        SIZE_T count = right - left;
        if (dst->Clip) {
            ULONG first = (ULONG)(dy + y) * dst->Width + (ULONG)(dx + left);
            copy_paint_span(dst, to, from, first, (LONG)count, same && dx > sx ? -1 : 1, src->Rgbx != dst->Rgbx);
        } else if (src->Rgbx == dst->Rgbx) RtlMoveMemory(to, from, count * sizeof(ULONG));
        else {
            for (SIZE_T x = 0; x < count; ++x) to[x] = swap_red_blue(from[x]);
        }
        if (dst->Display) flush_display(to, count * sizeof(ULONG));
    }
    if (dst->Window && dst->Window->Visible)
        present_window(dst->Window, dx + left, dy + top, right - left, bottom - top);
}

static ULONG_PTR gdi_bitblt(WIN32_ARGS)
{
    if ((ULONG)i != GDI_SRCCOPY || k || (LONG)d < 0 || (LONG)e < 0) return FALSE;
    gdi_enter();
    ARM64_GDI_RASTER dst, src;
    BOOLEAN valid = gdi_dc_raster(a, TRUE, &dst) && gdi_dc_raster(f, FALSE, &src);
    if (valid) gdi_copy_rect(&dst, (LONG)b, (LONG)c, (LONG)d, (LONG)e, &src, (LONG)g, (LONG)h);
    gdi_leave();
    return valid;
}
