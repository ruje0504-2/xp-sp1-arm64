/* Resident console objects follow the XP window station / desktop container
 * relationship. Their object-manager methods run from the shared kernel map. */
typedef struct {
    ULONG SessionId;
    LONG OpenHandles;
} ARM64_CONSOLE_OBJECT;

typedef struct _ARM64_CONSOLE_DESKTOP ARM64_CONSOLE_DESKTOP;
typedef struct {
    ARM64_CONSOLE_OBJECT Header;
    ARM64_CONSOLE_DESKTOP *Desktops;
    BOOLEAN Interactive;
} ARM64_CONSOLE_STATION;

struct _ARM64_CONSOLE_DESKTOP {
    ARM64_CONSOLE_OBJECT Header;
    ARM64_CONSOLE_STATION *Station;
    ARM64_CONSOLE_DESKTOP *Next;
    UNICODE_STRING Name;
    BOOLEAN Published;
    WCHAR NameBuffer[1];
};

static EX_PUSH_LOCK console_lock;
/* Zero-initialized push locks use the unlocked representation. */
static EX_PUSH_LOCK console_start_lock;
static HANDLE windows_directory;
static HANDLE station_directory;

static BOOLEAN register_console_services(VOID);
static VOID initialize_console_display(VOID);
static BOOLEAN console_handle_in_use(PEPROCESS process, HANDLE handle);

static VOID console_lock_acquire(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&console_lock);
}

static VOID console_lock_release(VOID)
{
    ExReleasePushLockExclusive(&console_lock);
    KeLeaveCriticalRegion();
}

static NTSTATUS console_open(PKWIN32_OPENMETHOD_PARAMETERS parameters)
{
    ARM64_CONSOLE_OBJECT *body = parameters->Object;
    if (!parameters->Process || parameters->Process->Session || body->SessionId)
        return STATUS_NOT_FOUND;
    InterlockedIncrement(&body->OpenHandles);
    return STATUS_SUCCESS;
}

static NTSTATUS console_okay_to_close(PKWIN32_OKAYTOCLOSEMETHOD_PARAMETERS parameters)
{
    ARM64_CONSOLE_OBJECT *body = parameters->Object;
    if (body->OpenHandles <= 0) return STATUS_INVALID_HANDLE;
    if (parameters->PreviousMode == UserMode && console_handle_in_use(parameters->Process, parameters->Handle))
        return STATUS_HANDLE_NOT_CLOSABLE;
    return STATUS_SUCCESS;
}

static NTSTATUS console_close(PKWIN32_CLOSEMETHOD_PARAMETERS parameters)
{
    ARM64_CONSOLE_OBJECT *body = parameters->Object;
    if (InterlockedDecrement(&body->OpenHandles) < 0)
        KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, (ULONG_PTR)body, body->OpenHandles, 0, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS console_delete(PKWIN32_DELETEMETHOD_PARAMETERS parameters)
{
    ARM64_CONSOLE_OBJECT *body = parameters->Object;
    if (body->OpenHandles) KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, (ULONG_PTR)body, body->OpenHandles, 0, 0);
    if (OBJECT_TO_OBJECT_HEADER(body)->Type == ExDesktopObjectType) {
        ARM64_CONSOLE_DESKTOP *desktop = (PVOID)body;
        ARM64_CONSOLE_STATION *station = desktop->Station;
        if (desktop->Published) {
            console_lock_acquire();
            ARM64_CONSOLE_DESKTOP **link = &station->Desktops;
            while (*link && *link != desktop) link = &(*link)->Next;
            if (*link != desktop) KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, (ULONG_PTR)desktop, 0, 0, 0);
            *link = desktop->Next;
            console_lock_release();
        }
        if (station) ObDereferenceObject(station);
    } else if (((ARM64_CONSOLE_STATION *)body)->Desktops) {
        KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, (ULONG_PTR)body, 0, 0, 0);
    }
    return STATUS_SUCCESS;
}

/* ParseWindowStation consumes one desktop name; ParseDesktop searches
 * the station's desktop list using the caller's case sensitivity. */
static ARM64_CONSOLE_DESKTOP *find_desktop(ARM64_CONSOLE_STATION *station, PCUNICODE_STRING name,
                                           BOOLEAN insensitive)
{
    for (ARM64_CONSOLE_DESKTOP *desktop = station->Desktops; desktop; desktop = desktop->Next) {
        if (RtlEqualUnicodeString(&desktop->Name, name, insensitive) && ObReferenceObjectSafe(desktop))
            return desktop;
    }
    return NULL;
}

static BOOLEAN component_name(PCUNICODE_STRING name)
{
    if (!name || !name->Buffer || !name->Length || name->Length > name->MaximumLength ||
        name->Length > 0xfffc || (name->Length & 1))
        return FALSE;
    for (ULONG i = 0; i < name->Length / sizeof(WCHAR); ++i) {
        if (!name->Buffer[i] || name->Buffer[i] == L'\\') return FALSE;
    }
    return TRUE;
}

static NTSTATUS console_parse(PKWIN32_PARSEMETHOD_PARAMETERS parameters)
{
    UNICODE_STRING name = *parameters->RemainingName;
    *parameters->Object = NULL;
    if (!name.Length) {
        if (parameters->ObjectType && parameters->ObjectType != ExWindowStationObjectType)
            return STATUS_OBJECT_TYPE_MISMATCH;
        ObReferenceObject(parameters->ParseObject);
        *parameters->Object = parameters->ParseObject;
        return STATUS_SUCCESS;
    }
    if (name.Buffer[0] == L'\\') {
        ++name.Buffer;
        name.Length -= sizeof(WCHAR);
        name.MaximumLength -= sizeof(WCHAR);
    }
    if (!component_name(&name)) return STATUS_OBJECT_PATH_INVALID;
    if (parameters->ObjectType != ExDesktopObjectType) return STATUS_OBJECT_TYPE_MISMATCH;
    console_lock_acquire();
    PVOID found = find_desktop(parameters->ParseObject, &name, !!(parameters->Attributes & OBJ_CASE_INSENSITIVE));
    console_lock_release();
    if (!found) return STATUS_OBJECT_NAME_NOT_FOUND;
    *parameters->Object = found;
    return STATUS_SUCCESS;
}

NTSTATUS ExpArm64InitializeWin32Console(VOID)
{
    PEPROCESS process = PsGetCurrentProcess();
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || KeGetPreviousMode() != KernelMode ||
        (process != PsInitialSystemProcess && process != PsIdleProcess) ||
        !ExDesktopObjectType || !ExWindowStationObjectType)
        return STATUS_INVALID_DEVICE_STATE;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&console_start_lock);
    NTSTATUS status = STATUS_SUCCESS;
    if (windows_directory && station_directory) goto done;

    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    RtlInitUnicodeString(&name, L"\\Windows");
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_OPENIF, NULL, NULL);
    HANDLE parent;
    status = NtCreateDirectoryObject(&parent, DIRECTORY_ALL_ACCESS, &attributes);
    if (!NT_SUCCESS(status)) goto done;
    RtlInitUnicodeString(&name, L"WindowStations");
    attributes.RootDirectory = parent;
    HANDLE directory;
    status = NtCreateDirectoryObject(&directory, DIRECTORY_ALL_ACCESS, &attributes);
    if (!NT_SUCCESS(status)) {
        ObArm64CloseBootObjectHandle(parent);
        goto done;
    }
    if (!register_console_services()) {
        ObArm64CloseBootObjectHandle(directory);
        ObArm64CloseBootObjectHandle(parent);
        status = STATUS_DEVICE_BUSY;
        goto done;
    }
    ExWindowStationObjectType->TypeInfo.GenericMapping = WinStaMapping;
    ExWindowStationObjectType->TypeInfo.ValidAccessMask = WinStaMapping.GenericAll;
    ExDesktopObjectType->TypeInfo.GenericMapping = DesktopMapping;
    ExDesktopObjectType->TypeInfo.ValidAccessMask = DesktopMapping.GenericAll;
    /* The two namespace handles belong to the console for this boot. */
    ExInitializePushLock(&console_lock);
    ExWindowStationOpenProcedureCallout = ExDesktopOpenProcedureCallout = console_open;
    ExWindowStationOkToCloseProcedureCallout = ExDesktopOkToCloseProcedureCallout = console_okay_to_close;
    ExWindowStationCloseProcedureCallout = ExDesktopCloseProcedureCallout = console_close;
    ExWindowStationDeleteProcedureCallout = ExDesktopDeleteProcedureCallout = console_delete;
    ExWindowStationParseProcedureCallout = console_parse;
    initialize_console_display();
    windows_directory = parent;
    station_directory = directory;

done:
    ExReleasePushLockExclusive(&console_start_lock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS ExpArm64CreateWindowStation(PCUNICODE_STRING name, PHANDLE handle)
{
    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = NULL;
    if (!station_directory) return STATUS_INVALID_DEVICE_STATE;
    if (!component_name(name)) return STATUS_OBJECT_NAME_INVALID;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, (PUNICODE_STRING)name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               station_directory, NULL);
    ARM64_CONSOLE_STATION *station;
    NTSTATUS status = ObArm64AllocateBootServiceObject(&attributes, ExWindowStationObjectType,
                                                       sizeof(*station), (PVOID *)&station);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING interactive;
    RtlInitUnicodeString(&interactive, L"WinSta0");
    station->Interactive = RtlEqualUnicodeString(name, &interactive, TRUE);
    return ObArm64InsertBootServiceObject(&attributes, station, GENERIC_ALL, handle);
}

NTSTATUS ExpArm64CreateDesktop(HANDLE station_handle, PCUNICODE_STRING name, PHANDLE handle)
{
    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = NULL;
    if (!station_directory || KeGetCurrentIrql() != PASSIVE_LEVEL || KeGetPreviousMode() != KernelMode)
        return STATUS_INVALID_DEVICE_STATE;
    if (!component_name(name)) return STATUS_OBJECT_NAME_INVALID;
    ARM64_CONSOLE_STATION *station;
    OBJECT_HANDLE_INFORMATION info;
    NTSTATUS status = ObReferenceObjectByHandle(station_handle, WINSTA_CREATEDESKTOP, ExWindowStationObjectType,
                                                KernelMode, (PVOID *)&station, &info);
    if (!NT_SUCCESS(status)) return status;
    if (!(info.GrantedAccess & WINSTA_CREATEDESKTOP)) {
        ObDereferenceObject(station);
        return STATUS_ACCESS_DENIED;
    }

    console_lock_acquire();
    ARM64_CONSOLE_DESKTOP *desktop = find_desktop(station, name, TRUE);
    if (desktop) {
        console_lock_release();
        ObDereferenceObject(desktop);
        ObDereferenceObject(station);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    ULONG size = FIELD_OFFSET(ARM64_CONSOLE_DESKTOP, NameBuffer) + name->Length + sizeof(WCHAR);
    status = ObCreateObject(KernelMode, ExDesktopObjectType, &attributes, KernelMode, NULL,
                            size, 0, 0, (PVOID *)&desktop);
    if (!NT_SUCCESS(status)) {
        console_lock_release();
        ObDereferenceObject(station);
        return status;
    }
    RtlZeroMemory(desktop, size);
    desktop->Name.Buffer = desktop->NameBuffer;
    desktop->Name.MaximumLength = name->Length + sizeof(WCHAR);
    RtlCopyUnicodeString(&desktop->Name, name);
    desktop->Station = station; /* transfer the station reference */
    status = ObInsertObject(desktop, NULL, GENERIC_ALL, 0, NULL, handle);
    if (NT_SUCCESS(status)) {
        desktop->Next = station->Desktops;
        desktop->Published = TRUE;
        station->Desktops = desktop;
    }
    console_lock_release();
    return status;
}
