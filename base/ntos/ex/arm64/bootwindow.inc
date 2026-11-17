/* Resident top-level USER windows. gdi_lock serializes the z-order, backing
 * surfaces and their DCs. Each window borrows its owner's desktop binding. */
#define USER_WINDOW_LIMIT 64
#define USER_CLASS_LIMIT 64
#define USER_NAME_CAPACITY 128
#define USER_WS_POPUP 0x80000000u
#define USER_WS_VISIBLE 0x10000000u

typedef struct {
    ULONG Size, Style;
    ULONG_PTR Procedure;
    LONG ClassExtra, WindowExtra;
    ULONG_PTR Module, Icon, Cursor, Background, MenuName, ClassName, SmallIcon;
} ARM64_WNDCLASSEX;

typedef struct {
    ULONG Length, MaximumLength;
    ULONG_PTR Buffer;
} ARM64_LARGE_STRING;

typedef struct {
    PEPROCESS Owner;
    ULONG_PTR Module, Procedure;
    USHORT Atom, NameBytes;
    WCHAR Name[USER_NAME_CAPACITY];
} ARM64_USER_CLASS;

struct _ARM64_USER_WINDOW {
    LIST_ENTRY ZOrder;
    PKTHREAD Owner;
    ARM64_CONSOLE_DESKTOP *Desktop;
    ARM64_USER_CLASS *Class;
    PEPROCESS_QUOTA_BLOCK Quota;
    SIZE_T Bytes;
    PULONG Pixels;
    LONG X, Y, Width, Height;
    USHORT Generation;
    BOOLEAN Visible, DispatchingPaint, BeginningPaint;
    ULONG PaintDc;
    ARM64_PAINT_REGION Update, Painting;
    WCHAR Title[USER_NAME_CAPACITY];
};

_Static_assert(sizeof(ARM64_WNDCLASSEX) == 80, "native WNDCLASSEX");
_Static_assert(sizeof(ARM64_LARGE_STRING) == 16, "native LARGE_STRING");

static ARM64_USER_CLASS user_classes[USER_CLASS_LIMIT];
static ARM64_USER_WINDOW user_windows[USER_WINDOW_LIMIT];
static LIST_ENTRY window_order = {&window_order, &window_order};
static ULONG next_class_atom = 0xc000;

static BOOLEAN desktop_is_presented(const ARM64_CONSOLE_DESKTOP *desktop)
{
    static const WCHAR text[] = L"Default";
    const UNICODE_STRING name = {sizeof(text) - sizeof(WCHAR), sizeof(text), (PWCHAR)text};
    return desktop->Station->Interactive && RtlEqualUnicodeString(&desktop->Name, &name, TRUE);
}

/* Repaint only the damaged screen rectangle, from the desktop background
 * to the topmost window. Occluded writes remain in their own backing store. */
static VOID present_window(const ARM64_USER_WINDOW *damage, LONG x, LONG y, LONG width, LONG height)
{
    const XP_ARM64_BOOT_DISPLAY *display = boot_display();
    if (!display->VirtualBase || !desktop_is_presented(damage->Desktop)) return;
    LONG left = max(0, (LONGLONG)damage->X + x);
    LONG top = max(0, (LONGLONG)damage->Y + y);
    LONG right = min(display->Width, (LONGLONG)damage->X + x + width);
    LONG bottom = min(display->Height, (LONGLONG)damage->Y + y + height);
    if (left >= right || top >= bottom) return;

    PULONG screen = (PULONG)display->VirtualBase;
    for (LONG row = top; row < bottom; ++row)
        RtlZeroMemory(screen + (SIZE_T)row * display->Stride + left, (right - left) * sizeof(ULONG));

    for (PLIST_ENTRY link = window_order.Flink; link != &window_order; link = link->Flink) {
        const ARM64_USER_WINDOW *window = CONTAINING_RECORD(link, ARM64_USER_WINDOW, ZOrder);
        if (!window->Visible || window->Desktop != damage->Desktop) continue;
        LONG first = max(left, window->X), last = min(right, window->X + window->Width);
        LONG start = max(top, window->Y), end = min(bottom, window->Y + window->Height);
        if (first >= last) continue;
        for (LONG row = start; row < end; ++row) {
            const ULONG *from = window->Pixels + (SIZE_T)(row - window->Y) * window->Width + (first - window->X);
            PULONG to = screen + (SIZE_T)row * display->Stride + first;
            if (display->Format == XP_DISPLAY_RGBX) {
                for (LONG column = first; column < last; ++column) *to++ = swap_red_blue(*from++);
            } else RtlCopyMemory(to, from, (last - first) * sizeof(ULONG));
        }
    }
    for (LONG row = top; row < bottom; ++row)
        flush_display(screen + (SIZE_T)row * display->Stride + left, (right - left) * sizeof(ULONG));
}

static ARM64_USER_WINDOW *window_lookup_live(ULONG_PTR handle)
{
    ULONG value = (ULONG)handle;
    if (handle != value && handle != (ULONG_PTR)(LONG_PTR)(LONG)value) return NULL;
    ULONG index = (value & 0xffffu) - 0x100u;
    if (index >= USER_WINDOW_LIMIT) return NULL;
    ARM64_USER_WINDOW *window = &user_windows[index];
    return window->Owner && window->Generation == (value >> 16) ? window : NULL;
}

static ARM64_USER_WINDOW *window_lookup(ULONG_PTR handle)
{
    ARM64_USER_WINDOW *window = window_lookup_live(handle);
    return window && window->Owner == KeGetCurrentThread() ? window : NULL;
}

static ULONG window_handle(const ARM64_USER_WINDOW *window)
{
    return ((ULONG)window->Generation << 16) | (0x100u + (ULONG)(window - user_windows));
}

static BOOLEAN empty_rect(const ARM64_USER_RECT *rect)
{
    return rect->Left >= rect->Right || rect->Top >= rect->Bottom;
}

static VOID union_bounds(ARM64_USER_RECT *bounds, const ARM64_USER_RECT *rect)
{
    if (empty_rect(bounds)) *bounds = *rect;
    else {
        bounds->Left = min(bounds->Left, rect->Left);
        bounds->Top = min(bounds->Top, rect->Top);
        bounds->Right = max(bounds->Right, rect->Right);
        bounds->Bottom = max(bounds->Bottom, rect->Bottom);
    }
}

static VOID invalidate_client(ARM64_USER_WINDOW *window, const ARM64_USER_RECT *client, BOOLEAN erase)
{
    ARM64_USER_RECT rect = {0, 0, window->Width, window->Height};
    if (client) {
        rect.Left = max(rect.Left, client->Left);
        rect.Top = max(rect.Top, client->Top);
        rect.Right = min(rect.Right, client->Right);
        rect.Bottom = min(rect.Bottom, client->Bottom);
    }
    if (empty_rect(&rect)) return;

    ARM64_PAINT_REGION *update = &window->Update;
    if (!client) RtlSetAllBits(&update->Bits);
    else {
        for (LONG y = rect.Top; y < rect.Bottom; ++y)
            RtlSetBits(&update->Bits, (ULONG)y * window->Width + rect.Left, rect.Right - rect.Left);
    }
    union_bounds(&update->Bounds, &rect);
    update->Erase |= erase;
}

static VOID window_destroy(ARM64_USER_WINDOW *window)
{
    ARM64_USER_THREAD *user = window->Owner->Win32Thread;
    discard_window_posts(user, window_handle(window));
    RemoveEntryList(&window->ZOrder);
    if (window->Visible) present_window(window, 0, 0, window->Width, window->Height);
    for (ULONG slot = 0; slot < GDI_SLOT_COUNT; ++slot) {
        ARM64_GDI_ENTRY *entry = &gdi_objects[slot];
        if (entry->Owner && entry->Type == GdiMemoryDc &&
            ((ARM64_GDI_DC *)entry->Object)->Window == window)
            gdi_free(entry);
    }
    ExFreePool(window->Pixels);
    PsReturnSharedPoolQuota(window->Quota, 0, window->Bytes);
    window->Owner = NULL;
    --user->WindowCount;
}

static BOOLEAN copy_window_name(ULONG_PTR address, ULONG length, ULONG capacity,
                                 WCHAR buffer[USER_NAME_CAPACITY], UNICODE_STRING *name)
{
    if ((length & 1) || length > capacity || length >= USER_NAME_CAPACITY * sizeof(WCHAR) ||
        (length && !NT_SUCCESS(copy_user_input(address, buffer, length)))) return FALSE;
    buffer[length / sizeof(WCHAR)] = 0;
    for (ULONG i = 0; i < length / sizeof(WCHAR); ++i)
        if (!buffer[i]) return FALSE;
    *name = (UNICODE_STRING){(USHORT)length, USER_NAME_CAPACITY * sizeof(WCHAR), buffer};
    return TRUE;
}

static BOOLEAN class_name(ULONG_PTR address, WCHAR buffer[USER_NAME_CAPACITY], UNICODE_STRING *name)
{
    UNICODE_STRING captured;
    return NT_SUCCESS(copy_user_input(address, &captured, sizeof(captured))) && captured.Length &&
           copy_window_name((ULONG_PTR)captured.Buffer, captured.Length, captured.MaximumLength, buffer, name);
}

static BOOLEAN large_window_name(ULONG_PTR address, WCHAR buffer[USER_NAME_CAPACITY], UNICODE_STRING *name)
{
    ARM64_LARGE_STRING captured;
    return NT_SUCCESS(copy_user_input(address, &captured, sizeof(captured))) &&
           !(captured.MaximumLength & 0x80000000u) &&
           copy_window_name(captured.Buffer, captured.Length, captured.MaximumLength, buffer, name);
}

static ARM64_USER_CLASS *find_user_class(ULONG_PTR module, ULONG_PTR atom, const UNICODE_STRING *name)
{
    PEPROCESS process = PsGetCurrentProcess();
    for (ULONG i = 0; i < USER_CLASS_LIMIT; ++i) {
        ARM64_USER_CLASS *entry = &user_classes[i];
        if (entry->Owner != process || entry->Module != module) continue;
        const UNICODE_STRING registered = {entry->NameBytes, sizeof(entry->Name), entry->Name};
        if (name ? RtlEqualUnicodeString(&registered, name, TRUE) : entry->Atom == atom) return entry;
    }
    return NULL;
}

static ULONG_PTR user_register_class(WIN32_ARGS)
{
    /* Local Unicode classes, without owned/class DCs, menu or extra bytes. */
    if (e || (f & ~0x80u) || g || !KeGetCurrentThread()->Win32Thread) return 0;
    ARM64_WNDCLASSEX definition;
    ULONG_PTR menu[3];
    UNICODE_STRING empty_menu, plain, version;
    WCHAR plain_name[USER_NAME_CAPACITY], version_name[USER_NAME_CAPACITY];
    if (!NT_SUCCESS(copy_user_input(a, &definition, sizeof(definition))) || definition.Size != sizeof(definition) ||
        (definition.Style & ~0xbu) || definition.ClassExtra || definition.WindowExtra ||
        definition.Icon || definition.Cursor || definition.Background || definition.MenuName || definition.SmallIcon ||
        !NT_SUCCESS(copy_user_input(d, menu, sizeof(menu))) || menu[0] || menu[1] ||
        !NT_SUCCESS(copy_user_input(menu[2], &empty_menu, sizeof(empty_menu))) || empty_menu.Length || empty_menu.Buffer ||
        !class_name(b, plain_name, &plain) || !class_name(c, version_name, &version) ||
        !RtlEqualUnicodeString(&plain, &version, TRUE)) return 0;

    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    BOOLEAN executable = MmArm64UserCodeAddress(PsArm64UserProcessSpace(PsGetCurrentProcess()), definition.Procedure);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!executable) return 0;

    ULONG atom = 0;
    gdi_enter();
    if (next_class_atom <= 0xffff && !find_user_class(definition.Module, 0, &plain)) {
        for (ULONG slot = 0; slot < USER_CLASS_LIMIT; ++slot) {
            ARM64_USER_CLASS *entry = &user_classes[slot];
            if (entry->Owner) continue;
            entry->Module = definition.Module;
            entry->Procedure = definition.Procedure;
            entry->NameBytes = plain.Length;
            RtlCopyMemory(entry->Name, plain.Buffer, plain.Length);
            atom = entry->Atom = (USHORT)next_class_atom++;
            entry->Owner = PsGetCurrentProcess();
            break;
        }
    }
    gdi_leave();
    return atom;
}

static ULONG_PTR user_create_window(WIN32_ARGS)
{
    PKTHREAD current = KeGetCurrentThread();
    ARM64_USER_THREAD *user = current->Win32Thread;
    LONG x = (LONG)f, y = (LONG)g, width = (LONG)h, height = (LONG)i;
    if (!user || !(user->DesktopAccess & DESKTOP_CREATEWINDOW) || a || j || k || m || n || o ||
        !((ULONG)e & USER_WS_POPUP) || ((ULONG)e & ~(USER_WS_POPUP | USER_WS_VISIBLE)) ||
        width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
        x == MINLONG || y == MINLONG || x > MAXLONG - width || y > MAXLONG - height) return 0;

    WCHAR plain_buffer[USER_NAME_CAPACITY], class_buffer[USER_NAME_CAPACITY], title_buffer[USER_NAME_CAPACITY] = {0};
    UNICODE_STRING plain_name, class_label, title = {0};
    const UNICODE_STRING *name = NULL;
    if (b > 0xffff || c > 0xffff) {
        if (!large_window_name(b, plain_buffer, &plain_name) || !large_window_name(c, class_buffer, &class_label) ||
            !class_label.Length || !RtlEqualUnicodeString(&plain_name, &class_label, TRUE)) return 0;
        name = &class_label;
    } else if (!c || b != c) return 0;
    if (d && !large_window_name(d, title_buffer, &title)) return 0;

    ULONG handle = 0;
    gdi_enter();
    ARM64_USER_CLASS *registered = find_user_class(l, c, name);
    if (registered) {
        for (ULONG slot = 0; slot < USER_WINDOW_LIMIT; ++slot) {
            ARM64_USER_WINDOW *window = &user_windows[slot];
            if (window->Owner || window->Generation == 0xffff) continue;
            ULONG pixel_count = (ULONG)width * height;
            ULONG mask_words = (pixel_count + 31) / 32;
            SIZE_T bytes = ((SIZE_T)pixel_count + 2 * mask_words) * sizeof(ULONG);
            PEPROCESS_QUOTA_BLOCK quota = PsChargeSharedPoolQuota(PsGetCurrentProcess(), 0, bytes);
            if (!quota) break;
            PULONG pixels = ExAllocatePoolWithTag(NonPagedPool, bytes, 'nwUW');
            if (!pixels) {
                PsReturnSharedPoolQuota(quota, 0, bytes);
                break;
            }
            RtlZeroMemory(pixels, bytes);
            window->Quota = quota;
            window->Bytes = bytes;
            window->Pixels = pixels;
            window->Class = registered;
            window->Desktop = user->Desktop;
            window->X = x;
            window->Y = y;
            window->Width = width;
            window->Height = height;
            window->Visible = !!(e & USER_WS_VISIBLE);
            window->PaintDc = 0;
            window->DispatchingPaint = window->BeginningPaint = FALSE;
            window->Update = window->Painting = (ARM64_PAINT_REGION){0};
            RtlInitializeBitMap(&window->Update.Bits, pixels + pixel_count, pixel_count);
            RtlInitializeBitMap(&window->Painting.Bits, pixels + pixel_count + mask_words, pixel_count);
            invalidate_client(window, NULL, FALSE);
            RtlCopyMemory(window->Title, title_buffer, title.Length + sizeof(WCHAR));
            window->Owner = current;
            ++user->WindowCount;
            ++window->Generation;
            handle = window_handle(window);
            InsertTailList(&window_order, &window->ZOrder);
            if (window->Visible) present_window(window, 0, 0, width, height);
            break;
        }
    }
    gdi_leave();
    return handle;
}

static ULONG_PTR user_show_window(WIN32_ARGS)
{
    if (b != 0 && b != 1 && b != 5) return FALSE; /* SW_HIDE, SW_SHOWNORMAL, SW_SHOW */
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    BOOLEAN previous = window && window->Visible;
    if (window && previous != !!b) {
        window->Visible = !!b;
        if (window->Visible) {
            RemoveEntryList(&window->ZOrder);
            InsertTailList(&window_order, &window->ZOrder);
        }
        present_window(window, 0, 0, window->Width, window->Height);
    }
    gdi_leave();
    return previous;
}

static ULONG_PTR user_destroy_window(WIN32_ARGS)
{
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    if (window) window_destroy(window);
    gdi_leave();
    return window != NULL;
}
