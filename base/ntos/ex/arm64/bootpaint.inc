/* Client-only update regions and the paint portion of the USER queue. */
#define USER_WM_PAINT 0x000fu
#define USER_WM_ERASEBKGND 0x0014u

typedef struct {
    ULONG_PTR Dc;
    LONG Erase;
    ARM64_USER_RECT Rect;
    LONG Restore, IncUpdate;
    UCHAR Reserved[32];
} ARM64_USER_PAINT;

_Static_assert(sizeof(ARM64_USER_PAINT) == 72 && FIELD_OFFSET(ARM64_USER_PAINT, Rect) == 12,
               "native PAINTSTRUCT");

static ULONG_PTR user_invalidate_rect(WIN32_ARGS)
{
    ARM64_USER_RECT rect;
    if (b && !NT_SUCCESS(copy_user_input(b, &rect, sizeof(rect)))) return FALSE;
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    if (window) invalidate_client(window, b ? &rect : NULL, (ULONG)c != 0);
    gdi_leave();
    return window != NULL;
}

static ULONG_PTR user_get_update_rect(WIN32_ARGS)
{
    if (c) return FALSE;
    gdi_enter();
    const ARM64_USER_WINDOW *window = window_lookup(a);
    BOOLEAN pending = window && !empty_rect(&window->Update.Bounds);
    if (window && b) {
        ARM64_USER_RECT bounds = window->Update.Bounds;
        pending = NT_SUCCESS(copy_user_buffer(b, &bounds, sizeof(bounds), TRUE)) && pending;
    }
    gdi_leave();
    return pending;
}

static VOID release_paint_dc(ARM64_USER_WINDOW *window)
{
    gdi_free(gdi_lookup(window->PaintDc, GdiMemoryDc));
    window->PaintDc = 0;
    RtlClearAllBits(&window->Painting.Bits);
    window->Painting.Bounds = (ARM64_USER_RECT){0};
    window->Painting.Erase = FALSE;
}

static VOID restore_paint_update(ARM64_USER_WINDOW *window)
{
    ARM64_PAINT_REGION *update = &window->Update, *paint = &window->Painting;
    if (!empty_rect(&paint->Bounds)) {
        for (ULONG i = 0; i < (paint->Bits.SizeOfBitMap + 31) / 32; ++i)
            update->Bits.Buffer[i] |= paint->Bits.Buffer[i];
        union_bounds(&update->Bounds, &paint->Bounds);
        update->Erase |= paint->Erase;
    }
    release_paint_dc(window);
}

static ULONG_PTR user_begin_paint(WIN32_ARGS)
{
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    ULONG dc = window && !window->PaintDc ?
               acquire_user_dc(window->Owner, window->Desktop, window, FALSE, TRUE) : 0;
    if (!dc) {
        gdi_leave();
        return 0;
    }

    ARM64_USER_PAINT paint;
    RtlZeroMemory(&paint, sizeof(paint));
    paint.Dc = dc;
    paint.Erase = window->Update.Erase;
    paint.Rect = window->Update.Bounds;
    if (!NT_SUCCESS(copy_user_buffer(b, &paint, sizeof(paint), TRUE))) {
        gdi_free(gdi_lookup(dc, GdiMemoryDc));
        gdi_leave();
        return 0;
    }

    /* Snapshot and validate before calling client code. New invalidations
     * from an erase handler belong to the next paint. */
    RTL_BITMAP empty = window->Painting.Bits;
    window->Painting = window->Update;
    window->Update = (ARM64_PAINT_REGION){.Bits = empty};
    window->PaintDc = dc;
    if (!paint.Erase) {
        gdi_leave();
        return dc;
    }
    window->BeginningPaint = TRUE;
    ULONG_PTR procedure = window->Class->Procedure;
    gdi_leave();

    ULONG_PTR erased = 0;
    BOOLEAN handled = call_window_procedure(a, procedure, USER_WM_ERASEBKGND, dc, 0, &erased);
    gdi_enter();
    window = window_lookup(a);
    if (!window || window->PaintDc != dc) dc = 0;
    else {
        window->BeginningPaint = FALSE;
        paint.Erase = !handled || !erased;
        if (!NT_SUCCESS(copy_user_buffer(b, &paint, sizeof(paint), TRUE))) {
            restore_paint_update(window);
            dc = 0;
        }
    }
    gdi_leave();
    return dc;
}

static ULONG_PTR user_end_paint(WIN32_ARGS)
{
    ARM64_USER_PAINT paint;
    if (!NT_SUCCESS(copy_user_input(b, &paint, sizeof(paint)))) return FALSE;
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(a);
    BOOLEAN active = window && window->PaintDc && window->PaintDc == paint.Dc && !window->BeginningPaint;
    if (active) release_paint_dc(window);
    gdi_leave();
    return active;
}

static BOOLEAN has_paint_message(const ARM64_USER_WINDOW *window)
{
    ARM64_USER_RECT clip;
    return !window->PaintDc && !window->DispatchingPaint && window_screen_clip(window, &clip) &&
           !empty_rect(&window->Update.Bounds);
}

static BOOLEAN dispatch_paint(ULONG_PTR hwnd, BOOLEAN require_update, PULONG_PTR result)
{
    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(hwnd);
    if (!window || window->DispatchingPaint || window->PaintDc) {
        gdi_leave();
        return FALSE;
    }
    if (require_update && !has_paint_message(window)) {
        gdi_leave();
        return TRUE;
    }
    window->DispatchingPaint = TRUE;
    ULONG_PTR procedure = window->Class->Procedure;
    gdi_leave();
    BOOLEAN handled = call_window_procedure(hwnd, procedure, USER_WM_PAINT, 0, 0, result);

    gdi_enter();
    window = window_lookup(hwnd);
    if (window) {
        if (!handled && window->PaintDc) restore_paint_update(window);
        window->DispatchingPaint = FALSE;
    }
    gdi_leave();
    return handled;
}

static ULONG_PTR user_call_hwnd_lock(WIN32_ARGS)
{
    if (b != XP_SFI_UPDATEWINDOW) return FALSE;
    ULONG_PTR unused;
    return dispatch_paint(a, TRUE, &unused);
}
