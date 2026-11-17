/* Coherent input transport delivers Linux evdev records at PASSIVE_LEVEL.
 * USER owns the pointer, focus and queue under gdi_lock. */
#define USER_WM_KEYDOWN 0x0100u
#define USER_WM_SYSKEYDOWN 0x0104u
#define USER_WM_MOUSEMOVE 0x0200u
#define USER_WM_LBUTTONDOWN 0x0201u
#define USER_WM_RBUTTONDOWN 0x0204u
#define USER_WM_MBUTTONDOWN 0x0207u
#define USER_MK_SHIFT 0x0004u
#define USER_MK_CONTROL 0x0008u

static ULONG focused_window;
static LONG pointer_x, pointer_y;
static ULONG pointer_buttons;
static BOOLEAN pointer_moved;
static BOOLEAN key_down[128];

static VOID post_input(ARM64_USER_WINDOW *window, ULONG category, ULONG message, ULONG_PTR wparam, ULONG_PTR lparam)
{
    if (!window) return;
    enqueue_user_message(window->Owner->Win32Thread, category, window_handle(window),
                         message, wparam, lparam, pointer_x, pointer_y);
}

static ARM64_USER_WINDOW *foreground_window(BOOLEAN hit_test)
{
    if (!hit_test) {
        ARM64_USER_WINDOW *focused = window_lookup_live(focused_window);
        if (focused && focused->Visible && desktop_is_presented(focused->Desktop)) return focused;
    }
    for (PLIST_ENTRY link = window_order.Blink; link != &window_order; link = link->Blink) {
        ARM64_USER_WINDOW *window = CONTAINING_RECORD(link, ARM64_USER_WINDOW, ZOrder);
        if (!window->Visible || !desktop_is_presented(window->Desktop)) continue;
        if (!hit_test || (pointer_x >= window->X && pointer_x < window->X + window->Width &&
                         pointer_y >= window->Y && pointer_y < window->Y + window->Height)) return window;
    }
    return NULL;
}

static VOID keyboard_event(USHORT code, LONG value)
{
    static const UCHAR vkeys[89] = {
        [1]=0x1b, [2]='1', [3]='2', [4]='3', [5]='4', [6]='5', [7]='6', [8]='7', [9]='8', [10]='9', [11]='0',
        [12]=0xbd, [13]=0xbb, [14]=0x08, [15]=0x09,
        [16]='Q', [17]='W', [18]='E', [19]='R', [20]='T', [21]='Y', [22]='U', [23]='I', [24]='O', [25]='P',
        [26]=0xdb, [27]=0xdd, [28]=0x0d, [29]=0x11,
        [30]='A', [31]='S', [32]='D', [33]='F', [34]='G', [35]='H', [36]='J', [37]='K', [38]='L',
        [39]=0xba, [40]=0xde, [41]=0xc0, [42]=0x10, [43]=0xdc,
        [44]='Z', [45]='X', [46]='C', [47]='V', [48]='B', [49]='N', [50]='M',
        [51]=0xbc, [52]=0xbe, [53]=0xbf, [54]=0x10, [56]=0x12, [57]=0x20, [58]=0x14,
        [59]=0x70, [60]=0x71, [61]=0x72, [62]=0x73, [63]=0x74, [64]=0x75, [65]=0x76, [66]=0x77,
        [67]=0x78, [68]=0x79, [87]=0x7a, [88]=0x7b
    };
    static const struct { UCHAR Key, Scan; } extended[16] = {
        [97-96]={0x11,0x1d}, [100-96]={0x12,0x38}, [102-96]={0x24,0x47},
        [103-96]={0x26,0x48}, [104-96]={0x21,0x49}, [105-96]={0x25,0x4b}, [106-96]={0x27,0x4d},
        [107-96]={0x23,0x4f}, [108-96]={0x28,0x50}, [109-96]={0x22,0x51},
        [110-96]={0x2d,0x52}, [111-96]={0x2e,0x53}
    };
    if (value < 0 || value > 2 || code >= (sizeof(key_down) / sizeof(key_down[0]))) return;
    ULONG key = 0, scan = 0;
    if (code < (sizeof(vkeys) / sizeof(vkeys[0]))) {
        key = vkeys[code];
        scan = code;
    } else if (code >= 96 && code - 96 < (sizeof(extended) / sizeof(extended[0]))) {
        key = extended[code - 96].Key;
        scan = 0x100 | extended[code - 96].Scan;
    }
    if (!key) return;
    BOOLEAN previous = key_down[code], alt = key_down[56] || key_down[100];
    key_down[code] = value != 0;
    ULONG lparam = 1 | (scan << 16) | (alt ? 1u << 29 : 0) |
                   (previous || value == 2 ? 1u << 30 : 0) | (value == 0 ? 1u << 31 : 0);
    ULONG message = alt || key == 0x12 || key == 0x79 ? USER_WM_SYSKEYDOWN : USER_WM_KEYDOWN;
    post_input(foreground_window(FALSE), USER_QS_KEY, message + (value == 0), key, lparam);
}

static VOID pointer_event(USHORT type, USHORT code, LONG value)
{
    const XP_ARM64_BOOT_DISPLAY *display = boot_display();
    if (type == 2 && code <= 1) {
        LONG *axis = code ? &pointer_y : &pointer_x;
        ULONG limit = code ? display->Height : display->Width;
        LONG updated = (LONG)max(0, min((LONGLONG)*axis + value, limit ? limit - 1 : 32767));
        pointer_moved |= *axis != updated;
        *axis = updated;
        return;
    }
    BOOLEAN button = type == 1 && code >= 0x110 && code <= 0x112 && (value == 0 || value == 1);
    if (!button && (type != 0 || code != 0 || !pointer_moved)) return;
    ARM64_USER_WINDOW *window = foreground_window(TRUE);
    ULONG message = USER_WM_MOUSEMOVE;
    if (button) {
        static const ULONG buttons[3] = {1, 2, 0x10};
        static const ULONG down[3] = {USER_WM_LBUTTONDOWN, USER_WM_RBUTTONDOWN, USER_WM_MBUTTONDOWN};
        ULONG index = code - 0x110;
        if (value) {
            pointer_buttons |= buttons[index];
            if (window) focused_window = window_handle(window);
        } else pointer_buttons &= ~buttons[index];
        message = down[index] + (value == 0);
    } else pointer_moved = FALSE;
    if (!window) return;
    ULONG lparam = (USHORT)(pointer_x - window->X) | (ULONG)(USHORT)(pointer_y - window->Y) << 16;
    ULONG modifiers = (key_down[42] || key_down[54] ? USER_MK_SHIFT : 0) |
                      (key_down[29] || key_down[97] ? USER_MK_CONTROL : 0);
    post_input(window, button ? USER_QS_MOUSEBUTTON : USER_QS_MOUSEMOVE,
               message, pointer_buttons | modifiers, lparam);
}

VOID ExpArm64RouteBootInput(BOOLEAN keyboard, USHORT type, USHORT code, LONG value)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    gdi_enter();
    if (!keyboard) pointer_event(type, code, value);
    else if (type == 1) keyboard_event(code, value);
    gdi_leave();
}
