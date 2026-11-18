/* Native EL0 fixture for window creation and lifetime during nested callbacks. */
#include "xp-win32-services.h"
#include "../base/win32/user32/arm64/edit.h"

typedef unsigned long long U64;
typedef unsigned int UINT;
typedef unsigned short WCHAR;
typedef struct { UINT Length, Capacity; const WCHAR *Buffer; } LARGE_STRING;
typedef struct {
    U64 Params, Module, Menu, Parent;
    int Height, Width, Y, X, Style;
    UINT Reserved;
    const WCHAR *Name, *Class;
    UINT ExStyle, ReservedTail;
} CREATE;
_Static_assert(sizeof(CREATE) == 80 && __builtin_offsetof(CREATE, Class) == 64, "native CREATESTRUCTW");

U64 XpWindowCreate(UINT, U64, U64, const LARGE_STRING *, UINT, int, int, int, int,
                  U64, U64, U64, U64, UINT, U64);
int XpWindowDestroy(U64);
U64 XpWindowGetDc(U64);
int XpWindowCallOne(U64, UINT);

enum { Create = 1, Destroy = 2, NcCreate = 0x81, NcDestroy = 0x82 };
enum { Idle, NamedSuccess, RejectNonclient, RejectClient, ReplaceDuringCreate };
static const WCHAR caption[] = L"Native lifetime";
static const WCHAR class_name[] = L"Arm64Raster";
static const LARGE_STRING title = {sizeof(caption) - 2, sizeof(caption), caption};
static const LARGE_STRING name = {sizeof(class_name) - 2, sizeof(class_name), class_name};
static const U64 parameter = 0x1234567812345678ULL;
static UINT atom, mode, events, failure;
static U64 current, replacement, dc;
static const CREATE *expected_child;
static U64 family[3];
static UINT child_creates, child_events, child_error;
static int observe_family_destroy;
static const U64 editor_parameter = 0x57494e444f574544ULL;
static U64 editor_child;
static int use_stock_editor;

U64 XpWindowAncestor(U64, UINT);


static int same_text(const WCHAR *left, const WCHAR *right, UINT count)
{
    if (!left) return 0;
    for (UINT i = 0; i < count; ++i)
        if (left[i] != right[i]) return 0;
    return 1;
}

static U64 create_probe(int named)
{
    U64 class_value = named ? (U64)&name : atom;
    return XpWindowCreate(0, class_value, class_value, &title, 0x90000000u, -71, -59, 16, 13,
                          0, 0, 0, parameter, 0, 0);
}

U64 XpNativeWindowLifecycle(U64 window, UINT message, U64 wparam, const CREATE *create)
{
    if (!mode) {
        if (message == Create && create && create->Params == editor_parameter) {
            U64 class = use_stock_editor ? (U64)&XpUser32EditClass : atom;
            editor_child = XpWindowCreate(0, class, class, 0, 0x50000000u, 4, 4, 64, 18,
                                          window, 101, 0, 0, 0, 0);
            if (!editor_child) return (U64)-1;
        }
        if (expected_child && (message == NcCreate || message == Create)) {
            if (!create || create->Parent != expected_child->Parent || create->Menu != expected_child->Menu ||
                create->X != expected_child->X || create->Y != expected_child->Y ||
                create->Width != expected_child->Width || create->Height != expected_child->Height ||
                (UINT)create->Style != 0x50000000u || XpWindowAncestor(window, 1) != create->Parent)
                child_error = __LINE__;
            ++child_creates;
        }
        if (observe_family_destroy && (message == Destroy || message == NcDestroy)) {
            for (UINT i = 0; i < 3; ++i) {
                if (window != family[i]) continue;
                child_events = (child_events << 4) | (i + 1 + (message == NcDestroy ? 8 : 0));
                if (i == 0 && message == Destroy && XpWindowAncestor(family[2], 2) != family[0])
                    child_error = __LINE__;
                if (i == 0 && message == NcDestroy && XpWindowAncestor(family[2], 2))
                    child_error = __LINE__;
            }
        }
        return message == NcCreate;
    }
    if (message == NcCreate) current = window;
    if (wparam || !window || window != current) failure = __LINE__;
    if (message == NcCreate || message == Create) {
        events = (events << 4) | (message == NcCreate ? 1 : 2);
        if (!create || create->Params != parameter || create->Module || create->Menu || create->Parent ||
            create->Height != 13 || create->Width != 16 || create->Y != -59 || create->X != -71 ||
            (UINT)create->Style != 0x90000000u || create->ExStyle || create->Reserved || create->ReservedTail ||
            !same_text(create->Name, caption, sizeof(caption) / sizeof(caption[0])) ||
            (mode == NamedSuccess ? !same_text(create->Class, class_name, sizeof(class_name) / sizeof(class_name[0]))
                                  : (U64)create->Class != atom))
            failure = __LINE__;
        if (message == NcCreate) {
            dc = XpWindowGetDc(window);
            if (!dc) failure = __LINE__;
            return mode != RejectNonclient;
        }
        if (mode == RejectClient) return (U64)-1;
        if (mode == ReplaceDuringCreate) {
            if (!XpWindowDestroy(window) || XpWindowGetDc(window)) failure = __LINE__;
            mode = Idle;
            replacement = create_probe(0);
            mode = ReplaceDuringCreate;
            if (!replacement || replacement == window || create->Params != parameter ||
                !same_text(create->Name, caption, sizeof(caption) / sizeof(caption[0])))
                failure = __LINE__;
        }
        return 0;
    }
    events = (events << 4) | (message == Destroy ? 3 : 4);
    /* The handle supports queries until WM_NCDESTROY unwinds. A second
     * DestroyWindow attempt must never re-enter the same two callbacks. */
    U64 during = XpWindowGetDc(window);
    if (create || !during || !XpWindowCallOne(during, XP_SFI_RELEASEDC) || XpWindowDestroy(window))
        failure = __LINE__;
    return 0;
}

UINT XpNativeWindowProbe(UINT window_class, U64 evidence[6])
{
    atom = window_class;
    for (mode = NamedSuccess; mode <= ReplaceDuringCreate; ++mode) {
        events = 0;
        U64 result = create_probe(mode == NamedSuccess);
        if (mode == NamedSuccess) {
            if (result != current || events != 0x12 || !XpWindowDestroy(result)) return __LINE__;
        } else if (result) return __LINE__;
        if (events != (mode == RejectNonclient ? 0x14u : 0x1234u) ||
            !dc || XpWindowGetDc(current) || XpWindowCallOne(dc, XP_SFI_RELEASEDC))
            return __LINE__;
        if (failure) return failure;
        evidence[mode - 1] = events;
    }
    mode = Idle;
    if ((replacement & 0xffff) != (current & 0xffff)) return __LINE__;
    U64 replacement_dc = XpWindowGetDc(replacement);
    if (!replacement_dc || !XpWindowDestroy(replacement) ||
        XpWindowCallOne(replacement_dc, XP_SFI_RELEASEDC)) return __LINE__;
    evidence[4] = current;
    evidence[5] = replacement;
    return 0;
}

U64 XpWindowSetFocus(U64);
U64 XpWindowGetThreadState(UINT);
int XpWindowShow(U64, UINT);

enum { FocusGained = 7, FocusLost = 8 };
enum { ObserveFocus, RedirectFocus, DestroyProposedFocus };
static U64 focus_windows[3];
static UINT focus_events, focus_error, focus_action;
static int focus_recording;
UINT XpWindowHardwareFocusCount;

static UINT focus_id(U64 window)
{
    if (!window) return 0;
    for (UINT i = 0; i < 3; ++i)
        if (focus_windows[i] == window) return i + 1;
    return 7;
}

U64 XpNativeFocusMessage(U64 window, UINT message, U64 other, U64 lparam)
{
    U64 live_focus = XpWindowGetThreadState(XP_THREAD_STATE_FOCUS);
    int good = !lparam && live_focus == (message == FocusGained ? window : other);
    if (!focus_recording) {
        XpWindowHardwareFocusCount += good ? (message == FocusGained ? 0x100 : 1) : 0x10000;
        return 0;
    }
    UINT id = focus_id(window), peer = focus_id(other);
    if (!good || id < 1 || id > 3 || peer > 3) focus_error = __LINE__;
    focus_events = (focus_events << 5) | (id << 3) | (message == FocusGained ? 4 : 0) | peer;
    if (message == FocusLost && id == 1 && peer == 2) {
        if (focus_action == RedirectFocus && XpWindowSetFocus(focus_windows[2]) != other)
            focus_error = __LINE__;
        if (focus_action == DestroyProposedFocus && !XpWindowDestroy(other))
            focus_error = __LINE__;
    }
    return 0;
}

UINT XpNativeFocusProbe(U64 evidence[4])
{
    U64 a = focus_windows[0] = create_probe(0);
    U64 b = focus_windows[1] = create_probe(0);
    U64 c = focus_windows[2] = create_probe(0);
    if (!a || !b || !c || XpWindowGetThreadState(XP_THREAD_STATE_FOCUS)) return __LINE__;

    focus_recording = 1;
    if (XpWindowSetFocus(a) || XpWindowSetFocus(a) != a || focus_events != 0x0c) return __LINE__;
    focus_events = 0;
    if (XpWindowSetFocus(b) != a || XpWindowSetFocus(0) != b ||
        XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) || focus_events != 0x2ab0) return __LINE__;
    evidence[0] = focus_events;

    if (XpWindowSetFocus(a)) return __LINE__;
    focus_events = 0;
    focus_action = RedirectFocus;
    if (XpWindowSetFocus(b) != a || XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) != c ||
        focus_events != 0x2a7e) return __LINE__;
    evidence[1] = focus_events;

    focus_action = ObserveFocus;
    if (XpWindowSetFocus(a) != c) return __LINE__;
    focus_events = 0;
    focus_action = DestroyProposedFocus;
    if (XpWindowSetFocus(b) != a || XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) ||
        focus_events != 0x150) return __LINE__;
    evidence[2] = focus_events;

    focus_action = ObserveFocus;
    if (XpWindowSetFocus(c)) return __LINE__;
    focus_events = 0;
    if (XpWindowSetFocus(b) || XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) != c ||
        !XpWindowShow(c, 0) || XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) ||
        focus_events != 0x18 || !XpWindowDestroy(a) || !XpWindowDestroy(c))
        return __LINE__;
    if (focus_error) return focus_error;
    focus_recording = 0;
    return 0;
}

UINT XpWindowSetPixel(U64, int, int, UINT);
UINT XpEditPixel(U64, int, int);
int XpWindowPost(U64, UINT, U64, U64);
int XpWindowPeek(void *, U64, UINT, UINT, UINT);

static U64 make_box(U64 parent, U64 id, int x, int y, int width, int height)
{
    CREATE expected = {.Parent = parent, .Menu = id, .X = x, .Y = y, .Width = width, .Height = height};
    expected_child = parent ? &expected : 0;
    U64 result = XpWindowCreate(0, atom, atom, 0, parent ? 0x50000000u : 0x90000000u,
                                x, y, width, height, parent, id, 0, 0, 0, 0);
    expected_child = 0;
    return result;
}

static U64 colored_dc(U64 window, int width, int height, UINT color)
{
    U64 dc = XpWindowGetDc(window);
    if (!dc) return 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (XpWindowSetPixel(dc, x, y, color) != color) return 0;
    return dc;
}

UINT XpNativeChildProbe(U64 evidence[4], U64 screen)
{
    const UINT parent_color = 0x319221, child_color = 0x2741da;
    const UINT grand_color = 0xc4172b, cover_color = 0xe4b699;
    U64 parent = family[0] = make_box(0, 0, 390, 104, 12, 10);
    U64 cover = make_box(0, 0, 399, 108, 2, 3);
    U64 child = family[1] = make_box(parent, 41, 8, 3, 8, 6);
    U64 grandchild = family[2] = make_box(child, 42, -2, 4, 8, 5);
    if (!parent || !cover || !child || !grandchild || child_creates != 4 || child_error) return __LINE__;
    if (XpWindowAncestor(grandchild, 1) != child || XpWindowAncestor(grandchild, 2) != parent ||
        XpWindowAncestor(parent, 1) || XpWindowAncestor(parent, 3) != parent ||
        XpWindowAncestor(child, 4) || make_box(1, 9, 0, 0, 2, 2) ||
        make_box(parent, 9, 0x7ffffff0, 0, 2, 2))
        return __LINE__;

    U64 parent_dc = colored_dc(parent, 12, 10, parent_color);
    U64 cover_dc = colored_dc(cover, 2, 3, cover_color);
    U64 child_dc = colored_dc(child, 8, 6, child_color);
    U64 grand_dc = colored_dc(grandchild, 8, 5, grand_color);
    if (!parent_dc || !cover_dc || !child_dc || !grand_dc || XpEditPixel(grand_dc, 0, 3) != grand_color)
        return __LINE__;

    UINT checked = 0;
    if (screen) {
        for (int y = 102; y < 117; ++y) {
            for (int x = 388; x < 409; ++x) {
                UINT expected = 0;
                if (x >= 390 && x < 402 && y >= 104 && y < 114) expected = parent_color;
                if (x >= 398 && x < 402 && y >= 107 && y < 113) expected = child_color;
                if (x >= 398 && x < 402 && y >= 111 && y < 113) expected = grand_color;
                if (x >= 399 && x < 401 && y >= 108 && y < 111) expected = cover_color;
                if (XpEditPixel(screen, x, y) != expected) return __LINE__;
                ++checked;
            }
        }
    }

    U64 message[6];
    if (!XpWindowPost(grandchild, 0x8002, 31, 43) ||
        !XpWindowPeek(message, parent, 0x8002, 0x8002, 1) || message[0] != grandchild ||
        (UINT)message[1] != 0x8002 || message[2] != 31 || message[3] != 43 ||
        XpWindowPeek(message, parent, 0x8002, 0x8002, 1)) return __LINE__;

    if (XpWindowSetFocus(grandchild) || !XpWindowShow(parent, 0) ||
        XpWindowGetThreadState(XP_THREAD_STATE_FOCUS)) return __LINE__;
    if (screen && (XpEditPixel(screen, 398, 111) || XpEditPixel(screen, 399, 108) != cover_color))
        return __LINE__;
    if (XpWindowShow(parent, 1)) return __LINE__;
    if (screen && (XpEditPixel(screen, 399, 108) != child_color ||
                   XpEditPixel(screen, 398, 111) != grand_color)) return __LINE__;
    if (XpWindowSetFocus(grandchild) || !XpWindowShow(child, 0) ||
        XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) != parent ||
        XpWindowShow(child, 1)) return __LINE__;

    observe_family_destroy = 1;
    if (!XpWindowDestroy(parent) || child_events != 0x123ba9 || child_error) return __LINE__;
    observe_family_destroy = 0;
    if (XpWindowGetDc(child) || XpWindowGetDc(grandchild) || XpWindowAncestor(grandchild, 2) ||
        XpWindowCallOne(child_dc, XP_SFI_RELEASEDC) || XpWindowCallOne(grand_dc, XP_SFI_RELEASEDC) ||
        XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) || !XpWindowDestroy(cover)) return __LINE__;
    if (XpWindowHardwareFocusCount != 0x303) return __LINE__;
    XpWindowHardwareFocusCount = 0;
    evidence[0] = child_creates;
    evidence[1] = checked;
    evidence[2] = child_events;
    evidence[3] = child;
    return 0;
}

/* The editor follows the application pattern of creating the child while its
 * parent is handling WM_CREATE, before either window is presented. */
U64 XpNativeCreateEditHost(U64 *child)
{
    U64 parent = XpWindowCreate(0, atom, atom, 0, 0x90000000u, 300, 76, 72, 26,
                               0, 0, 0, editor_parameter, 0, 0);
    *child = editor_child;
    return parent;
}

U64 XpNativeCreateStockEditHost(U64 *child)
{
    use_stock_editor = 1;
    U64 host = XpNativeCreateEditHost(child);
    use_stock_editor = 0;
    return host;
}
