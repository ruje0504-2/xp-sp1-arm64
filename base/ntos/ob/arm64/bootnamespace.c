/* Kernel-owned directories before the security and system-service phases. */
#include "obp.h"
#include "bootobject.h"

NTSTATUS ObArm64InsertBootDirectoryEntry(POBJECT_DIRECTORY directory, POBJECT_HEADER header)
{
    if (!header) return STATUS_INVALID_PARAMETER;
    POBJECT_HEADER_NAME_INFO name = OBJECT_HEADER_TO_NAME_INFO(header);
    if (!directory || !name || !name->Name.Length || !name->Name.Buffer || name->Directory ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    OBP_LOOKUP_CONTEXT context;
    ObpInitializeLookupContext(&context);
    ObpLockLookupContext(&context, directory);
    NTSTATUS status;
    if (ObpLookupDirectoryEntry(directory, &name->Name, OBJ_CASE_INSENSITIVE, FALSE, &context)) {
        status = STATUS_OBJECT_NAME_COLLISION;
    } else if (ObpInsertDirectoryEntry(directory, &context, header)) {
        /* The name now owns a directory reference. The caller transfers
         * the existing object reference to the directory entry. */
        ObReferenceObject(directory);
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    ObpReleaseLookupContext(&context);
    return status;
}

PVOID ObArm64LookupBootDirectoryEntry(POBJECT_DIRECTORY directory, PUNICODE_STRING name,
                                      ULONG attributes)
{
    if (KeGetCurrentIrql() > APC_LEVEL) return NULL;

    OBP_LOOKUP_CONTEXT context;
    ObpInitializeLookupContext(&context);
    PVOID object = ObpLookupDirectoryEntry(directory, name, attributes, FALSE, &context);
    if (object) ObReferenceObject(object);
    ObpReleaseLookupContext(&context);
    return object;
}

NTSTATUS ObArm64RemoveBootDirectoryEntry(POBJECT_DIRECTORY directory, PUNICODE_STRING name)
{
    if (!directory || !name || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    OBP_LOOKUP_CONTEXT context;
    ObpInitializeLookupContext(&context);
    ObpLockLookupContext(&context, directory);
    PVOID object = ObpLookupDirectoryEntry(directory, name, OBJ_CASE_INSENSITIVE, FALSE, &context);
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;
    if (object) {
        POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
        if (header->Flags & OB_FLAG_PERMANENT_OBJECT) {
            status = STATUS_ACCESS_DENIED;
        } else if (header->Type == ObpDirectoryObjectType) {
            status = STATUS_NOT_SUPPORTED;
        } else if (ObpDeleteDirectoryEntry(&context)) {
            ObpDereferenceNameInfo(OBJECT_HEADER_TO_NAME_INFO(header));
            ObDereferenceObject(object);
            status = STATUS_SUCCESS;
        }
    }
    ObpReleaseLookupContext(&context);
    return status;
}

NTSTATUS ObArm64AllocateBootUnicodeObject(PCUNICODE_STRING source, POBJECT_TYPE type,
                                          ULONG size, PVOID *result)
{
    *result = NULL;
    if (!source || !source->Length || (source->Length & 1) ||
        source->Length > source->MaximumLength || source->Length > 0xfffc || !source->Buffer)
        return STATUS_OBJECT_NAME_INVALID;
    UNICODE_STRING name;
    name.MaximumLength = source->Length + sizeof(WCHAR);
    name.Buffer = ExAllocatePoolWithTag(PagedPool, name.MaximumLength, 'mNbO');
    if (!name.Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyUnicodeString(&name, source);

    POBJECT_HEADER header;
    NTSTATUS status = ObpAllocateObject(NULL, KernelMode, type, &name, size, &header);
    if (!NT_SUCCESS(status)) {
        ExFreePool(name.Buffer);
        return status;
    }
    *result = &header->Body;
    RtlZeroMemory(*result, size);
    return STATUS_SUCCESS;
}

NTSTATUS ObArm64AllocateBootNamedObject(PCWSTR text, POBJECT_TYPE type, ULONG size, PVOID *result)
{
    UNICODE_STRING source;
    RtlInitUnicodeString(&source, text);
    return ObArm64AllocateBootUnicodeObject(&source, type, size, result);
}

NTSTATUS ObArm64CreateBootDirectory(PCWSTR text, POBJECT_DIRECTORY *result)
{
    NTSTATUS status = ObArm64AllocateBootNamedObject(text, ObpDirectoryObjectType,
                                                    sizeof(**result), (PVOID *)result);
    if (NT_SUCCESS(status)) ExInitializePushLock(&(*result)->Lock);
    return status;
}

/* Early failures occur before either directory is published. The type
 * objects keep their original names and references after rollback. */
static VOID detach_unpublished_entries(POBJECT_DIRECTORY directory)
{
    for (ULONG i = 0; i < NUMBER_HASH_BUCKETS; ++i) {
        POBJECT_DIRECTORY_ENTRY entry = directory->HashBuckets[i];
        directory->HashBuckets[i] = NULL;
        while (entry) {
            POBJECT_DIRECTORY_ENTRY next = entry->ChainLink;
            POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(entry->Object);
            OBJECT_HEADER_TO_NAME_INFO(header)->Directory = NULL;
            ObDereferenceObject(directory);
            ExFreePool(entry);
            entry = next;
        }
    }
}

NTSTATUS ObArm64InitializeBootNamespace(VOID)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !ObpDirectoryObjectType ||
        ObpRootDirectoryObject || ObpTypeDirectoryObject)
        return STATUS_INVALID_DEVICE_STATE;

    ExInitializeWorkItem(&ObpRemoveObjectWorkItem, ObpProcessRemoveObjectQueue, NULL);
    POBJECT_DIRECTORY root, types;
    NTSTATUS status = ObArm64CreateBootDirectory(L"\\", &root);
    if (!NT_SUCCESS(status)) return status;
    status = ObArm64CreateBootDirectory(L"ObjectTypes", &types);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(root);
        return status;
    }
    status = ObArm64InsertBootDirectoryEntry(root, OBJECT_TO_OBJECT_HEADER(types));
    PLIST_ENTRY head = &ObpTypeObjectType->TypeList;
    for (PLIST_ENTRY link = head->Flink; NT_SUCCESS(status) && link != head; link = link->Flink) {
        POBJECT_HEADER_CREATOR_INFO creator = CONTAINING_RECORD(link, OBJECT_HEADER_CREATOR_INFO, TypeList);
        status = ObArm64InsertBootDirectoryEntry(types, (POBJECT_HEADER)(creator + 1));
    }
    if (!NT_SUCCESS(status)) {
        detach_unpublished_entries(types);
        detach_unpublished_entries(root);
        ObDereferenceObject(types);
        ObDereferenceObject(root);
        return status;
    }
    OBJECT_TO_OBJECT_HEADER(root)->Flags |= OB_FLAG_PERMANENT_OBJECT;
    OBJECT_TO_OBJECT_HEADER(types)->Flags |= OB_FLAG_PERMANENT_OBJECT;
    ObReferenceObject(types);
    ObpTypeDirectoryObject = types;
    ObpRootDirectoryObject = root;
    return STATUS_SUCCESS;
}
