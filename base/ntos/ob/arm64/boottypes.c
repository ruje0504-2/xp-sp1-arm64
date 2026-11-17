/* Initial object types in the order used by the XP ObInitSystem Phase 0. */
#include "bootobject.h"

POBJECT_TYPE ObpTypeObjectType, ObpDirectoryObjectType, ObpSymbolicLinkObjectType;
POBJECT_DIRECTORY ObpRootDirectoryObject, ObpTypeDirectoryObject;
WORK_QUEUE_ITEM ObpRemoveObjectWorkItem;
/* Device maps are created by the later object-manager initialization. */
ULONG ObpLUIDDeviceMapsEnabled;
ULONG ObpCaseInsensitive = 1;
ULONG ObpAccessProtectCloseBit = MAXIMUM_ALLOWED;
ULONG NtGlobalFlag = FLG_POOL_ENABLE_TAGGING;

extern const GENERIC_MAPPING ObpTypeMapping, ObpDirectoryMapping, ObpSymbolicLinkMapping;
VOID ObpDeleteObjectType(PVOID Object);
VOID ObpDeleteSymbolicLink(PVOID Object);
NTSTATUS ObpParseSymbolicLink(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
    KPROCESSOR_MODE AccessMode, ULONG Attributes, PUNICODE_STRING CompleteName,
    PUNICODE_STRING RemainingName, PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos,
    PVOID *Object);

NTSTATUS ObArm64InitializeBootObjectTypes(VOID)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !ObpKernelHandleTable || ObpTypeObjectType)
        return STATUS_INVALID_DEVICE_STATE;

    OBJECT_TYPE_INITIALIZER info = {0};
    info.Length = sizeof(info);
    info.InvalidAttributes = OBJ_OPENLINK;
    info.PoolType = NonPagedPool;
    info.DefaultNonPagedPoolCharge = sizeof(OBJECT_TYPE);
    info.ValidAccessMask = OBJECT_TYPE_ALL_ACCESS;
    info.GenericMapping = ObpTypeMapping;
    info.MaintainTypeList = TRUE;
    info.UseDefaultObject = TRUE;
    info.DeleteProcedure = ObpDeleteObjectType;

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"Type");
    NTSTATUS status = ObCreateObjectType(&name, &info, NULL, &ObpTypeObjectType);
    if (!NT_SUCCESS(status)) return status;

    RtlInitUnicodeString(&name, L"Directory");
    info.PoolType = PagedPool; /* OB_NAMESPACE_POOL_TYPE in the XP object build. */
    info.DefaultNonPagedPoolCharge = sizeof(OBJECT_DIRECTORY);
    info.ValidAccessMask = DIRECTORY_ALL_ACCESS;
    info.GenericMapping = ObpDirectoryMapping;
    info.CaseInsensitive = TRUE;
    info.MaintainTypeList = FALSE;
    info.DeleteProcedure = NULL;
    status = ObCreateObjectType(&name, &info, NULL, &ObpDirectoryObjectType);
    if (!NT_SUCCESS(status)) return status;
    ObpDirectoryObjectType->TypeInfo.ValidAccessMask &= ~SYNCHRONIZE;

    RtlInitUnicodeString(&name, L"SymbolicLink");
    info.DefaultNonPagedPoolCharge = sizeof(OBJECT_SYMBOLIC_LINK);
    info.ValidAccessMask = SYMBOLIC_LINK_ALL_ACCESS;
    info.GenericMapping = ObpSymbolicLinkMapping;
    info.DeleteProcedure = ObpDeleteSymbolicLink;
    info.ParseProcedure = ObpParseSymbolicLink;
    status = ObCreateObjectType(&name, &info, NULL, &ObpSymbolicLinkObjectType);
    if (!NT_SUCCESS(status)) return status;
    ObpSymbolicLinkObjectType->TypeInfo.ValidAccessMask &= ~SYNCHRONIZE;
    return STATUS_SUCCESS;
}
