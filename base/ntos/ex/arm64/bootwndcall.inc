/* The port carries HWND in the client callback record. The scalar callbacks
 * share XP's native field order and use the same result capture. */
typedef struct {
    ULONG_PTR Window;
    ULONG Message, Reserved;
    ULONG_PTR WParam, LParam, Extra, Procedure;
} ARM64_USER_WNDPROC_CALL;

typedef struct {
    ULONG_PTR Result;
    ULONG OutputBytes;
    ULONG_PTR Output;
} ARM64_USER_CALLBACK_STATUS;

_Static_assert(sizeof(ARM64_USER_WNDPROC_CALL) == 48 && FIELD_OFFSET(ARM64_USER_WNDPROC_CALL, Procedure) == 40,
               "native fnDWORD");
_Static_assert(sizeof(ARM64_USER_CALLBACK_STATUS) == 24, "native CALLBACKSTATUS");

/* Never hold gdi_lock across a user callback. The caller must revalidate
 * generation-tagged HWNDs before using any state obtained before the call. */
static BOOLEAN capture_window_callback(ULONG api, PVOID input, ULONG length, ARM64_USER_CALLBACK_STATUS *returned)
{
    PVOID output;
    ULONG bytes;
    NTSTATUS status = KeUserModeCallback(api, input, length, &output, &bytes);
    return NT_SUCCESS(status) && bytes == sizeof(*returned) &&
           NT_SUCCESS(copy_user_input((ULONG_PTR)output, returned, sizeof(*returned)));
}

static BOOLEAN call_window_callback(ULONG api, PVOID input, ULONG length, PULONG_PTR result)
{
    ARM64_USER_CALLBACK_STATUS returned;
    if (!capture_window_callback(api, input, length, &returned) || returned.OutputBytes || returned.Output)
        return FALSE;
    *result = returned.Result;
    return TRUE;
}

static BOOLEAN call_window_procedure(ULONG_PTR handle, ULONG_PTR procedure, ULONG message,
                                     ULONG_PTR wparam, ULONG_PTR lparam, PULONG_PTR result)
{
    ARM64_USER_WNDPROC_CALL call = {.Window = handle, .Message = message,
                                   .WParam = wparam, .LParam = lparam, .Procedure = procedure};
    return call_window_callback(XP_FI_FNDWORD, &call, sizeof(call), result);
}
