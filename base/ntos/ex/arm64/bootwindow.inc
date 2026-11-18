/* Resident USER windows. gdi_lock serializes the z-order, backing
 * surfaces and their DCs. Each window borrows its owner's desktop binding. */
#define USER_WINDOW_LIMIT 64
#define USER_CLASS_LIMIT 64
#define USER_NAME_CAPACITY 128
#define USER_WS_POPUP 0x80000000u
#define USER_WS_CHILD 0x40000000u
#define USER_WINDOW_DEPTH_LIMIT 8
#define USER_WS_VISIBLE 0x10000000u
#define USER_WM_CREATE 0x0001u
#define USER_WM_DESTROY 0x0002u
#define USER_WM_NCCREATE 0x0081u
#define USER_WM_NCDESTROY 0x0082u

typedef enum {
    UserWindowNonclient,
    UserWindowClient,
    UserWindowReady,
    UserWindowDestroying
} ARM64_WINDOW_STATE;

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
    USHORT Atom, NameBytes, Function;
    WCHAR Name[USER_NAME_CAPACITY];
} ARM64_USER_CLASS;

struct _ARM64_USER_WINDOW {
    LIST_ENTRY ZOrder;
    ARM64_USER_WINDOW *Parent;
    PKTHREAD Owner;
    ARM64_CONSOLE_DESKTOP *Desktop;
    ARM64_USER_CLASS *Class;
    PEPROCESS_QUOTA_BLOCK Quota;
    SIZE_T Bytes;
    PULONG Pixels;
    LONG X, Y, Width, Height;
    USHORT Generation, Depth, Function;
    ARM64_WINDOW_STATE State;
    BOOLEAN Visible, DispatchingPaint, BeginningPaint;
    ULONG PaintDc;
    ARM64_PAINT_REGION Update, Painting;
    WCHAR Title[USER_NAME_CAPACITY];
};

typedef struct {
    ULONG_PTR Params, Module, Menu, Parent;
    LONG Height, Width, Y, X, Style;
    ULONG Reserved;
    ULONG_PTR Name, Class;
    ULONG ExStyle, ReservedTail;
} ARM64_CREATESTRUCT;

typedef struct {
    ARM64_USER_WNDPROC_CALL Header;
    ARM64_CREATESTRUCT Create;
    ULONG NameOffset, ClassOffset;
    WCHAR Strings[2 * USER_NAME_CAPACITY];
} ARM64_CREATE_CALL;

_Static_assert(sizeof(ARM64_CREATESTRUCT) == 80 && FIELD_OFFSET(ARM64_CREATESTRUCT, Name) == 56,
               "native CREATESTRUCTW");
_Static_assert(FIELD_OFFSET(ARM64_CREATE_CALL, Create) == 48 &&
               FIELD_OFFSET(ARM64_CREATE_CALL, Strings) == 136, "native create callback");

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

/* Parent precedes its complete subtree. A root contends only with other
 * roots, and a child contends only with its siblings inside the same root. */
static PLIST_ENTRY after_window_tree(const ARM64_USER_WINDOW *window)
{
    PLIST_ENTRY link = window->ZOrder.Flink;
    while (link != &window_order &&
           CONTAINING_RECORD(link, ARM64_USER_WINDOW, ZOrder)->Depth > window->Depth)
        link = link->Flink;
    return link;
}

static PLIST_ENTRY window_sibling_end(const ARM64_USER_WINDOW *parent)
{
    return parent ? after_window_tree(parent) : &window_order;
}

static VOID raise_window_tree(ARM64_USER_WINDOW *window)
{
    PLIST_ENTRY first = &window->ZOrder, after = after_window_tree(window);
    PLIST_ENTRY end = window_sibling_end(window->Parent);
    if (after == end) return;
    PLIST_ENTRY last = after->Blink;
    first->Blink->Flink = after;
    after->Blink = first->Blink;
    first->Blink = end->Blink;
    end->Blink->Flink = first;
    last->Flink = end;
    end->Blink = last;
}

static BOOLEAN window_subtree_contains(const ARM64_USER_WINDOW *root, const ARM64_USER_WINDOW *window)
{
    for (; window; window = window->Parent)
        if (window == root) return TRUE;
    return FALSE;
}

static BOOLEAN window_screen_clip(const ARM64_USER_WINDOW *window, ARM64_USER_RECT *clip)
{
    *clip = (ARM64_USER_RECT){window->X, window->Y, window->X + window->Width, window->Y + window->Height};
    for (; window; window = window->Parent) {
        if (!window->Visible) return FALSE;
        clip->Left = max(clip->Left, window->X);
        clip->Top = max(clip->Top, window->Y);
        clip->Right = min(clip->Right, window->X + window->Width);
        clip->Bottom = min(clip->Bottom, window->Y + window->Height);
    }
    return clip->Left < clip->Right && clip->Top < clip->Bottom;
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
        ARM64_USER_RECT clip;
        if (window->Desktop != damage->Desktop || !window_screen_clip(window, &clip)) continue;
        LONG first = max(left, clip.Left), last = min(right, clip.Right);
        LONG start = max(top, clip.Top), end = min(bottom, clip.Bottom);
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

static ULONG_PTR user_get_ancestor(WIN32_ARGS)
{
    if (b < 1 || b > 3) return 0;
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    if (b == 1) {
        if (window) window = window->Parent;
    } else {
        while (window && window->Parent) window = window->Parent;
    }
    ULONG result = window ? window_handle(window) : 0;
    gdi_leave();
    return result;
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

#include "bootfocus.inc"

static VOID window_destroy(ARM64_USER_WINDOW *window)
{
    ARM64_USER_THREAD *user = window->Owner->Win32Thread;
    ULONG handle = window_handle(window);
    if (user->FocusWindow == handle) {
        user->FocusWindow = 0;
        ++user->FocusRevision;
    }
    if (pointer_focus == handle) pointer_focus = 0;
    discard_window_posts(user, handle);
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
    if (--user->WindowCount == 0 && foreground_queue == user)
        foreground_queue = NULL;
}

/* Ordinary destruction may call the owner. Rundown instead calls the raw
 * release above because the exiting thread can no longer execute at EL0. */
static BOOLEAN destroy_user_window(ULONG_PTR handle)
{
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(handle);
    if (!window || window->State == UserWindowDestroying) {
        gdi_leave();
        return FALSE;
    }
    PLIST_ENTRY after = after_window_tree(window);
    for (PLIST_ENTRY link = window->ZOrder.Flink; link != after; link = link->Flink) {
        if (CONTAINING_RECORD(link, ARM64_USER_WINDOW, ZOrder)->State == UserWindowDestroying) {
            gdi_leave();
            return FALSE;
        }
    }
    BOOLEAN client_exists = window->State != UserWindowNonclient;
    ULONG retiring = 0, fallback = 0;
    ULONG_PTR procedure = window->Class->Procedure;
    window->State = UserWindowDestroying;
    if (window->Visible) {
        window->Visible = FALSE;
        present_window(window, 0, 0, window->Width, window->Height);
    }
    window_focus_loss(window, &retiring, &fallback);
    gdi_leave();

    ULONG_PTR result;
    if (retiring) update_window_focus(fallback, 0, retiring);
    if (client_exists) call_window_procedure(handle, procedure, USER_WM_DESTROY, 0, 0, &result);

    /* WM_DESTROY sees the children; WM_NCDESTROY runs after their teardown.
     * Re-read the first child after every callback because it can reenter. */
    for (;;) {
        gdi_enter();
        window = window_lookup(handle);
        ARM64_USER_WINDOW *child = window && window->ZOrder.Flink != &window_order ?
            CONTAINING_RECORD(window->ZOrder.Flink, ARM64_USER_WINDOW, ZOrder) : NULL;
        ULONG first_child = child && child->Parent == window ? window_handle(child) : 0;
        gdi_leave();
        if (!first_child) break;
        destroy_user_window(first_child);
    }

    ARM64_USER_WNDPROC_CALL cleanup = {.Window = handle, .Message = USER_WM_NCDESTROY,
                                      .Procedure = procedure};
    call_window_callback(XP_FI_FNNCDESTROY, &cleanup, sizeof(cleanup), &result);

    gdi_enter();
    window = window_lookup(handle);
    if (window) window_destroy(window);
    gdi_leave();
    return TRUE;
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
        if (entry->Owner != process || (entry->Module != module &&
            (module || entry->Function != XP_FNID_EDIT))) continue;
        const UNICODE_STRING registered = {entry->NameBytes, sizeof(entry->Name), entry->Name};
        if (name ? RtlEqualUnicodeString(&registered, name, TRUE) : entry->Atom == atom) return entry;
    }
    return NULL;
}

static BOOLEAN client_code(ULONG_PTR address)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    BOOLEAN executable = MmArm64UserCodeAddress(PsArm64UserProcessSpace(PsGetCurrentProcess()), address);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return executable;
}

static ULONG_PTR user_initialize_client(WIN32_ARGS)
{
    ARM64_USER_PROCESS *process = PsGetCurrentProcess()->Win32Process;
    if (!process || !KeGetCurrentThread()->Win32Thread) return (ULONG)STATUS_INVALID_DEVICE_STATE;
    if (!b || !d || ((a | b | c) & (sizeof(ULONG_PTR) - 1))) return (ULONG)STATUS_INVALID_PARAMETER;

    ULONG_PTR client[XP_PFNCLIENT_COUNT], worker[XP_PFNCLIENTWORKER_COUNT];
    if (!NT_SUCCESS(copy_user_input(b, client, sizeof(client)))) return (ULONG)STATUS_ACCESS_VIOLATION;
    ULONG_PTR edit = client[XP_PFNCLIENT_EDIT];
    if (!edit) return (ULONG)STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < XP_PFNCLIENT_COUNT; ++i)
        if (client[i] && !client_code(client[i])) return (ULONG)STATUS_ACCESS_VIOLATION;
    if (a) {
        if (!NT_SUCCESS(copy_user_input(a, client, sizeof(client)))) return (ULONG)STATUS_ACCESS_VIOLATION;
        for (ULONG i = 0; i < XP_PFNCLIENT_COUNT; ++i)
            if (client[i] && !client_code(client[i])) return (ULONG)STATUS_ACCESS_VIOLATION;
    }
    if (c) {
        if (!NT_SUCCESS(copy_user_input(c, worker, sizeof(worker)))) return (ULONG)STATUS_ACCESS_VIOLATION;
        for (ULONG i = 0; i < XP_PFNCLIENTWORKER_COUNT; ++i)
            if (worker[i] && !client_code(worker[i])) return (ULONG)STATUS_ACCESS_VIOLATION;
    }

    gdi_enter();
    BOOLEAN compatible = !process->EditProcedure ||
                         (process->EditProcedure == edit && process->ClientModule == d);
    if (compatible) {
        process->EditProcedure = edit;
        process->ClientModule = d;
    }
    gdi_leave();
    return (ULONG)(compatible ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER);
}

static ULONG_PTR user_set_window_fnid(WIN32_ARGS)
{
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    USHORT function = (USHORT)b;
    BOOLEAN valid = window && window->Class->Function == XP_FNID_EDIT &&
        ((function == XP_FNID_EDIT && window->State == UserWindowNonclient && !window->Function) ||
         (function == XP_FNID_CLEANEDUP_BIT && window->State == UserWindowDestroying &&
          window->Function == XP_FNID_EDIT));
    if (valid) window->Function |= function;
    gdi_leave();
    return valid;
}

static ULONG_PTR user_register_class(WIN32_ARGS)
{
    USHORT function = (USHORT)e;
    BOOLEAN edit = function == XP_FNID_EDIT;
    if ((function && !edit) || ((ULONG)f & ~0x80u) || g || !KeGetCurrentThread()->Win32Thread) return 0;
    ARM64_WNDCLASSEX definition;
    ULONG_PTR menu[3];
    UNICODE_STRING empty_menu, plain, version;
    WCHAR plain_name[USER_NAME_CAPACITY], version_name[USER_NAME_CAPACITY];
    if (!NT_SUCCESS(copy_user_input(a, &definition, sizeof(definition))) || definition.Size != sizeof(definition) ||
        (edit ? definition.Style != 0x4088 || definition.WindowExtra != sizeof(ULONG_PTR) :
                (definition.Style & ~0xbu) || definition.WindowExtra) || definition.ClassExtra ||
        definition.Icon || definition.Cursor || definition.Background || definition.MenuName || definition.SmallIcon ||
        !NT_SUCCESS(copy_user_input(d, menu, sizeof(menu))) || menu[0] || menu[1] ||
        !NT_SUCCESS(copy_user_input(menu[2], &empty_menu, sizeof(empty_menu))) || empty_menu.Length || empty_menu.Buffer ||
        !class_name(b, plain_name, &plain) || !class_name(c, version_name, &version) ||
        !RtlEqualUnicodeString(&plain, &version, TRUE)) return 0;

    static WCHAR edit_name_buffer[] = L"Edit";
    static const UNICODE_STRING edit_name = {8, 10, edit_name_buffer};
    if (!client_code(definition.Procedure) || (edit && !RtlEqualUnicodeString(&plain, &edit_name, TRUE))) return 0;

    ULONG atom = 0;
    gdi_enter();
    ARM64_USER_PROCESS *process = PsGetCurrentProcess()->Win32Process;
    BOOLEAN registered_client = !edit || (process && definition.Procedure == process->EditProcedure &&
                                          definition.Module == process->ClientModule);
    if (registered_client && next_class_atom <= 0xffff && !find_user_class(definition.Module, 0, &plain)) {
        for (ULONG slot = 0; slot < USER_CLASS_LIMIT; ++slot) {
            ARM64_USER_CLASS *entry = &user_classes[slot];
            if (entry->Owner) continue;
            entry->Module = definition.Module;
            entry->Procedure = definition.Procedure;
            entry->NameBytes = plain.Length;
            entry->Function = function;
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
    ULONG style = (ULONG)e;
    BOOLEAN child = (style & USER_WS_CHILD) != 0;
    ULONG kind = child ? USER_WS_CHILD : USER_WS_POPUP;
    if (!user || !(user->DesktopAccess & DESKTOP_CREATEWINDOW) || (ULONG)a || (ULONG)n || o ||
        !(style & kind) || (child ? !j : (j || k)) ||
        width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
        x == MINLONG || y == MINLONG) return 0;

    WCHAR plain_buffer[USER_NAME_CAPACITY], class_buffer[USER_NAME_CAPACITY], title_buffer[USER_NAME_CAPACITY] = {0};
    UNICODE_STRING plain_name, class_label, title = {0};
    const UNICODE_STRING *name = NULL;
    if (b > 0xffff || c > 0xffff) {
        if (!large_window_name(b, plain_buffer, &plain_name) || !large_window_name(c, class_buffer, &class_label) ||
            !class_label.Length || !RtlEqualUnicodeString(&plain_name, &class_label, TRUE)) return 0;
        name = &class_label;
    } else if (!c || b != c) return 0;
    if (d && !large_window_name(d, title_buffer, &title)) return 0;

    ULONG handle = 0, parent_handle = 0;
    ULONG_PTR procedure = 0;
    gdi_enter();
    ARM64_USER_WINDOW *parent = child ? window_lookup(j) : NULL;
    if (child && (!parent || parent->State == UserWindowNonclient || parent->Depth >= USER_WINDOW_DEPTH_LIMIT)) {
        gdi_leave();
        return 0;
    }
    for (ARM64_USER_WINDOW *ancestor = parent; ancestor; ancestor = ancestor->Parent) {
        if (ancestor->State == UserWindowDestroying) {
            gdi_leave();
            return 0;
        }
    }
    LONGLONG screen_x = (LONGLONG)x + (parent ? parent->X : 0);
    LONGLONG screen_y = (LONGLONG)y + (parent ? parent->Y : 0);
    if (screen_x < (LONG)MINLONG || screen_x > MAXLONG - width ||
        screen_y < (LONG)MINLONG || screen_y > MAXLONG - height) {
        gdi_leave();
        return 0;
    }
    if (parent) parent_handle = window_handle(parent);
    ARM64_USER_CLASS *registered = find_user_class(l, c, name);
    ULONG allowed_style = kind | USER_WS_VISIBLE;
    if (registered && registered->Function == XP_FNID_EDIT && child)
        allowed_style |= 0x4u | 0x80u | 0x800u | 0x1000u; /* ES_MULTILINE, AUTOHSCROLL, READONLY, WANTRETURN */
    if (registered && !(style & ~allowed_style)) {
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
            window->X = (LONG)screen_x;
            window->Y = (LONG)screen_y;
            window->Parent = parent;
            window->Depth = parent ? parent->Depth + 1 : 0;
            window->Function = 0;
            window->Width = width;
            window->Height = height;
            window->Visible = FALSE;
            window->State = UserWindowNonclient;
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
            InsertTailList(window_sibling_end(parent), &window->ZOrder);
            procedure = registered->Procedure;
            break;
        }
    }
    gdi_leave();
    if (!handle) return 0;

    /* Only the captured strings and the application's opaque parameter enter
     * the callout. The receiver rebases these offsets inside its own stack. */
    ARM64_CREATE_CALL call;
    RtlZeroMemory(&call, sizeof(call));
    call.Header.Window = handle;
    call.Header.Procedure = procedure;
    call.Header.Message = USER_WM_NCCREATE;
    call.Create = (ARM64_CREATESTRUCT){.Params = m, .Module = l, .Menu = k, .Parent = parent_handle,
                                       .Height = height, .Width = width, .Y = y, .X = x,
                                       .Style = (LONG)e, .Class = name ? 0 : c};
    ULONG bytes = FIELD_OFFSET(ARM64_CREATE_CALL, Strings);
    if (d) {
        call.NameOffset = bytes;
        RtlCopyMemory((PUCHAR)&call + bytes, title.Buffer, title.Length + sizeof(WCHAR));
        bytes += title.Length + sizeof(WCHAR);
    }
    if (name) {
        call.ClassOffset = bytes;
        RtlCopyMemory((PUCHAR)&call + bytes, name->Buffer, name->Length + sizeof(WCHAR));
        bytes += name->Length + sizeof(WCHAR);
    }

    ULONG_PTR result;
    if (!call_window_callback(XP_FI_FNINLPCREATESTRUCT, &call, bytes, &result) || !result) {
        destroy_user_window(handle);
        return 0;
    }

    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(handle);
    if (window) window->State = UserWindowClient;
    gdi_leave();
    if (!window) return 0;

    call.Header.Message = USER_WM_CREATE;
    if (!call_window_callback(XP_FI_FNINLPCREATESTRUCT, &call, bytes, &result) ||
        (LONG_PTR)result == -1) {
        destroy_user_window(handle);
        return 0;
    }

    gdi_enter();
    window = window_lookup(handle);
    if (window) {
        window->State = UserWindowReady;
        window->Visible = !!(e & USER_WS_VISIBLE);
        if (window->Visible) present_window(window, 0, 0, width, height);
    }
    gdi_leave();
    return window ? handle : 0;
}

static ULONG_PTR user_show_window(WIN32_ARGS)
{
    if (b != 0 && b != 1 && b != 5) return FALSE; /* SW_HIDE, SW_SHOWNORMAL, SW_SHOW */
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    BOOLEAN previous = window && window->Visible;
    ULONG hidden = 0, fallback = 0;
    if (window && window->State == UserWindowReady && previous != !!b) {
        window->Visible = !!b;
        if (window->Visible) raise_window_tree(window);
        else window_focus_loss(window, &hidden, &fallback);
        present_window(window, 0, 0, window->Width, window->Height);
    }
    gdi_leave();
    if (hidden) update_window_focus(fallback, 0, hidden);
    return previous;
}

static ULONG_PTR user_destroy_window(WIN32_ARGS)
{
    return destroy_user_window(a);
}
