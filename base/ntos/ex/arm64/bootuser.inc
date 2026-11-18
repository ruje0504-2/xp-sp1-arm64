#include "../../ke/arm64/userentry.h"
#include "../../ps/arm64/bootprocess.h"
#include "xp-token.h"
#include "xp-win32-services.h"
#include "xp-win32-postable.h"
#include "xp-kbd-us.h"
#include "font8x8-basic.h"

typedef struct {
    HANDLE StationHandle;
    ACCESS_MASK StationAccess;
    ARM64_CONSOLE_STATION *Station;
    LIST_ENTRY Threads;
    ULONG_PTR ClientModule, EditProcedure;
} ARM64_USER_PROCESS;

#define USER_QUEUE_LIMIT 64
#define USER_WM_QUIT 0x0012u

typedef struct {
    ULONG_PTR Window;
    ULONG Message;
    ULONG_PTR WParam, LParam;
    ULONG Time;
    LONG X, Y;
} ARM64_USER_MSG;

typedef struct {
    ARM64_USER_MSG Message;
    ULONG Category, InputState;
} ARM64_QUEUE_ENTRY;

_Static_assert(sizeof(ARM64_USER_MSG) == 48 && FIELD_OFFSET(ARM64_USER_MSG, WParam) == 16,
               "native MSG");

typedef struct {
    LIST_ENTRY Link;
    HANDLE DesktopHandle;
    ACCESS_MASK DesktopAccess;
    ULONG WindowCount;
    ARM64_CONSOLE_DESKTOP *Desktop;
    PEPROCESS_QUOTA_BLOCK Quota;
    KEVENT QueueEvent;
    ARM64_QUEUE_ENTRY Messages[USER_QUEUE_LIMIT];
    ULONG MessageCount, PostedCount, QuitCode, KeyModifiers;
    ULONG FocusWindow;
    ULONG64 FocusRevision;
    BOOLEAN QuitPosted;
} ARM64_USER_THREAD;

static EX_PUSH_LOCK binding_lock;

static BOOLEAN console_handle_in_use(PEPROCESS process, HANDLE handle)
{
    const ARM64_USER_PROCESS *user = process->Win32Process;
    if (!user) return FALSE;
    if (user->StationHandle == handle) return TRUE;
    for (PLIST_ENTRY link = user->Threads.Flink; link != &user->Threads; link = link->Flink) {
        const ARM64_USER_THREAD *thread = CONTAINING_RECORD(link, ARM64_USER_THREAD, Link);
        if (thread->DesktopHandle == handle) return TRUE;
    }
    return FALSE;
}

NTSTATUS ExpArm64CloseUserHandle(HANDLE handle, POBJECT_TYPE type)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&binding_lock);
    NTSTATUS status = ObArm64CloseUserHandle(handle, type);
    ExReleasePushLockExclusive(&binding_lock);
    KeLeaveCriticalRegion();
    return status;
}

VOID ExpArm64ReleaseWin32Bindings(PETHREAD thread, BOOLEAN last)
{
    PEPROCESS process = thread->ThreadsProcess;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&binding_lock);
    ARM64_USER_THREAD *user_thread = thread->Tcb.Win32Thread;
    ARM64_USER_PROCESS *user_process = last ? process->Win32Process : NULL;
    if (user_thread) {
        RemoveEntryList(&user_thread->Link);
        thread->Tcb.Win32Thread = NULL;
    }
    if (user_process) {
        if (!IsListEmpty(&user_process->Threads))
            KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, (ULONG_PTR)process, 0x811, 0, 0);
        process->Win32Process = NULL;
    }
    ExReleasePushLockExclusive(&binding_lock);
    KeLeaveCriticalRegion();
    if (user_thread) {
        ObDereferenceObject(user_thread->Desktop);
        PsReturnSharedPoolQuota(user_thread->Quota, 0, sizeof(*user_thread));
        ExFreePool(user_thread);
    }
    if (user_process) {
        ObDereferenceObject(user_process->Station);
        ExFreePool(user_process);
    }
}

static NTSTATUS copy_user_buffer(ULONG_PTR address, PVOID buffer, SIZE_T size, BOOLEAN to_user)
{
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(PsGetCurrentProcess());
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64CopyUserBuffer(space, address, buffer, size, to_user);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

static NTSTATUS copy_user_input(ULONG_PTR address, PVOID buffer, SIZE_T size)
{
    return copy_user_buffer(address, buffer, size, FALSE);
}

static NTSTATUS capture_name(ULONG_PTR address, POBJECT_ATTRIBUTES attributes, PUNICODE_STRING name,
                             PWCHAR buffer, USHORT bytes)
{
    NTSTATUS status = copy_user_input(address, attributes, sizeof(*attributes));
    if (!NT_SUCCESS(status)) return status;
    if (attributes->Length != sizeof(*attributes) ||
        (attributes->Attributes & ~OBJ_CASE_INSENSITIVE) ||
        attributes->SecurityDescriptor || attributes->SecurityQualityOfService)
        return STATUS_INVALID_PARAMETER;
    status = copy_user_input((ULONG_PTR)attributes->ObjectName, name, sizeof(*name));
    if (!NT_SUCCESS(status)) return status;
    if (!name->Length || name->Length > name->MaximumLength || (name->Length & 1) ||
        name->Length > bytes - sizeof(WCHAR))
        return STATUS_OBJECT_NAME_INVALID;
    status = copy_user_input((ULONG_PTR)name->Buffer, buffer, name->Length);
    if (!NT_SUCCESS(status)) return status;
    name->Buffer = buffer;
    name->MaximumLength = bytes;
    buffer[name->Length / sizeof(WCHAR)] = 0;
    return STATUS_SUCCESS;
}

static BOOLEAN token_has_sid(const TOKEN *token, PSID sid, BOOLEAN deny)
{
    for (ULONG i = 0; i < token->UserAndGroupCount; ++i) {
        const SID_AND_ATTRIBUTES *entry = &token->UserAndGroups[i];
        if (RtlEqualSid(entry->Sid, sid))
            return i == 0 || (entry->Attributes & SE_GROUP_ENABLED) ||
                   (deny && (entry->Attributes & SE_GROUP_USE_FOR_DENY_ONLY));
    }
    return FALSE;
}

/* XP's ordinary ordered DACL check for the immutable descriptors on
 * console objects. Restricted, impersonated and audited tokens require the
 * full security subsystem and cannot use this path. */
static NTSTATUS console_grant_access(PVOID object, POBJECT_TYPE type, ACCESS_MASK desired, PACCESS_MASK granted)
{
    RtlMapGenericMask(&desired, &type->TypeInfo.GenericMapping);
    BOOLEAN maximum = !!(desired & MAXIMUM_ALLOWED);
    desired &= ~MAXIMUM_ALLOWED;
    if (desired & ~type->TypeInfo.ValidAccessMask) return STATUS_ACCESS_DENIED;
    PEPROCESS process = PsGetCurrentProcess();
    if (PsGetCurrentThread()->ImpersonationInfo) return STATUS_ACCESS_DENIED;

    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
    PSECURITY_DESCRIPTOR sd = ExFastRefGetObject(*(PEX_FAST_REF)&header->SecurityDescriptor);
    PACL dacl;
    BOOLEAN present, defaulted;
    if (!sd || !NT_SUCCESS(RtlGetDaclSecurityDescriptor(sd, &present, &dacl, &defaulted)) ||
        (present && dacl && !RtlValidAcl(dacl)))
        return STATUS_ACCESS_DENIED;

    PTOKEN token = ObFastReferenceObject(&process->Token);
    if (!token) return STATUS_ACCESS_DENIED;
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(token->TokenLock, TRUE);
    ACCESS_MASK allowed = 0, denied = 0;
    NTSTATUS status = STATUS_ACCESS_DENIED;
    if (!token->RestrictedSidCount && token->SessionId == 0) {
        if (!present || !dacl) allowed = type->TypeInfo.ValidAccessMask;
        else {
            for (ULONG i = 0; i < dacl->AceCount; ++i) {
                PACCESS_ALLOWED_ACE ace;
                if (!NT_SUCCESS(RtlGetAce(dacl, i, (PVOID *)&ace)) || ace->Header.AceFlags ||
                    (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceType != ACCESS_DENIED_ACE_TYPE))
                    goto done;
                BOOLEAN deny = ace->Header.AceType == ACCESS_DENIED_ACE_TYPE;
                if (!token_has_sid(token, &ace->SidStart, deny)) continue;
                if (deny) denied |= ace->Mask & ~allowed;
                else allowed |= ace->Mask & ~denied;
            }
        }
        if (!(desired & ~allowed) && (!maximum || (allowed & type->TypeInfo.ValidAccessMask))) {
            *granted = maximum ? allowed & type->TypeInfo.ValidAccessMask : desired;
            status = STATUS_SUCCESS;
        }
    }
done:
    ExReleaseResourceLite(token->TokenLock);
    KeLeaveCriticalRegion();
    ObFastDereferenceObject(&process->Token, token);
    return status;
}

static PVOID open_console_object(ULONG_PTR address, ACCESS_MASK access, BOOLEAN desktop)
{
    WCHAR buffer[260];
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name;
    NTSTATUS status = capture_name(address, &attributes, &name, buffer, sizeof(buffer));
    if (!NT_SUCCESS(status)) return NULL;

    /* Resolve stations only in the resident console directory. A relative
     * desktop lookup begins at a typed window-station handle in this process. */
    PVOID object;
    POBJECT_TYPE type = desktop ? ExDesktopObjectType : ExWindowStationObjectType;
    if (desktop) {
        ARM64_CONSOLE_STATION *station;
        if (!component_name(&name) || !attributes.RootDirectory) return NULL;
        status = ObReferenceObjectByHandle(attributes.RootDirectory, WINSTA_ENUMDESKTOPS,
                                            ExWindowStationObjectType, UserMode, (PVOID *)&station, NULL);
        if (!NT_SUCCESS(status)) return NULL;
        console_lock_acquire();
        object = find_desktop(station, &name, !!(attributes.Attributes & OBJ_CASE_INSENSITIVE));
        console_lock_release();
        ObDereferenceObject(station);
        if (!object) return NULL;
        access |= DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS;
    } else {
        static const WCHAR path[] = L"\\Windows\\WindowStations\\";
        UNICODE_STRING prefix = {sizeof(path) - sizeof(WCHAR), sizeof(path), (PWCHAR)path};
        if (attributes.RootDirectory || name.Length <= prefix.Length) return NULL;
        UNICODE_STRING given = {prefix.Length, prefix.Length, name.Buffer};
        if (!RtlEqualUnicodeString(&prefix, &given, !!(attributes.Attributes & OBJ_CASE_INSENSITIVE))) return NULL;
        name.Buffer += prefix.Length / sizeof(WCHAR);
        name.Length -= prefix.Length;
        name.MaximumLength -= prefix.Length;
        if (!component_name(&name)) return NULL;
        status = ObArm64LookupBootObjectName(station_directory, &name, attributes.Attributes, type, &object);
        if (!NT_SUCCESS(status)) return NULL;
    }

    HANDLE handle = NULL;
    ACCESS_MASK granted;
    status = console_grant_access(object, type, access, &granted);
    if (NT_SUCCESS(status)) status = ObArm64CreateBootHandle(object, granted, FALSE, &handle);
    ObDereferenceObject(object);
    return NT_SUCCESS(status) ? handle : NULL;
}

static BOOLEAN bind_window_station(HANDLE handle)
{
    PEPROCESS process = PsGetCurrentProcess();
    ARM64_CONSOLE_STATION *station, *old = NULL;
    ARM64_USER_PROCESS *user = process->Win32Process;
    if (!user) {
        user = ExAllocatePoolWithTag(NonPagedPool, sizeof(*user), 'pUsW');
        if (!user) return FALSE;
        RtlZeroMemory(user, sizeof(*user));
        InitializeListHead(&user->Threads);
    }
    OBJECT_HANDLE_INFORMATION info;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, ExWindowStationObjectType,
                                                UserMode, (PVOID *)&station, &info);
    if (NT_SUCCESS(status)) {
        if (user->Station && user->Station != station && !IsListEmpty(&user->Threads)) {
            status = STATUS_DEVICE_BUSY;
            ObDereferenceObject(station);
        } else {
            old = user->Station;
            user->Station = station;
            user->StationHandle = handle;
            user->StationAccess = info.GrantedAccess;
            process->Win32Process = user;
        }
    }
    if (process->Win32Process != user) ExFreePool(user);
    if (old) ObDereferenceObject(old);
    return NT_SUCCESS(status);
}

static BOOLEAN bind_thread_desktop(HANDLE handle)
{
    ARM64_USER_PROCESS *user = PsGetCurrentProcess()->Win32Process;
    PKTHREAD current = KeGetCurrentThread();
    if (!user) return FALSE;
    ARM64_CONSOLE_DESKTOP *desktop;
    OBJECT_HANDLE_INFORMATION info;
    NTSTATUS status = ObReferenceObjectByHandle(handle, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS,
                                                ExDesktopObjectType, UserMode, (PVOID *)&desktop, &info);
    if (!NT_SUCCESS(status)) return FALSE;
    ARM64_USER_THREAD *thread = current->Win32Thread;
    if (desktop->Station != user->Station || (thread && thread->WindowCount && thread->Desktop != desktop)) {
        ObDereferenceObject(desktop);
        return FALSE;
    }
    if (!thread) {
        PEPROCESS_QUOTA_BLOCK quota = PsChargeSharedPoolQuota(PsGetCurrentProcess(), 0, sizeof(*thread));
        if (!quota) {
            ObDereferenceObject(desktop);
            return FALSE;
        }
        thread = ExAllocatePoolWithTag(NonPagedPool, sizeof(*thread), 'tUsW');
        if (!thread) {
            PsReturnSharedPoolQuota(quota, 0, sizeof(*thread));
            ObDereferenceObject(desktop);
            return FALSE;
        }
        RtlZeroMemory(thread, sizeof(*thread));
        thread->Quota = quota;
        KeInitializeEvent(&thread->QueueEvent, NotificationEvent, FALSE);
        InsertTailList(&user->Threads, &thread->Link);
        current->Win32Thread = thread;
    }
    ARM64_CONSOLE_DESKTOP *old = thread->Desktop;
    thread->Desktop = desktop;
    thread->DesktopHandle = handle;
    thread->DesktopAccess = info.GrantedAccess;
    if (old) ObDereferenceObject(old);
    return TRUE;
}

static HANDLE query_thread_desktop(ULONG tid)
{
    PETHREAD thread;
    HANDLE desktop = NULL;
    if (NT_SUCCESS(PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)tid, &thread))) {
        ARM64_USER_THREAD *user = thread->Tcb.Win32Thread;
        if (thread->ThreadsProcess == PsGetCurrentProcess() && user) desktop = user->DesktopHandle;
        ObDereferenceObject(thread);
    }
    return desktop;
}

/* These eight XP services return a handle or BOOL. The slot-specific
 * adapters use the register ABI of the existing Win32 service table. */
#define WIN32_ARGS ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d, \
                   ULONG_PTR e, ULONG_PTR f, ULONG_PTR g, ULONG_PTR h, \
                   ULONG_PTR i, ULONG_PTR j, ULONG_PTR k, ULONG_PTR l, \
                   ULONG_PTR m, ULONG_PTR n, ULONG_PTR o
static ULONG_PTR user_open_station(WIN32_ARGS)
{
    return (ULONG_PTR)open_console_object(a, (ACCESS_MASK)b, FALSE);
}

static ULONG_PTR user_open_desktop(WIN32_ARGS)
{
    return b ? 0 : (ULONG_PTR)open_console_object(a, (ACCESS_MASK)c, TRUE);
}

static ULONG_PTR user_set_station(WIN32_ARGS)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&binding_lock);
    BOOLEAN result = bind_window_station((HANDLE)a);
    ExReleasePushLockExclusive(&binding_lock);
    KeLeaveCriticalRegion();
    return result;
}

static ULONG_PTR user_get_station(WIN32_ARGS)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&binding_lock);
    ARM64_USER_PROCESS *user = PsGetCurrentProcess()->Win32Process;
    HANDLE handle = user ? user->StationHandle : NULL;
    ExReleasePushLockShared(&binding_lock);
    KeLeaveCriticalRegion();
    return (ULONG_PTR)handle;
}

static ULONG_PTR user_set_desktop(WIN32_ARGS)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&binding_lock);
    BOOLEAN result = bind_thread_desktop((HANDLE)a);
    ExReleasePushLockExclusive(&binding_lock);
    KeLeaveCriticalRegion();
    return result;
}

static ULONG_PTR user_get_desktop(WIN32_ARGS)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&binding_lock);
    HANDLE handle = query_thread_desktop((ULONG)a);
    ExReleasePushLockShared(&binding_lock);
    KeLeaveCriticalRegion();
    return (ULONG_PTR)handle;
}

static ULONG_PTR user_close_station(WIN32_ARGS)
{
    return NT_SUCCESS(ExpArm64CloseUserHandle((HANDLE)a, ExWindowStationObjectType));
}

static ULONG_PTR user_close_desktop(WIN32_ARGS)
{
    return NT_SUCCESS(ExpArm64CloseUserHandle((HANDLE)a, ExDesktopObjectType));
}
#include "bootgdi.inc"
#undef WIN32_ARGS

static PKARM64_WIN32_SERVICE user_services[] = {
    [XP_UserOpenWindowStation - 0x1000] = user_open_station,
    [XP_UserOpenDesktop - 0x1000] = user_open_desktop,
    [XP_UserSetProcessWindowStation - 0x1000] = user_set_station,
    [XP_UserGetProcessWindowStation - 0x1000] = user_get_station,
    [XP_UserSetThreadDesktop - 0x1000] = user_set_desktop,
    [XP_UserGetThreadDesktop - 0x1000] = user_get_desktop,
    [XP_UserCloseWindowStation - 0x1000] = user_close_station,
    [XP_UserCloseDesktop - 0x1000] = user_close_desktop,
    [XP_GdiCreateBitmap - 0x1000] = gdi_create_bitmap,
    [XP_GdiCreateCompatibleDC - 0x1000] = gdi_create_dc,
    [XP_GdiSelectBitmap - 0x1000] = gdi_select_bitmap,
    [XP_GdiSetPixel - 0x1000] = gdi_set_pixel,
    [XP_GdiGetPixel - 0x1000] = gdi_get_pixel,
    [XP_GdiGetBitmapBits - 0x1000] = gdi_get_bitmap_bits,
    [XP_GdiDeleteObjectApp - 0x1000] = gdi_delete_object,
    [XP_GdiBitBlt - 0x1000] = gdi_bitblt,
    [XP_GdiExtTextOutW - 0x1000] = gdi_ext_text_out,
    [XP_GdiGetTextExtent - 0x1000] = gdi_get_text_extent,
    [XP_UserGetDC - 0x1000] = user_get_dc,
    [XP_UserRegisterClassExWOW - 0x1000] = user_register_class,
    [XP_UserInitializeClientPfnArrays - 0x1000] = user_initialize_client,
    [XP_UserSetWindowFNID - 0x1000] = user_set_window_fnid,
    [XP_UserCreateWindowEx - 0x1000] = user_create_window,
    [XP_UserDestroyWindow - 0x1000] = user_destroy_window,
    [XP_UserShowWindow - 0x1000] = user_show_window,
    [XP_UserSetFocus - 0x1000] = user_set_focus,
    [XP_UserGetAncestor - 0x1000] = user_get_ancestor,
    [XP_UserGetThreadState - 0x1000] = user_get_thread_state,
    [XP_UserCallOneParam - 0x1000] = user_call_one_param,
    [XP_UserInvalidateRect - 0x1000] = user_invalidate_rect,
    [XP_UserGetUpdateRect - 0x1000] = user_get_update_rect,
    [XP_UserBeginPaint - 0x1000] = user_begin_paint,
    [XP_UserEndPaint - 0x1000] = user_end_paint,
    [XP_UserPeekMessage - 0x1000] = user_peek_message,
    [XP_UserGetMessage - 0x1000] = user_get_message,
    [XP_UserPostMessage - 0x1000] = user_post_message,
    [XP_UserPostThreadMessage - 0x1000] = user_post_thread_message,
    [XP_UserDispatchMessage - 0x1000] = user_dispatch_message,
    [XP_UserTranslateMessage - 0x1000] = user_translate_message,
    [XP_UserMessageCall - 0x1000] = user_message_call,
    [XP_UserDefSetText - 0x1000] = user_default_set_text,
    [XP_UserInternalGetWindowText - 0x1000] = user_internal_get_window_text,
    [XP_UserCallHwndLock - 0x1000] = user_call_hwnd_lock,
};

static UCHAR user_service_arguments[sizeof(user_services) / sizeof(user_services[0])] = {
    [XP_GdiBitBlt - 0x1000] = 11,
    [XP_GdiExtTextOutW - 0x1000] = 9,
    [XP_UserCreateWindowEx - 0x1000] = 15,
};

static BOOLEAN register_console_services(VOID)
{
    return KeAddSystemServiceTable((PULONG_PTR)user_services, NULL,
                                  sizeof(user_services) / sizeof(user_services[0]),
                                  user_service_arguments, WIN32K_SERVICE_INDEX);
}
