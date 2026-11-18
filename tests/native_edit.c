/* Native EL0 test application. The editable buffer belongs to this WndProc. */
#include "xp-win32-services.h"
#include "../base/win32/user32/arm64/edit.h"

typedef unsigned long long U64;
typedef unsigned UINT;
typedef unsigned short WCHAR;
typedef struct { int Left, Top, Right, Bottom; } RECT;
typedef struct { int Width, Height; } SIZE;
typedef struct {
    U64 Window;
    UINT Message;
    U64 WParam, LParam;
    UINT Time;
    int X, Y;
} MSG;
typedef struct {
    U64 Dc;
    int Erase;
    RECT Region;
    int Restore, Update;
    unsigned char Reserved[32];
} PAINT;

_Static_assert(sizeof(MSG) == 48 && sizeof(PAINT) == 72, "native USER ABI");

int XpEditGetMessage(MSG *, U64, UINT, UINT);
int XpEditTranslate(const MSG *, UINT);
U64 XpEditDispatch(const MSG *);
U64 XpEditBeginPaint(U64, PAINT *);
int XpEditEndPaint(U64, const PAINT *);
int XpEditInvalidate(U64, const RECT *, int);
int XpEditCallHwndLock(U64, UINT);
int XpEditTextOut(U64, int, int, UINT, const RECT *, const WCHAR *, int, const int *, UINT);
int XpEditExtent(U64, const WCHAR *, int, SIZE *, UINT);
UINT XpEditPixel(U64, int, int);
U64 XpWindowGetThreadState(UINT);
U64 XpNativeCreateEditHost(U64 *);
int XpWindowDestroy(U64);
int XpWindowPost(U64, UINT, U64, U64);
U64 XpEditMessage(U64, UINT, U64, U64, U64, UINT, UINT);
int XpWindowDefSetText(U64, void *);
UINT XpWindowInternalText(U64, WCHAR *, int);

enum { SetText = 0x0c, GetText = 0x0d, GetTextLength = 0x0e, SetSelection = 0xb1,
       ReplaceSelection = 0xc2, Paint = 0x0f, KeyDown = 0x100, KeyUp = 0x101, Character = 0x102, Click = 0x201 };

U64 XpEditWindow;
static WCHAR buffer[16];
static UINT length, caret, anchor, characters, paint_count;
static UINT text_callbacks, nested_length;
static int check_text_callbacks, malformed_output;
static SIZE extent;

static U64 send_text(U64 window, UINT message, U64 wparam, const void *lparam)
{
    return XpEditMessage(window, message, wparam, (U64)lparam, 0, XP_FNID_SENDMESSAGE, 0);
}

static UINT replace_text(const WCHAR *source)
{
    UINT first = caret < anchor ? caret : anchor;
    UINT last = caret > anchor ? caret : anchor;
    UINT count = 0, capacity = sizeof(buffer) / sizeof(buffer[0]);
    while (source && source[count]) {
        if (count == capacity - (length - (last - first))) return 0;
        ++count;
    }

    if (count > last - first) {
        for (UINT i = length; i > last; --i) buffer[first + count + i - last - 1] = buffer[i - 1];
    } else {
        for (UINT i = last; i < length; ++i) buffer[first + count + i - last] = buffer[i];
    }
    for (UINT i = 0; i < count; ++i) buffer[first + i] = source[i];
    length = length - (last - first) + count;
    anchor = caret = first + count;
    return 1;
}

static U64 draw_edit(U64 window)
{
    PAINT paint;
    U64 dc = XpEditBeginPaint(window, &paint);
    if (!dc) return 0;
    RECT client = {0, 0, 64, 18};
    RECT cursor = {2 + 8 * (int)caret, 12, 3 + 8 * (int)caret, 16};
    const WCHAR bar = '|';
    SIZE legacy, empty;
    int valid = XpEditExtent(0, 0, 0, &empty, 0) && !empty.Width && !empty.Height &&
                XpEditTextOut(0, 0, 0, 0, 0, 0, 0, 0, 0) &&
                !XpEditTextOut(dc, 2, 2, 6, &client, (const WCHAR *)1, 1, 0, 0) &&
                !XpEditExtent(dc, buffer, -1, &extent, 0) &&
                !XpEditExtent(dc, buffer, (int)length, &extent, 2) &&
                XpEditExtent(dc, buffer, (int)length, &extent, 0) &&
                XpEditExtent(dc, buffer, (int)length, &legacy, 1) &&
                legacy.Width == extent.Width && legacy.Height == extent.Height &&
                XpEditTextOut(dc, 2, 2, 6, &client, buffer, (int)length, 0, 0) &&
                XpEditTextOut(dc, cursor.Left - 3, cursor.Top, 4, &cursor, &bar, 1, 0, 0) &&
                XpEditPixel(dc, 1, 2) == 0xffffff;
    if (!XpEditEndPaint(window, &paint) || !valid) return 0;
    ++paint_count;
    return 1;
}

U64 XpNativeEditProcedure(U64 window, UINT message, U64 wparam, U64 lparam)
{
    if (message == GetTextLength) return length;
    if (message == GetText) {
        ++text_callbacks;
        if (malformed_output) {
            ((WCHAR *)lparam)[0] = '!';
            return wparam;
        }
        if (!wparam) return 0;
        WCHAR *text = (WCHAR *)lparam;
        UINT count = length < wparam - 1 ? length : (UINT)wparam - 1;
        for (UINT i = 0; i < count; ++i) text[i] = buffer[i];
        text[count] = 0;
        return count;
    }
    if (message == SetSelection) {
        anchor = (int)wparam < 0 ? caret : wparam > length ? length : (UINT)wparam;
        caret = (int)wparam < 0 ? caret : lparam > length ? length : (UINT)lparam;
        return 0;
    }
    if (message == SetText || message == ReplaceSelection) {
        ++text_callbacks;
        if (message == SetText) { anchor = 0; caret = length; }
        if (!replace_text((const WCHAR *)lparam)) return 0;
        if (check_text_callbacks && lparam) {
            nested_length = (UINT)send_text(window, GetTextLength, 0, 0);
            if (*(WCHAR *)lparam) *(WCHAR *)lparam = '!';
        }
        return message == SetText;
    }
    if (message == Paint) return draw_edit(window);
    if (message == Click) return 1;
    if (message == KeyDown && wparam == 0x25 && caret) {
        --caret;
    } else if (message == Character) {
        ++characters;
        if (wparam == 8 && caret) {
            --caret;
            --length;
            for (UINT i = caret; i < length; ++i) buffer[i] = buffer[i + 1];
        } else if (wparam >= 0x20 && wparam < 0x7f && length < sizeof(buffer) / sizeof(buffer[0])) {
            for (UINT i = length; i > caret; --i) buffer[i] = buffer[i - 1];
            buffer[caret++] = (WCHAR)wparam;
            ++length;
        }
    }
    anchor = caret;
    return 1;
}

U64 XpNativeCreateStockEditHost(U64 *);
U64 XpWindowGetDc(U64);
int XpWindowCallOne(U64, UINT);
U64 XpWindowCreate(UINT, U64, U64, const XP_USER32_LARGE_STRING *, UINT, int, int, int, int,
                  U64, U64, U64, U64, UINT, U64);

enum { GetSelection = 0xb0 };

static int receive_key(U64 window, MSG *msg)
{
    if (XpEditGetMessage(msg, window, KeyDown, 0x106) != 1) return 0;
    int is_key = msg->Message == KeyDown || msg->Message == KeyUp ||
                 msg->Message == 0x104 || msg->Message == 0x105;
    if (XpEditTranslate(msg, 0) != is_key) return 0;
    XpEditDispatch(msg);
    return 1;
}

int XpNativeStockEditProbe(U64 window, volatile UINT *evidence)
{
    MSG msg;
    *evidence = 4;
    if (XpEditGetMessage(&msg, window, Click, Click) != 1 || msg.LParam != 0x00080006 ||
        XpWindowGetThreadState(XP_THREAD_STATE_FOCUS) != window) return __LINE__;
    XpEditDispatch(&msg);

    WCHAR edited[3];
    *evidence = 5;
    for (UINT chars = 0; chars < 2; chars += msg.Message == Character)
        if (!receive_key(window, &msg)) return __LINE__;
    if (send_text(window, GetText, 3, edited) != 2 || edited[0] != 'a' || edited[1] != 'B' ||
        send_text(window, GetSelection, 0, 0) != 0x20002) return __LINE__;

    *evidence = 6;
    for (UINT chars = 0; chars < 2; chars += msg.Message == Character)
        if (!receive_key(window, &msg)) return __LINE__;
    if (send_text(window, GetText, 3, edited) != 2 || edited[0] != 'c' || edited[1] != 'B' || edited[2] ||
        send_text(window, GetSelection, 0, 0) != 0x10001) return __LINE__;
    U64 dc = XpWindowGetDc(window);
    SIZE current, legacy;
    if (!dc || !XpEditExtent(dc, edited, 2, &current, 0) || !XpEditExtent(dc, edited, 2, &legacy, 1) ||
        current.Width != 16 || current.Height != 8 || legacy.Width != 16 || legacy.Height != 8 ||
        !XpWindowCallOne(dc, XP_SFI_RELEASEDC) || !XpEditCallHwndLock(window, XP_SFI_UPDATEWINDOW))
        return __LINE__;
    evidence[4] = edited[0] | (UINT)edited[1] << 16;
    evidence[5] = 1;
    evidence[6] = current.Width;
    evidence[7] = current.Height;
    *evidence = 7;
    do {
        if (!receive_key(window, &msg)) return __LINE__;
    } while (msg.Message != Character || msg.WParam != 13);
    return 0;
}

UINT XpNativeEditClassProbe(U64 evidence[4])
{
    const U64 module = 0x400000;
    UINT atom = XpUser32InitializeEdit(module);
    if (!atom || XpUser32InitializeEdit(module + 4096)) return __LINE__;

    U64 single;
    U64 host = XpNativeCreateStockEditHost(&single);
    if (!host || !single) return __LINE__;
    const WCHAR initial[] = L"A";
    const XP_USER32_LARGE_STRING title = {2, 4, initial};
    U64 multi = XpWindowCreate(0, atom, atom, &title, 0x50001004u, 4, 4, 64, 24,
                              host, 104, 0, 0, 0, 0);
    if (!multi || send_text(multi, GetTextLength, 0, 0) != 1 ||
        send_text(single, GetTextLength, 0, 0)) return __LINE__;
    const WCHAR distinct[] = L"independent";
    if (!send_text(single, SetText, 0, distinct)) return __LINE__;
    XpEditMessage(multi, SetSelection, 1, 1, 0, XP_FNID_SENDMESSAGE, 0);
    XpEditMessage(multi, Character, 13, 1, 0, XP_FNID_SENDMESSAGE, 0);
    XpEditMessage(multi, Character, 'B', 1, 0, XP_FNID_SENDMESSAGE, 0);
    WCHAR text[12];
    if (send_text(multi, GetText, 12, text) != 4 ||
        text[0] != 'A' || text[1] != 13 || text[2] != 10 || text[3] != 'B' || text[4]) return __LINE__;
    evidence[1] = text[0] | (UINT)text[1] << 8 | (UINT)text[2] << 16 | (UINT)text[3] << 24;
    if (send_text(single, GetText, 12, text) != 11 || text[0] != 'i' || text[10] != 't' || text[11])
        return __LINE__;
    evidence[0] = atom;
    evidence[2] = single;
    evidence[3] = multi;
    if (!XpWindowDestroy(host) || send_text(multi, GetTextLength, 0, 0) ||
        send_text(single, GetTextLength, 0, 0)) return __LINE__;
    return 0;
}

UINT XpNativeSyncTextProbe(U64 evidence[4], WCHAR *edge)
{
    U64 child;
    U64 host = XpNativeCreateEditHost(&child);
    if (!host || !child) return __LINE__;
    XpEditWindow = child;

    WCHAR source[] = {'p', 0x03a9, 'q', 0};
    WCHAR output[] = {'?', '?', '?', '?', '?', '?'};
    static const WCHAR insert[] = {'X', 'Y', 0};
    check_text_callbacks = 1;
    if (!send_text(child, SetText, 0, source) || source[0] != 'p' ||
        nested_length != 3 || send_text(child, GetTextLength, 0, 0) != 3 ||
        send_text(child, GetText, 3, output) != 2 ||
        output[0] != 'p' || output[1] != 0x03a9 || output[2] || output[3] != '?')
        return __LINE__;
    if (!XpEditCallHwndLock(child, XP_SFI_UPDATEWINDOW) ||
        paint_count != 1 || extent.Width != 24 || extent.Height != 8) return __LINE__;
    evidence[0] = output[0] | (UINT)output[1] << 16;
    evidence[1] = nested_length;
    check_text_callbacks = 0;

    XpEditMessage(child, SetSelection, 1, 2, 0, XP_FNID_SENDMESSAGE, 0);
    send_text(child, ReplaceSelection, 1, insert);
    if (send_text(child, GetText, 6, output) != 4 ||
        output[0] != 'p' || output[1] != 'X' || output[2] != 'Y' || output[3] != 'q' || output[4])
        return __LINE__;
    evidence[2] = output[0] | (UINT)output[1] << 8 | (UINT)output[2] << 16 | (UINT)output[3] << 24;

    UINT before = text_callbacks;
    if (send_text(child, GetText, 6, insert) || send_text(child, GetText, 4097, output) ||
        send_text(child, GetText, 0, output) || send_text(child, SetText, 0, (const void *)1) ||
        XpWindowPost(child, SetText, 0, (U64)source) || text_callbacks != before) return __LINE__;

    malformed_output = 1;
    if (send_text(child, GetText, 6, output) || output[0] != 'p' || output[3] != 'q' ||
        text_callbacks != before + 1) return __LINE__;
    malformed_output = 0;

    *edge = 'v';
    if (send_text(child, SetText, 0, edge) || send_text(child, GetTextLength, 0, 0) != 4)
        return __LINE__;
    *edge = 0;
    if (!send_text(child, SetText, 0, edge) || send_text(child, GetTextLength, 0, 0) ||
        send_text(child, GetText, 6, output) || output[0] || output[1] != 'X')
        return __LINE__;
    if (!send_text(child, SetText, 0, 0)) return __LINE__;

    struct { UINT Length, Capacity; const WCHAR *Buffer; } caption = {6, sizeof(source), source};
    if (!XpWindowDefSetText(host, &caption) || XpWindowInternalText(host, output, 3) != 2 ||
        output[0] != 'p' || output[1] != 0x03a9 || output[2] ||
        !XpWindowDefSetText(host, 0) || XpWindowInternalText(host, output, 6) || output[0] ||
        !XpWindowDestroy(host) || send_text(child, SetText, 0, source)) return __LINE__;
    evidence[3] = text_callbacks;
    XpEditWindow = 0;
    return 0;
}
