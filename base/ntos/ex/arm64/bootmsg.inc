/* The binding lock pins another thread's USER state while posting.
 * gdi_lock serializes the queue with HWND publication and paint state. */
#define USER_PM_REMOVE 0x0001u
#define USER_QS_KEY 0x0001u
#define USER_QS_MOUSEMOVE 0x0002u
#define USER_QS_MOUSEBUTTON 0x0004u
#define USER_QS_INPUT (USER_QS_KEY | USER_QS_MOUSEMOVE | USER_QS_MOUSEBUTTON)
#define USER_QS_POST 0x0008u
#define USER_QS_PAINT 0x0020u
#define USER_QS_ALLPOST 0x0100u
#define USER_MOD_SHIFT 1u
#define USER_MOD_CTRL 2u
#define USER_MOD_CAPS 4u
#define USER_MOD_ALT 8u
#define USER_WM_KEYDOWN 0x0100u
#define USER_WM_CHAR 0x0102u
#define USER_WM_SYSKEYDOWN 0x0104u
#define USER_WM_SYSCHAR 0x0106u
#define USER_THREAD_MESSAGE_FILTER ((ULONG_PTR)-1)

typedef enum {
    UserMessageNone, UserMessageQueued, UserMessageQuit, UserMessagePaint
} ARM64_MESSAGE_KIND;

static VOID make_user_message(ARM64_USER_MSG *msg, ULONG_PTR hwnd, ULONG message,
                              ULONG_PTR wparam, ULONG_PTR lparam)
{
    RtlZeroMemory(msg, sizeof(*msg));
    msg->Window = hwnd;
    msg->Message = message;
    msg->WParam = wparam;
    msg->LParam = lparam;
    msg->Time = (ULONG)(KeQueryInterruptTime() / 10000);
}

static VOID discard_window_posts(ARM64_USER_THREAD *thread, ULONG_PTR hwnd)
{
    ULONG kept = 0;
    for (ULONG i = 0; i < thread->MessageCount; ++i) {
        const ARM64_QUEUE_ENTRY *entry = &thread->Messages[i];
        if (entry->Message.Window != hwnd) thread->Messages[kept++] = *entry;
        else if (entry->Category == USER_QS_POST) --thread->PostedCount;
    }
    thread->MessageCount = kept;
}

static BOOLEAN enqueue_user_message(ARM64_USER_THREAD *receiver, ULONG category, ULONG_PTR hwnd,
                                    ULONG message, ULONG_PTR wparam, ULONG_PTR lparam, LONG x, LONG y, ULONG input_state)
{
    if (!receiver || receiver->MessageCount == USER_QUEUE_LIMIT) return FALSE;
    ARM64_QUEUE_ENTRY *entry = &receiver->Messages[receiver->MessageCount++];
    entry->Category = category;
    entry->InputState = input_state;
    make_user_message(&entry->Message, hwnd, message, wparam, lparam);
    entry->Message.X = x;
    entry->Message.Y = y;
    if (category == USER_QS_POST) ++receiver->PostedCount;
    KeSetEvent(&receiver->QueueEvent, 0, FALSE);
    return TRUE;
}

static BOOLEAN user_scalar_message(ULONG message, ULONG_PTR lparam)
{
    if (message > 0xffff || message == USER_WM_QUIT || message == 0x00ff ||
        message == 0x0118 || (message == 0x0113 && lparam) ||
        (message >= 0x03e0 && message <= 0x03e8)) return FALSE;
    return message >= 0x400 || ((user_scalar_post_mask[message / 32] >> (message % 32)) & 1);
}

static BOOLEAN post_to_thread(PKTHREAD target, ULONG_PTR hwnd, ULONG message, ULONG_PTR wparam, ULONG_PTR lparam)
{
    ARM64_USER_THREAD *sender = KeGetCurrentThread()->Win32Thread;
    if (!sender || !target || ((PETHREAD)target)->ThreadsProcess != PsGetCurrentProcess()) return FALSE;
    ARM64_USER_THREAD *receiver = target->Win32Thread;
    return receiver && receiver->Desktop == sender->Desktop &&
           enqueue_user_message(receiver, USER_QS_POST, hwnd, message, wparam, lparam, 0, 0, 0);
}

static ULONG_PTR user_post_message(WIN32_ARGS)
{
    if (!user_scalar_message((ULONG)b, d)) return FALSE;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&binding_lock);
    gdi_enter();
    ARM64_USER_WINDOW *window = a ? window_lookup_live(a) : NULL;
    PKTHREAD target = a ? (window ? window->Owner : NULL) : KeGetCurrentThread();
    BOOLEAN posted = post_to_thread(target, window ? window_handle(window) : 0, (ULONG)b, c, d);
    gdi_leave();
    ExReleasePushLockShared(&binding_lock);
    KeLeaveCriticalRegion();
    return posted;
}

static ULONG_PTR user_post_thread_message(WIN32_ARGS)
{
    if (!user_scalar_message((ULONG)b, d)) return FALSE;
    PETHREAD target;
    if (!NT_SUCCESS(PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)(ULONG)a, &target))) return FALSE;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&binding_lock);
    gdi_enter();
    BOOLEAN posted = post_to_thread(&target->Tcb, 0, (ULONG)b, c, d);
    gdi_leave();
    ExReleasePushLockShared(&binding_lock);
    KeLeaveCriticalRegion();
    ObDereferenceObject(target);
    return posted;
}

static BOOLEAN post_quit_message(ULONG code)
{
    ARM64_USER_THREAD *thread = KeGetCurrentThread()->Win32Thread;
    if (!thread) return FALSE;
    gdi_enter();
    thread->QuitPosted = TRUE;
    thread->QuitCode = code;
    KeSetEvent(&thread->QueueEvent, 0, FALSE);
    gdi_leave();
    return TRUE;
}

static BOOLEAN message_in_range(ULONG message, ULONG low, ULONG high)
{
    return !(low || high) || (message >= low && message <= high);
}

static BOOLEAN matches_message_window(ULONG_PTR candidate, ULONG_PTR filter)
{
    if (!filter) return TRUE;
    if (filter == USER_THREAD_MESSAGE_FILTER) return !candidate;
    return window_subtree_contains(window_lookup(filter), window_lookup_live(candidate));
}

static BOOLEAN find_queued_message(const ARM64_USER_THREAD *thread, ULONG_PTR hwnd, ULONG low, ULONG high,
                                   ULONG categories, ULONG *index, ARM64_USER_MSG *msg)
{
    if (!categories) return FALSE;
    for (ULONG i = 0; i < thread->MessageCount; ++i) {
        const ARM64_QUEUE_ENTRY *entry = &thread->Messages[i];
        const ARM64_USER_MSG *candidate = &entry->Message;
        if (!(categories & entry->Category) ||
            !matches_message_window(candidate->Window, hwnd) ||
            !message_in_range(candidate->Message, low, high)) continue;
        *msg = *candidate;
        *index = i;
        return TRUE;
    }
    return FALSE;
}

static ARM64_MESSAGE_KIND select_message(ARM64_USER_THREAD *thread, ULONG_PTR hwnd, ULONG low, ULONG high,
                                         ULONG flags, ULONG *index, ARM64_USER_MSG *msg)
{
    ULONG categories = flags >> 16;
    if (!categories) categories = USER_QS_INPUT | USER_QS_POST | USER_QS_PAINT;
    if (categories & (USER_QS_POST | USER_QS_ALLPOST)) {
        /* XP synthesizes WM_QUIT after its posted list drains. */
        if (thread->QuitPosted && !thread->PostedCount) {
            make_user_message(msg, 0, USER_WM_QUIT, thread->QuitCode, 0);
            return UserMessageQuit;
        }
        if (find_queued_message(thread, hwnd, low, high, USER_QS_POST, index, msg))
            return UserMessageQueued;
    }
    if (find_queued_message(thread, hwnd, low, high, categories & USER_QS_INPUT, index, msg))
        return UserMessageQueued;
    if (hwnd == USER_THREAD_MESSAGE_FILTER || !(categories & USER_QS_PAINT) ||
        !message_in_range(USER_WM_PAINT, low, high)) return UserMessageNone;

    /* Paint stays eligible until BeginPaint validates the update region. */
    for (PLIST_ENTRY link = window_order.Blink; link != &window_order; link = link->Blink) {
        ARM64_USER_WINDOW *window = CONTAINING_RECORD(link, ARM64_USER_WINDOW, ZOrder);
        if (window->Owner == KeGetCurrentThread() && matches_message_window(window_handle(window), hwnd) &&
            has_paint_message(window)) {
            make_user_message(msg, window_handle(window), USER_WM_PAINT, 0, 0);
            return UserMessagePaint;
        }
    }
    return UserMessageNone;
}

static BOOLEAN user_output_writable(ULONG_PTR output, SIZE_T bytes)
{
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(PsGetCurrentProcess());
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64ProbeUserWrite(space, output, bytes);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return NT_SUCCESS(status);
}

static ULONG receive_message(ULONG_PTR output, ULONG_PTR hwnd, ULONG low, ULONG high, ULONG flags, BOOLEAN wait)
{
    ARM64_USER_THREAD *thread = KeGetCurrentThread()->Win32Thread;
    ULONG failure = wait ? MAXULONG : 0;
    if (!thread || low > high || (flags & 0x0000fffcu) ||
        (wait && !user_output_writable(output, sizeof(ARM64_USER_MSG)))) return failure;
    if (hwnd == 0xffff || hwnd == MAXULONG) hwnd = USER_THREAD_MESSAGE_FILTER;

    for (;;) {
        gdi_enter();
        if (hwnd && hwnd != USER_THREAD_MESSAGE_FILTER && !window_lookup(hwnd)) {
            gdi_leave();
            return failure;
        }
        ULONG index = 0;
        ARM64_USER_MSG msg;
        ARM64_MESSAGE_KIND source = select_message(thread, hwnd, low, high, flags, &index, &msg);
        if (source != UserMessageNone) {
            BOOLEAN copied = NT_SUCCESS(copy_user_buffer(output, &msg, sizeof(msg), TRUE));
            ULONG focus_ticket = 0;
            if (copied && (flags & USER_PM_REMOVE)) {
                if (source == UserMessageQuit) thread->QuitPosted = FALSE;
                else if (source == UserMessageQueued) {
                    ARM64_QUEUE_ENTRY *entry = &thread->Messages[index];
                    if (entry->Category == USER_QS_POST) --thread->PostedCount;
                    if (entry->Category == USER_QS_KEY) thread->KeyModifiers = entry->InputState;
                    if (entry->Category == USER_QS_MOUSEBUTTON) focus_ticket = entry->InputState;
                    --thread->MessageCount;
                    RtlMoveMemory(entry, entry + 1, (thread->MessageCount - index) * sizeof(*entry));
                }
            }
            gdi_leave();
            if (focus_ticket) update_window_focus(msg.Window, focus_ticket, 0);
            return copied ? !wait || source != UserMessageQuit : failure;
        }
        if (!wait) {
            gdi_leave();
            return FALSE;
        }

        /* A post between releasing the lock and entering the wait leaves
         * the notification event signaled, including when the receiver
         * filtered out older messages already in the queue. */
        KeResetEvent(&thread->QueueEvent);
        gdi_leave();
        if (KeWaitForSingleObject(&thread->QueueEvent, WrUserRequest, UserMode, FALSE, NULL) != STATUS_SUCCESS)
            return failure;
    }
}

static ULONG_PTR user_peek_message(WIN32_ARGS)
{
    return receive_message(a, b, (ULONG)c, (ULONG)d, (ULONG)e, FALSE);
}

static ULONG_PTR user_get_message(WIN32_ARGS)
{
    return receive_message(a, b, (ULONG)c, (ULONG)d, USER_PM_REMOVE, TRUE);
}

/* XP translates with the state of removed keyboard input. A later hardware
 * modifier transition can already be queued without changing this state. */
static ULONG_PTR user_translate_message(WIN32_ARGS)
{
    ARM64_USER_MSG message;
    if (b || !NT_SUCCESS(copy_user_input(a, &message, sizeof(message)))) return FALSE;
    BOOLEAN system_key = message.Message == USER_WM_SYSKEYDOWN;
    if (!system_key && message.Message != USER_WM_KEYDOWN)
        return message.Message == USER_WM_KEYDOWN + 1 || message.Message == USER_WM_SYSKEYDOWN + 1;
    if (message.LParam & 0x80000000u) return TRUE;

    gdi_enter();
    ARM64_USER_THREAD *thread = KeGetCurrentThread()->Win32Thread;
    ARM64_USER_WINDOW *window = window_lookup(message.Window);
    BOOLEAN translated = thread && window;
    USHORT vk = (USHORT)message.WParam;
    if (translated && vk < (sizeof(user_key_chars) / sizeof(user_key_chars[0])) && user_key_chars[vk].Present) {
        ULONG mods = thread->KeyModifiers;
        ULONG column = mods & (USER_MOD_SHIFT | USER_MOD_CTRL);
        if (!(mods & USER_MOD_CTRL) && (mods & USER_MOD_CAPS) && user_key_chars[vk].Caps)
            column ^= USER_MOD_SHIFT;
        USHORT ch = user_key_chars[vk].Char[column];
        if (ch == 0xffff && (mods & USER_MOD_CTRL) && vk >= 'A' && vk <= 'Z') ch = vk & 0x1f;
        if (ch != 0xffff && (mods & (USER_MOD_ALT | USER_MOD_CTRL)) != (USER_MOD_ALT | USER_MOD_CTRL))
            translated = enqueue_user_message(thread, USER_QS_POST, window_handle(window),
                                              system_key ? USER_WM_SYSCHAR : USER_WM_CHAR, ch,
                                              message.LParam & 0x7fffffffu, message.X, message.Y, 0);
    }
    gdi_leave();
    return translated;
}

static ULONG_PTR user_dispatch_message(WIN32_ARGS)
{
    ARM64_USER_MSG message;
    if (!NT_SUCCESS(copy_user_input(a, &message, sizeof(message))) || !message.Window ||
        !user_scalar_message(message.Message, message.LParam)) return 0;
    ULONG_PTR result = 0;
    if (message.Message == USER_WM_PAINT)
        return !message.WParam && !message.LParam &&
               dispatch_paint(message.Window, FALSE, &result) ? result : 0;

    gdi_enter();
    ARM64_USER_WINDOW *window = window_lookup(message.Window);
    ULONG_PTR procedure = window ? window->Class->Procedure : 0;
    gdi_leave();
    return procedure && call_window_procedure(message.Window, procedure, message.Message,
                                              message.WParam, message.LParam, &result) ? result : 0;
}
