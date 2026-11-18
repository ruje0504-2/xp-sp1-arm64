#include "edit.h"
#include "xp-win32-services.h"

typedef unsigned long long ULONG_PTR;
typedef unsigned UINT;
typedef unsigned short WCHAR;
typedef struct { int Left, Top, Right, Bottom; } RECT;

typedef struct {
    ULONG_PTR Dc;
    int Erase;
    RECT Region;
    int Restore, Update;
    unsigned char Reserved[32];
} PAINT;
typedef struct {
    ULONG_PTR Params, Module, Menu, Parent;
    int Height, Width, Y, X, Style;
    const WCHAR *Name, *Class;
    UINT ExStyle;
} CREATE;
typedef struct { unsigned short Length, Capacity; const WCHAR *Buffer; } UNICODE;
typedef struct {
    UINT Size, Style;
    ULONG_PTR Procedure;
    int ClassExtra, WindowExtra;
    ULONG_PTR Module, Icon, Cursor, Background, MenuName;
    const WCHAR *ClassName;
    ULONG_PTR SmallIcon;
} WNDCLASS;

_Static_assert(sizeof(PAINT) == 72 && sizeof(CREATE) == 80 && sizeof(WNDCLASS) == 80, "USER ABI");

int XpUserInitializeClient(const ULONG_PTR *, const ULONG_PTR *, const ULONG_PTR *, ULONG_PTR);
UINT XpUserRegisterClass(const WNDCLASS *, const UNICODE *, const UNICODE *, const void *, UINT, UINT, void *);
int XpUserSetWindowFnid(ULONG_PTR, UINT);
int XpUserInvalidate(ULONG_PTR, const RECT *, int);
ULONG_PTR XpUserBeginPaint(ULONG_PTR, PAINT *);
int XpUserEndPaint(ULONG_PTR, const PAINT *);
int XpUserDrawText(ULONG_PTR, int, int, UINT, const RECT *, const WCHAR *, int, const int *, UINT);

enum {
    NcCreate = 0x81, NcDestroy = 0x82, SetFocus = 7, KillFocus = 8, SetText = 12, GetText = 13,
    GetTextLength = 14, Paint = 15, KeyDown = 0x100, Character = 0x102, LeftButtonDown = 0x201,
    GetSel = 0xb0, SetSel = 0xb1, ReplaceSel = 0xc2
};
enum { MultiLine = 4, ReadOnly = 0x800, WantReturn = 0x1000, TextCapacity = 256 };

typedef struct {
    ULONG_PTR Window;
    UINT Style, Count, Anchor, Caret, LeftColumn;
    int Width, Height, Focus;
    WCHAR Text[TextCapacity];
} EDIT;

/* A HWND slot has one live owner. The generation check rejects old handles.
 * A reused slot is initialized even after direct thread rundown. */
static EDIT edits[64];
static const WCHAR edit_name[] = L"Edit";
const XP_USER32_LARGE_STRING XpUser32EditClass = {8, 10, edit_name};

UINT XpUser32InitializeEdit(ULONG_PTR module)
{
    ULONG_PTR callbacks[XP_PFNCLIENT_COUNT] = {0};
    callbacks[XP_PFNCLIENT_EDIT] = (ULONG_PTR)XpUser32EditWndProc;
    if (XpUserInitializeClient(0, callbacks, 0, module) < 0) return 0;
    const UNICODE name = {8, 10, edit_name}, empty = {0};
    const void *menu[3] = {0, 0, &empty};
    const WNDCLASS cls = {sizeof(cls), 0x4088, (ULONG_PTR)XpUser32EditWndProc,
                         0, sizeof(ULONG_PTR), module, 0, 0, 0, 0, edit_name, 0};
    return XpUserRegisterClass(&cls, &name, &name, menu, XP_FNID_EDIT, 0x80, 0);
}

static UINT minimum(UINT a, UINT b)
{
    return a < b ? a : b;
}

static UINT maximum(UINT a, UINT b)
{
    return a > b ? a : b;
}

static int replace(EDIT *edit, UINT first, UINT last, const WCHAR *text, UINT limit)
{
    UINT count = 0, old = last - first;
    while (text && count < limit && text[count]) {
        if (count == TextCapacity - (edit->Count - old)) return 0;
        if (!(edit->Style & MultiLine) && (text[count] == '\r' || text[count] == '\n')) break;
        ++count;
    }
    if (count > old) {
        for (UINT index = edit->Count; index > last; --index)
            edit->Text[index - 1 + count - old] = edit->Text[index - 1];
    } else {
        for (UINT index = last; index < edit->Count; ++index)
            edit->Text[index - old + count] = edit->Text[index];
    }
    for (UINT index = 0; index < count; ++index) edit->Text[first + index] = text[index];
    edit->Count += count;
    edit->Count -= old;
    edit->Anchor = edit->Caret = first + count;
    XpUserInvalidate(edit->Window, 0, 0);
    return 1;
}

static UINT line_end(const EDIT *edit, UINT start)
{
    while (start < edit->Count && edit->Text[start] != '\r' && edit->Text[start] != '\n') ++start;
    return start;
}

static UINT next_line(const EDIT *edit, UINT end)
{
    return end + 1 + (edit->Text[end] == '\r' && end + 1 < edit->Count && edit->Text[end + 1] == '\n');
}

static void paint_edit(EDIT *edit)
{
    PAINT paint;
    ULONG_PTR dc = XpUserBeginPaint(edit->Window, &paint);
    if (!dc) return;
    RECT client = {0, 0, edit->Width, edit->Height};
    XpUserDrawText(dc, 0, 0, 2, &client, 0, 0, 0, 0);
    UINT columns = (UINT)maximum(1, edit->Width > 4 ? (edit->Width - 4) / 8 : 0);
    if (!(edit->Style & MultiLine)) {
        if (edit->Caret < edit->LeftColumn) edit->LeftColumn = edit->Caret;
        if (edit->Caret >= edit->LeftColumn + columns) edit->LeftColumn = edit->Caret - columns + 1;
    }

    int x_cursor = -1, y_cursor = 0;
    for (UINT start = 0, row = 0; start <= edit->Count && 2 + 10 * (int)row < edit->Height; ++row) {
        UINT end = line_end(edit, start);
        UINT first = minimum(end, start + edit->LeftColumn);
        int y = 2 + 10 * (int)row;
        XpUserDrawText(dc, 2, y, 4, &client, edit->Text + first,
                       (int)minimum(end - first, columns), 0, 0);
        if (edit->Caret >= first && edit->Caret <= end) {
            x_cursor = 2 + 8 * (int)(edit->Caret - first);
            y_cursor = y + ((edit->Style & MultiLine) ? 6 : 10);
        }
        if (end == edit->Count) break;
        start = next_line(edit, end);
    }
    if (edit->Focus && x_cursor >= 0 && x_cursor < edit->Width && y_cursor < edit->Height) {
        const WCHAR bar = '|';
        RECT caret = {x_cursor, y_cursor, x_cursor + 1, y_cursor + 4};
        XpUserDrawText(dc, x_cursor - 3, y_cursor, 4, &caret, &bar, 1, 0, 0);
    }
    XpUserEndPaint(edit->Window, &paint);
}

static void edit_key(EDIT *edit, UINT vk)
{
    UINT to = edit->Caret;
    switch (vk) {
    case 0x25:
        to = minimum(edit->Caret, edit->Anchor);
        if (edit->Caret == edit->Anchor && to) {
            --to;
            if (to && edit->Text[to] == '\n' && edit->Text[to - 1] == '\r') --to;
        }
        break;
    case 0x27:
        to = maximum(edit->Caret, edit->Anchor);
        if (edit->Caret == edit->Anchor && to < edit->Count) {
            ++to;
            if (to < edit->Count && edit->Text[to] == '\n' && edit->Text[to - 1] == '\r') ++to;
        }
        break;
    case 0x24:
        while (to && edit->Text[to - 1] != '\r' && edit->Text[to - 1] != '\n') --to;
        break;
    case 0x23:
        to = line_end(edit, to);
        break;
    case 0x2e: {
        if (edit->Style & ReadOnly) return;
        UINT first = minimum(edit->Anchor, to), last = maximum(edit->Anchor, to);
        if (last == first && last < edit->Count) {
            ++last;
            if (last < edit->Count && edit->Text[last - 1] == '\r' && edit->Text[last] == '\n') ++last;
        }
        replace(edit, first, last, 0, 0);
        return;
    }
    default:
        return;
    }
    edit->Anchor = edit->Caret = to;
    XpUserInvalidate(edit->Window, 0, 0);
}

static void edit_character(EDIT *edit, WCHAR character)
{
    if (edit->Style & ReadOnly) return;
    UINT first = minimum(edit->Anchor, edit->Caret), last = maximum(edit->Anchor, edit->Caret);
    if (character == 8) {
        if (first == last && first) {
            --first;
            if (first && edit->Text[first] == '\n' && edit->Text[first - 1] == '\r') --first;
        }
        replace(edit, first, last, 0, 0);
    } else if (character == 13 && (edit->Style & MultiLine)) {
        const WCHAR pair[] = {'\r', '\n'};
        replace(edit, first, last, pair, 2);
    } else if (character >= 32 && character != 127) {
        replace(edit, first, last, &character, 1);
    }
}

ULONG_PTR XpUser32EditWndProc(ULONG_PTR window, UINT message, ULONG_PTR wparam, ULONG_PTR lparam)
{
    UINT slot = ((UINT)window & 0xffff) - 0x100;
    if (slot >= sizeof(edits) / sizeof(edits[0])) return 0;
    EDIT *edit = &edits[slot];
    if (message == NcCreate) {
        const CREATE *cs = (const CREATE *)lparam;
        if (!cs || !XpUserSetWindowFnid(window, XP_FNID_EDIT)) return 0;
        edit->Window = window;
        edit->Style = cs->Style;
        edit->Width = cs->Width;
        edit->Height = cs->Height;
        edit->Count = edit->Anchor = edit->Caret = edit->LeftColumn = edit->Focus = 0;
        return replace(edit, 0, 0, cs->Name, TextCapacity + 1);
    }
    if (edit->Window != window) return 0;
    switch (message) {
    case NcDestroy:
        XpUserSetWindowFnid(window, XP_FNID_CLEANEDUP_BIT);
        edit->Window = 0;
        break;
    case SetFocus:
    case KillFocus:
        edit->Focus = message == SetFocus;
        XpUserInvalidate(window, 0, 0);
        break;
    case Paint:
        paint_edit(edit);
        break;
    case SetText:
        if (!replace(edit, 0, edit->Count, (const WCHAR *)lparam, TextCapacity + 1)) return 0;
        edit->Anchor = edit->Caret = edit->LeftColumn = 0;
        return 1;
    case GetText: {
        if (!wparam || !lparam) return 0;
        WCHAR *output = (WCHAR *)lparam;
        UINT count = (UINT)minimum(edit->Count, (UINT)wparam - 1);
        for (UINT i = 0; i < count; ++i) output[i] = edit->Text[i];
        output[count] = 0;
        return count;
    }
    case GetTextLength:
        return edit->Count;
    case GetSel:
        return minimum(edit->Anchor, edit->Caret) | (maximum(edit->Anchor, edit->Caret) << 16);
    case SetSel:
        if ((int)wparam == -1) edit->Anchor = edit->Caret;
        else {
            edit->Anchor = minimum((UINT)wparam, edit->Count);
            edit->Caret = minimum((UINT)lparam, edit->Count);
        }
        XpUserInvalidate(window, 0, 0);
        break;
    case ReplaceSel:
        if (!(edit->Style & ReadOnly))
            replace(edit, minimum(edit->Anchor, edit->Caret), maximum(edit->Anchor, edit->Caret),
                    (const WCHAR *)lparam, TextCapacity + 1);
        break;
    case KeyDown:
        edit_key(edit, (UINT)wparam);
        break;
    case Character:
        edit_character(edit, (WCHAR)wparam);
        break;
    case LeftButtonDown: {
        UINT start = 0, end = line_end(edit, 0);
        int x = (short)lparam, y = (short)(lparam >> 16);
        for (int row = 2; row + 10 <= y && end < edit->Count; row += 10) {
            start = next_line(edit, end);
            end = line_end(edit, start);
        }
        edit->Anchor = edit->Caret = minimum(end, start + edit->LeftColumn + (x > 2 ? (x + 2) / 8 : 0));
        XpUserInvalidate(window, 0, 0);
        break;
    }
    }
    return 0;
}
