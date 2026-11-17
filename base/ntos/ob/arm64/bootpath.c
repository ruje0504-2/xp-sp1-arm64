/* Trusted kernel namespace operations before the Se/Ps phases. */
#include "obp.h"
#include "bootobject.h"

static NTSTATUS copy_lookup_name(PCUNICODE_STRING name, PUNICODE_STRING path)
{
    if ((name->Length & 1) || name->Length > name->MaximumLength ||
        name->Length > 0xfffc || (name->Length && !name->Buffer))
        return STATUS_OBJECT_NAME_INVALID;
    if (name->Length) {
        path->MaximumLength = name->Length + sizeof(WCHAR);
        path->Buffer = ExAllocatePoolWithTag(PagedPool, path->MaximumLength, 'mNbO');
        if (!path->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyUnicodeString(path, name);
    }
    return STATUS_SUCCESS;
}

NTSTATUS ObArm64LookupBootObjectName(HANDLE root, PCUNICODE_STRING name, ULONG attributes,
                                    POBJECT_TYPE type, PVOID *object)
{
    if (!object) return STATUS_INVALID_PARAMETER;
    *object = NULL;
    if (!name || !ObpRootDirectoryObject || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (attributes & ~(OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE))
        return STATUS_NOT_SUPPORTED;
    UNICODE_STRING path = {0};
    NTSTATUS status = copy_lookup_name(name, &path);
    if (!NT_SUCCESS(status)) return status;

    OBP_LOOKUP_CONTEXT context;
    PVOID found = NULL;
    status = ObpLookupObjectName(root, &path, attributes, type, KernelMode,
                                NULL, NULL, NULL, NULL, &context, &found);
    ObpReleaseLookupContext(&context);
    if (path.Buffer) ExFreePool(path.Buffer);
    if (!NT_SUCCESS(status)) return status;
    if (!found) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (type && OBJECT_TO_OBJECT_HEADER(found)->Type != type) {
        ObDereferenceObject(found);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    *object = found;
    return STATUS_SUCCESS;
}

static NTSTATUS create_table_handle(PEPROCESS process, PHANDLE_TABLE table, PVOID object, ACCESS_MASK access,
                                    BOOLEAN kernel_handle, PHANDLE handle)
{
    if (!table) return STATUS_PROCESS_IS_TERMINATING;
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
    POBJECT_TYPE type = header->Type;
    RtlMapGenericMask(&access, &type->TypeInfo.GenericMapping);
    if (access & ~type->TypeInfo.ValidAccessMask)
        return STATUS_ACCESS_DENIED;
    if (type->TypeInfo.MaintainHandleCount)
        return STATUS_NOT_SUPPORTED;

    HANDLE_TABLE_ENTRY entry = {0};
    entry.Object = header;
    entry.GrantedAccess = access;
    ObpLockObject(header);
    InterlockedIncrement(&header->HandleCount);
    if (type->TypeInfo.OpenProcedure) {
        ObpUnlockObject(header);
        KIRQL saved_irql;
        ObpBeginTypeSpecificCallOut(saved_irql);
        NTSTATUS status = type->TypeInfo.OpenProcedure(ObOpenHandle, process, object, access, 0);
        ObpEndTypeSpecificCallOut(saved_irql, "Open", type, object);
        if (!NT_SUCCESS(status)) {
            ObpLockObject(header);
            InterlockedDecrement(&header->HandleCount);
            ObpUnlockObject(header);
            return status;
        }
        ObpLockObject(header);
    }
    ObReferenceObject(object);
    ULONG total = (ULONG)InterlockedIncrement((PLONG)&type->TotalNumberOfHandles);
    HANDLE index = ExCreateHandle(table, &entry);
    ULONG system_count = header->HandleCount;
    if (!index) {
        InterlockedDecrement(&header->HandleCount);
        InterlockedDecrement((PLONG)&type->TotalNumberOfHandles);
    } else if (total > type->HighWaterNumberOfHandles) {
        type->HighWaterNumberOfHandles = total;
    }
    ObpUnlockObject(header);
    if (!index) {
        /* No handle was published. Balance the successful open before the
         * caller rolls back any name that it owns in its lookup context. */
        if (type->TypeInfo.CloseProcedure) {
            KIRQL saved_irql;
            ObpBeginTypeSpecificCallOut(saved_irql);
            type->TypeInfo.CloseProcedure(process, object, access, 0, system_count);
            ObpEndTypeSpecificCallOut(saved_irql, "Close", type, object);
        }
        ObDereferenceObject(object);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *handle = kernel_handle ? EncodeKernelHandle(index) : index;
    return STATUS_SUCCESS;
}

NTSTATUS ObArm64CreateBootHandle(PVOID object, ACCESS_MASK access, BOOLEAN kernel_handle, PHANDLE handle)
{
    PHANDLE_TABLE table = kernel_handle ? ObpKernelHandleTable : PsGetCurrentProcess()->ObjectTable;
    return create_table_handle(PsGetCurrentProcess(), table, object, access, kernel_handle, handle);
}

NTSTATUS ObArm64CreateProcessHandle(PEPROCESS process, PVOID object, ACCESS_MASK access, PHANDLE handle)
{
    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = NULL;
    if (!object || !process || KeGetCurrentIrql() != PASSIVE_LEVEL || KeGetPreviousMode() != KernelMode)
        return STATUS_INVALID_PARAMETER;
    PHANDLE_TABLE table = ObReferenceProcessHandleTable(process);
    if (!table) return STATUS_PROCESS_IS_TERMINATING;
    NTSTATUS status = create_table_handle(process, table, object, access, FALSE, handle);
    ObDereferenceProcessHandleTable(process);
    return status;
}

NTSTATUS ObArm64OpenBootObjectByName(HANDLE root, PCUNICODE_STRING name, ULONG attributes,
                                    POBJECT_TYPE type, ACCESS_MASK access, PHANDLE handle)
{
    if (!handle) return STATUS_INVALID_PARAMETER;
    *handle = NULL;
    PVOID object;
    NTSTATUS status = ObArm64LookupBootObjectName(root, name, attributes, type, &object);
    if (!NT_SUCCESS(status)) return status;
    status = ObArm64CreateBootHandle(object, access, TRUE, handle);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS ObArm64AllocateBootServiceObject(POBJECT_ATTRIBUTES attributes, POBJECT_TYPE type,
                                         ULONG size, PVOID *object)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (KeGetPreviousMode() != KernelMode) return STATUS_NOT_SUPPORTED;
    if (attributes && attributes->Length != sizeof(*attributes)) return STATUS_INVALID_PARAMETER;
    if (attributes && attributes->ObjectName) {
        if (attributes->SecurityQualityOfService || PsGetCurrentThread()->ImpersonationInfo ||
            (attributes->Attributes & ~(OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT | OBJ_OPENIF)))
            return STATUS_NOT_SUPPORTED;
        return ObArm64AllocateBootUnicodeObject(attributes->ObjectName, type, size, object);
    }
    return ObCreateObject(KernelMode, type, attributes, KernelMode, NULL, size, 0, 0, object);
}

NTSTATUS ObArm64InsertBootServiceObject(POBJECT_ATTRIBUTES attributes, PVOID object,
                                       ACCESS_MASK access, PHANDLE handle)
{
    if (!attributes || !attributes->ObjectName)
        return ObInsertObject(object, NULL, access, 0, NULL, handle);

    POBJECT_TYPE type = OBJECT_TO_OBJECT_HEADER(object)->Type;
    PSECURITY_DESCRIPTOR relative;
    NTSTATUS status = ObArm64CaptureBootTokenSecurity(type, attributes->SecurityDescriptor, &relative);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(object);
        return status;
    }
    return ObArm64InsertBootObject(attributes->RootDirectory, object,
                                   attributes->Attributes, relative, access, handle);
}

NTSTATUS ObArm64InsertBootObject(HANDLE root, PVOID object, ULONG attributes,
                                 PSECURITY_DESCRIPTOR descriptor, ACCESS_MASK access, PHANDLE handle)
{
    if (handle) *handle = NULL;
    if (!object || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
    POBJECT_HEADER_NAME_INFO name = OBJECT_HEADER_TO_NAME_INFO(header);
    if (!name || name->Directory || header->ObjectCreateInfo || header->SecurityDescriptor ||
        (header->Flags & (OB_FLAG_NEW_OBJECT | OB_FLAG_KERNEL_OBJECT | OB_FLAG_PERMANENT_OBJECT)) !=
            (OB_FLAG_NEW_OBJECT | OB_FLAG_KERNEL_OBJECT))
        return STATUS_INVALID_PARAMETER;

    POBJECT_TYPE type = header->Type;
    UNICODE_STRING path = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    if (!handle || !name->Name.Length) goto done;

    status = STATUS_NOT_SUPPORTED;
    if (!ObpRootDirectoryObject || !ObpKernelHandleTable ||
        (attributes & ~(OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_PERMANENT)) ||
        type->TypeInfo.SecurityProcedure != SeDefaultObjectMethod ||
        (type->TypeInfo.SecurityRequired && !descriptor))
        goto done;

    status = copy_lookup_name(&name->Name, &path);
    if (!NT_SUCCESS(status)) goto done;

    OBP_LOOKUP_CONTEXT context;
    PVOID found;
    status = ObpLookupObjectName(root, &path, attributes, type, KernelMode,
                                NULL, NULL, object, NULL, &context, &found);
    BOOLEAN inserted = NT_SUCCESS(status) && found == object;
    /* Root shortcuts and parse callbacks return an independent reference. */
    BOOLEAN extra_reference = NT_SUCCESS(status) && !inserted && context.Object != found;
    if (inserted) {
        /* The original parser holds the final directory exclusively until
         * both security and the handle are ready. */
        if (type == ObpSymbolicLinkObjectType && name->Directory->DeviceMap) {
            status = STATUS_NOT_SUPPORTED;
        } else {
            status = type->TypeInfo.SecurityProcedure(object, AssignSecurityDescriptor,
                        NULL, descriptor, NULL, &header->SecurityDescriptor,
                        type->TypeInfo.PoolType, &type->TypeInfo.GenericMapping);
        }
        if (NT_SUCCESS(status)) descriptor = NULL;
    } else if (NT_SUCCESS(status)) {
        if (!(attributes & OBJ_OPENIF))
            status = STATUS_OBJECT_NAME_COLLISION;
        else if (OBJECT_TO_OBJECT_HEADER(found)->Type != type)
            status = STATUS_OBJECT_TYPE_MISMATCH;
        else
            status = STATUS_OBJECT_NAME_EXISTS;
    }

    if (NT_SUCCESS(status)) {
        NTSTATUS create_status = ObArm64CreateBootHandle(found, access,
                                                        (attributes & OBJ_KERNEL_HANDLE) != 0, handle);
        if (!NT_SUCCESS(create_status)) status = create_status;
        else if (inserted && attributes & OBJ_PERMANENT) {
            ObpLockObject(header);
            header->Flags |= OB_FLAG_PERMANENT_OBJECT;
            ObpUnlockObject(header);
        }
    }
    if (inserted && !NT_SUCCESS(status)) {
        if (!ObpDeleteDirectoryEntry(&context))
            KeBugCheckEx(OBJECT_INITIALIZATION_FAILED, (ULONG_PTR)object, status, 0, 0);
        ObpDereferenceNameInfo(name);
        ObDereferenceObject(object);
    }
    ObpReleaseLookupContext(&context);
    if (extra_reference) ObDereferenceObject(found);

done:
    if (path.Buffer) ExFreePool(path.Buffer);
    if (descriptor) ExFreePool(descriptor);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS ObArm64MakeBootObjectTemporary(HANDLE handle)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (KeGetPreviousMode() != KernelMode) return STATUS_NOT_SUPPORTED;
    OBJECT_HANDLE_INFORMATION info;
    PVOID object;
    NTSTATUS status = ObReferenceObjectByHandle(handle, DELETE, NULL, KernelMode, &object, &info);
    if (!NT_SUCCESS(status)) return status;
    if (info.GrantedAccess & DELETE) {
        POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
        ObpLockObject(header);
        header->Flags &= ~OB_FLAG_PERMANENT_OBJECT;
        ObpUnlockObject(header);
    } else {
        status = STATUS_ACCESS_DENIED;
    }
    ObDereferenceObject(object);
    return status;
}

static NTSTATUS close_locked_entry(PEPROCESS process, PHANDLE_TABLE table, PHANDLE_TABLE_ENTRY entry,
                                   HANDLE index, HANDLE original_handle, KPROCESSOR_MODE mode, BOOLEAN rundown)
{
    POBJECT_HEADER header = (PVOID)((ULONG_PTR)entry->Object & ~(ULONG_PTR)OBJ_HANDLE_ATTRIBUTES);
    POBJECT_TYPE type = header->Type;
    if (!rundown && type->TypeInfo.OkayToCloseProcedure) {
        KIRQL saved_irql;
        ObpBeginTypeSpecificCallOut(saved_irql);
        BOOLEAN okay = type->TypeInfo.OkayToCloseProcedure(process, &header->Body, original_handle, mode);
        ObpEndTypeSpecificCallOut(saved_irql, "Close", type, &header->Body);
        if (!okay) {
            ExUnlockHandleTableEntry(table, entry);
            return STATUS_HANDLE_NOT_CLOSABLE;
        }
    }
    if (!rundown && (ObpGetHandleAttributes(entry) & OBJ_PROTECT_CLOSE)) {
        ExUnlockHandleTableEntry(table, entry);
        return STATUS_HANDLE_NOT_CLOSABLE;
    }
    ACCESS_MASK access = ObpDecodeGrantedAccess(entry->GrantedAccess);
    if (!ExDestroyHandle(table, index, entry))
        KeBugCheckEx(BAD_EXHANDLE, (ULONG_PTR)index, (ULONG_PTR)entry, 0, 0);
    ObpDecrementHandleCount(process, header, type, access);
    ObDereferenceObject(&header->Body);
    return STATUS_SUCCESS;
}

BOOLEAN ObArm64SweepProcessHandle(PHANDLE_TABLE_ENTRY entry, HANDLE index, PVOID context)
{
    PEPROCESS process = context;
    close_locked_entry(process, process->ObjectTable, entry, index, index, KernelMode, TRUE);
    return TRUE;
}

static NTSTATUS close_handle(HANDLE handle, POBJECT_TYPE expected_type, KPROCESSOR_MODE mode)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    PEPROCESS process = PsGetCurrentProcess();
    PHANDLE_TABLE table = process->ObjectTable;
    HANDLE index = handle;
    if (IsKernelHandle(handle, mode)) {
        table = ObpKernelHandleTable;
        index = DecodeKernelHandle(handle);
    }
    if (!table || (ULONG_PTR)index > MAXLONG || ((ULONG_PTR)index & 3))
        return STATUS_INVALID_HANDLE;
    KeEnterCriticalRegion();
    PHANDLE_TABLE_ENTRY entry = ExMapHandleToPointer(table, index);
    NTSTATUS status = STATUS_INVALID_HANDLE;
    if (entry) {
        POBJECT_HEADER header = (PVOID)((ULONG_PTR)entry->Object & ~(ULONG_PTR)OBJ_HANDLE_ATTRIBUTES);
        if (expected_type && header->Type != expected_type) {
            ExUnlockHandleTableEntry(table, entry);
            status = STATUS_OBJECT_TYPE_MISMATCH;
        } else status = close_locked_entry(process, table, entry, index, handle, mode, FALSE);
    }
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS ObArm64CloseBootObjectHandle(HANDLE handle)
{
    if (KeGetPreviousMode() != KernelMode) return STATUS_INVALID_DEVICE_STATE;
    return close_handle(handle, NULL, KernelMode);
}

NTSTATUS ObArm64CloseUserHandle(HANDLE handle, POBJECT_TYPE type)
{
    return close_handle(handle, type, UserMode);
}
