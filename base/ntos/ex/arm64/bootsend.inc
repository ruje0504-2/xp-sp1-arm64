#define USER_WM_SETTEXT 0x000cu
#define USER_WM_GETTEXT 0x000du
#define USER_WM_GETTEXTLENGTH 0x000eu
#define USER_EM_GETSEL 0x00b0u
#define USER_EM_SETSEL 0x00b1u
#define USER_EM_REPLACESEL 0x00c2u
#define USER_TEXT_CAPACITY 4096u

typedef struct {
    ARM64_USER_WNDPROC_CALL Header;
    WCHAR Text[];
} ARM64_USER_TEXT_CALL;

/* Capture only through the first terminating UTF-16 unit. Never probe the
 * following page until it is required to continue the string. */
static ULONG capture_text(ULONG_PTR address, WCHAR *text)
{
    if (address & (sizeof(WCHAR) - 1)) return 0;
    for (ULONG index = 0; index < USER_TEXT_CAPACITY;) {
        ULONG count = min(USER_TEXT_CAPACITY - index, (PAGE_SIZE - (address & (PAGE_SIZE - 1))) / sizeof(WCHAR));
        if (!NT_SUCCESS(copy_user_input(address, text + index, count * sizeof(WCHAR)))) return 0;
        for (ULONG end = index + count; index < end; ++index)
            if (!text[index]) return (index + 1) * sizeof(WCHAR);
        address += count * sizeof(WCHAR);
    }
    return 0;
}

static ULONG_PTR send_input_text(const ARM64_USER_WNDPROC_CALL *header, ULONG_PTR address)
{
    ULONG_PTR result = 0;
    if (!address) {
        ARM64_USER_WNDPROC_CALL call = *header;
        call_window_callback(XP_FI_FNINSTRINGNULL, &call, sizeof(call), &result);
        return result;
    }

    ULONG fixed = FIELD_OFFSET(ARM64_USER_TEXT_CALL, Text);
    ARM64_USER_TEXT_CALL *call = ExAllocatePoolWithTag(PagedPool, fixed + USER_TEXT_CAPACITY * sizeof(WCHAR), 'txUW');
    if (!call) return 0;
    ULONG bytes = capture_text(address, call->Text);
    if (bytes) {
        call->Header = *header;
        call_window_callback(XP_FI_FNINSTRINGNULL, call, fixed + bytes, &result);
    }
    ExFreePool(call);
    return result;
}

static ULONG_PTR send_output_text(const ARM64_USER_WNDPROC_CALL *header, ULONG_PTR destination)
{
    ULONG_PTR capacity = header->WParam;
    if (!capacity || capacity > USER_TEXT_CAPACITY || (destination & (sizeof(WCHAR) - 1))) return 0;
    ULONG maximum = (ULONG)capacity * sizeof(WCHAR);
    if (!user_output_writable(destination, maximum)) return 0;

    ULONG fixed = FIELD_OFFSET(ARM64_USER_TEXT_CALL, Text);
    ARM64_USER_TEXT_CALL *call = ExAllocatePoolWithTag(PagedPool, fixed + maximum, 'txUW');
    if (!call) return 0;
    call->Header = *header;
    RtlZeroMemory(call->Text, maximum);

    ULONG_PTR result = 0;
    ARM64_USER_CALLBACK_STATUS returned;
    if (capture_window_callback(XP_FI_FNOUTSTRING, call, fixed + maximum, &returned) &&
        returned.Result < capacity && returned.OutputBytes <= maximum &&
        !(returned.OutputBytes & (sizeof(WCHAR) - 1)) &&
        (!!returned.OutputBytes == !!returned.Output) &&
        !(returned.Output & (sizeof(WCHAR) - 1))) {
        ULONG bytes = (ULONG)returned.Result * sizeof(WCHAR);
        if (!bytes || (returned.OutputBytes >= bytes &&
                       NT_SUCCESS(copy_user_input(returned.Output, call->Text, bytes)))) {
            call->Text[returned.Result] = 0;
            if (NT_SUCCESS(copy_user_buffer(destination, call->Text, bytes + sizeof(WCHAR), TRUE)))
                result = returned.Result;
        }
    }
    ExFreePool(call);
    return result;
}

static ULONG_PTR user_message_call(WIN32_ARGS)
{
    if (e || f != XP_FNID_SENDMESSAGE || g || b > 0xffff) return 0;
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    ULONG_PTR procedure = window ? window->Class->Procedure : 0;
    ULONG handle = window ? window_handle(window) : 0;
    gdi_leave();
    if (!procedure) return 0;

    ARM64_USER_WNDPROC_CALL call = {.Window = handle, .Message = (ULONG)b, .WParam = c,
                                   .Procedure = procedure};
    ULONG_PTR result = 0;
    switch ((ULONG)b) {
    case USER_WM_SETTEXT:
    case USER_EM_REPLACESEL:
        return send_input_text(&call, d);
    case USER_WM_GETTEXT:
        return send_output_text(&call, d);
    case USER_WM_GETTEXTLENGTH:
        call_window_callback(XP_FI_FNGETTEXTLENGTHS, &call, sizeof(call), &result);
        return result;
    default:
        if (b == USER_EM_GETSEL && (c || d)) return 0;
        if (b != USER_EM_SETSEL && b != USER_EM_GETSEL && !user_scalar_message((ULONG)b, d)) return 0;
        if (b == USER_WM_PAINT)
            return !c && !d && dispatch_paint(handle, FALSE, &result) ? result : 0;
        call.LParam = d;
        call_window_callback(XP_FI_FNDWORD, &call, sizeof(call), &result);
        return result;
    }
}

static ULONG_PTR user_default_set_text(WIN32_ARGS)
{
    WCHAR text[USER_NAME_CAPACITY] = {0};
    UNICODE_STRING name = {0};
    if (b && !large_window_name(b, text, &name)) return FALSE;
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    if (window) RtlCopyMemory(window->Title, text, name.Length + sizeof(WCHAR));
    gdi_leave();
    return window != NULL;
}

static ULONG_PTR user_internal_get_window_text(WIN32_ARGS)
{
    LONG capacity = (LONG)c;
    if (capacity <= 0 || (b & (sizeof(WCHAR) - 1))) return 0;
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    LONG count = 0;
    if (window) {
        while (count < capacity - 1 && window->Title[count]) ++count;
        WCHAR text[USER_NAME_CAPACITY];
        RtlCopyMemory(text, window->Title, count * sizeof(WCHAR));
        text[count] = 0;
        if (!NT_SUCCESS(copy_user_buffer(b, text, (count + 1) * sizeof(WCHAR), TRUE))) count = 0;
    }
    gdi_leave();
    return count;
}
