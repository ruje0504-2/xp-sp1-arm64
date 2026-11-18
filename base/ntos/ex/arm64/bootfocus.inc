#define USER_WM_SETFOCUS 0x0007u
#define USER_WM_KILLFOCUS 0x0008u

/* All references are under gdi_lock. Thread rundown clears its foreground
 * pointer before releasing the queue and revokes outstanding HWND tickets. */
static ARM64_USER_THREAD *foreground_queue;
static ULONG pointer_focus, pointer_focus_ticket, pointer_focus_sequence;

/* Called after hiding the subtree but before removing any HWNDs. */
static VOID window_focus_loss(const ARM64_USER_WINDOW *window, ULONG *lost, ULONG *fallback)
{
    ARM64_USER_THREAD *user = window->Owner->Win32Thread;
    if (window_subtree_contains(window, window_lookup_live(pointer_focus))) pointer_focus = 0;
    if (!window_subtree_contains(window, window_lookup_live(user->FocusWindow))) return;
    *lost = user->FocusWindow;
    ARM64_USER_RECT clip;
    if (window->Parent && window_screen_clip(window->Parent, &clip))
        *fallback = window_handle(window->Parent);
}

/* XP publishes the new focus before WM_KILLFOCUS. Recheck the focus revision
 * after the old window's callback so reentry cannot deliver a stale gain. */
static ULONG_PTR update_window_focus(ULONG_PTR handle, ULONG ticket, ULONG clear_if)
{
    PKTHREAD current = KeGetCurrentThread();
    ARM64_USER_THREAD *user = current->Win32Thread;
    if (!user) return 0;

    gdi_enter();
    ARM64_USER_WINDOW *target = handle ? window_lookup(handle) : NULL;
    ULONG next = target ? window_handle(target) : 0;
    ARM64_USER_RECT clip;
    if ((handle && !target) || (clear_if && user->FocusWindow != clear_if) ||
        (ticket && (!target || !window_screen_clip(target, &clip) ||
                    pointer_focus != next || pointer_focus_ticket != ticket))) {
        gdi_leave();
        return 0;
    }

    for (ARM64_USER_WINDOW *ancestor = target; ancestor; ancestor = ancestor->Parent) {
        if (ancestor->State == UserWindowDestroying) {
            gdi_leave();
            return 0;
        }
    }
    ARM64_USER_WINDOW *pending = window_lookup_live(pointer_focus);
    if (ticket || (!clear_if && pending && pending->Owner == current)) pointer_focus = 0;
    if (ticket || (!foreground_queue && target && desktop_is_presented(target->Desktop)))
        foreground_queue = user;

    ULONG previous = user->FocusWindow;
    if (previous == next) {
        gdi_leave();
        return previous;
    }
    ARM64_USER_WINDOW *old = window_lookup(previous);
    ULONG_PTR old_procedure = old ? old->Class->Procedure : 0;
    user->FocusWindow = next;
    ULONG64 revision = ++user->FocusRevision;
    gdi_leave();

    ULONG_PTR result;
    if (old_procedure) call_window_procedure(previous, old_procedure, USER_WM_KILLFOCUS, next, 0, &result);
    gdi_enter();
    target = window_lookup(next);
    ULONG_PTR procedure = target && user->FocusRevision == revision ? target->Class->Procedure : 0;
    ULONG prior_handle = window_lookup(previous) ? previous : 0;
    gdi_leave();
    if (procedure) {
        call_window_procedure(next, procedure, USER_WM_SETFOCUS, prior_handle, 0, &result);
        gdi_enter();
        prior_handle = window_lookup(previous) ? previous : 0;
        gdi_leave();
    }
    return prior_handle;
}

static ULONG_PTR user_set_focus(WIN32_ARGS)
{
    return update_window_focus(a, 0, 0);
}

static ULONG_PTR user_get_thread_state(WIN32_ARGS)
{
    if (a != XP_THREAD_STATE_FOCUS) return 0;
    ARM64_USER_THREAD *user = KeGetCurrentThread()->Win32Thread;
    gdi_enter();
    ULONG handle = user ? user->FocusWindow : 0;
    gdi_leave();
    return handle;
}
