#include "../base/ntos/ob/arm64/bootobject.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/mm/arm64/bootframes.h"
#include "../base/ntos/mm/arm64/bootpool.h"
#include "../base/ntos/ps/arm64/bootexit.h"
#include "xp-obcache.h"
#include "uefi_test.h"
#include <seopaque.h>

extern EPROCESS_QUOTA_BLOCK PspDefaultQuotaBlock;
extern LIST_ENTRY PspQuotaBlockList, HandleTableListHead;
extern ULONG ExPoolFailures;
VOID SepAssemblePrivileges(ULONG Count, BOOLEAN SystemSecurity, BOOLEAN WriteOwner,
                           PPRIVILEGE_SET *Privileges);
VOID SepTokenDeleteMethod(PVOID Token);

typedef struct _XP_TEST_DESCRIPTOR {
    SECURITY_DESCRIPTOR_RELATIVE Descriptor;
    SID Owner;
    ACL Dacl;
} XP_TEST_DESCRIPTOR;

static VOID init_descriptor(XP_TEST_DESCRIPTOR *data, ULONG rid)
{
    RtlZeroMemory(data, sizeof(*data));
    data->Descriptor.Revision = SECURITY_DESCRIPTOR_REVISION;
    data->Descriptor.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
    data->Descriptor.Owner = FIELD_OFFSET(XP_TEST_DESCRIPTOR, Owner);
    data->Descriptor.Dacl = FIELD_OFFSET(XP_TEST_DESCRIPTOR, Dacl);
    data->Owner.Revision = SID_REVISION;
    data->Owner.SubAuthorityCount = 1;
    data->Owner.IdentifierAuthority.Value[5] = 5;
    data->Owner.SubAuthority[0] = rid;
    data->Dacl.AclRevision = ACL_REVISION;
    data->Dacl.AclSize = sizeof(data->Dacl);
}

static VOID check_empty_security_cache(VOID)
{
    for (ULONG i = 0; i < SECURITY_DESCRIPTOR_CACHE_ENTRIES; ++i) {
        OB_SD_CACHE_LIST *entry = &ObsSecurityDescriptorCache[i];
        CHECK(!entry->PushLock.Value && entry->Head.Flink == &entry->Head &&
              entry->Head.Blink == &entry->Head);
    }
}

static VOID check_security_cache(VOID)
{
    check_empty_security_cache();
    XP_TEST_DESCRIPTOR first, same, different;
    init_descriptor(&first, 18);
    init_descriptor(&same, 18);
    init_descriptor(&different, 19);
    CHECK(RtlLengthSecurityDescriptor(&first) == sizeof(first));
    PSECURITY_DESCRIPTOR a = NULL, b = NULL, c = NULL;
    CHECK(ObLogSecurityDescriptor(&first, &a, 2) == STATUS_SUCCESS);
    CHECK(ObLogSecurityDescriptor(&same, &b, 1) == STATUS_SUCCESS);
    CHECK(ObLogSecurityDescriptor(&different, &c, 1) == STATUS_SUCCESS);
    CHECK(a && a == b && c && c != a &&
          (ULONG_PTR)a - MM_BOOT_PAGED_POOL_BASE < MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE);
    CHECK(RtlEqualMemory(a, &first, sizeof(first)) && RtlEqualMemory(c, &different, sizeof(different)));
    CHECK(MmArm64QueryPhysicalAddress((ULONG_PTR)a) != ~0ULL &&
          MmArm64QueryPhysicalAddress((ULONG_PTR)c) != ~0ULL);
    PSECURITY_DESCRIPTOR_HEADER header = CONTAINING_RECORD(a, SECURITY_DESCRIPTOR_HEADER, SecurityDescriptor);
    PSECURITY_DESCRIPTOR_HEADER distinct = CONTAINING_RECORD(c, SECURITY_DESCRIPTOR_HEADER, SecurityDescriptor);
    CHECK(header->RefCount == 3 && distinct->RefCount == 1 && header->FullHash != distinct->FullHash);
    POB_SD_CACHE_LIST bucket = &ObsSecurityDescriptorCache[header->FullHash % SECURITY_DESCRIPTOR_CACHE_ENTRIES];
    CHECK(bucket->Head.Flink == &header->Link && bucket->Head.Blink == &header->Link);
    ObReferenceSecurityDescriptor(a, 1);
    CHECK(header->RefCount == 4);
    ObDereferenceSecurityDescriptor(a, 1);
    CHECK(header->RefCount == 3 && bucket->Head.Flink == &header->Link);
    ObDereferenceSecurityDescriptor(b, 3);
    CHECK(IsListEmpty(&bucket->Head));
    ObDereferenceSecurityDescriptor(c, 1);
    check_empty_security_cache();
}

static VOID check_object_lookaside(ULONG index, PGENERAL_LOOKASIDE list, POOL_TYPE type)
{
    PKPRCB prcb = KeGetCurrentPrcb();
    CHECK(prcb->PPLookasideList[index].P == list && prcb->PPLookasideList[index].L == list &&
          list->Type == type && !QueryDepthSList(&list->ListHead));
    PVOID pointer = ExAllocateFromPPLookasideList(index);
    ULONG_PTR base = type == PagedPool ? MM_BOOT_PAGED_POOL_BASE : MM_BOOT_POOL_BASE;
    CHECK((ULONG_PTR)pointer - base < MM_BOOT_POOL_PAGES * PAGE_SIZE);
    ExFreeToPPLookasideList(index, pointer);
    CHECK(QueryDepthSList(&list->ListHead) == 1 && ExAllocateFromPPLookasideList(index) == pointer);
    list->Free(pointer);
    CHECK(!QueryDepthSList(&list->ListHead));
}

VOID XpCheckBootObjectState(VOID)
{
    PEPROCESS process = PsGetCurrentProcess();
    PHANDLE_TABLE table = ObpKernelHandleTable;
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && table && process->ObjectTable == table);
    CHECK(HandleTableListHead.Flink == &table->HandleTableList &&
          HandleTableListHead.Blink == &table->HandleTableList);
    CHECK(process->QuotaBlock == &PspDefaultQuotaBlock && PspDefaultQuotaBlock.ProcessCount == 1 &&
          PspDefaultQuotaBlock.ReferenceCount == 1 && IsListEmpty(&PspQuotaBlockList));
    for (ULONG i = 0; i < PsQuotaTypes; ++i) {
        CHECK(PspDefaultQuotaBlock.QuotaEntry[i].Limit == (SIZE_T)-1 &&
              !PspDefaultQuotaBlock.QuotaEntry[i].Usage);
    }
    PETHREAD idle = CONTAINING_RECORD(KeGetCurrentPrcb()->IdleThread, ETHREAD, Tcb);
    CHECK(process->GrantedAccess == PROCESS_ALL_ACCESS && idle->GrantedAccess == THREAD_ALL_ACCESS);
    CHECK(ObpDefaultObject.Header.Type == NotificationEvent && ObpDefaultObject.Header.SignalState == 1 &&
          !ObpLock.Value && !ObpRemoveObjectList && ObpDeviceMapLock.Count == 1 && !ObpDeviceMapLock.Owner);
    CHECK(ExSystemLookasideListHead.Flink == &ObpCreateInfoLookasideList.ListEntry &&
          ObpCreateInfoLookasideList.ListEntry.Flink == &ObpNameBufferLookasideList.ListEntry &&
          ExSystemLookasideListHead.Blink == &ObpNameBufferLookasideList.ListEntry);
    CHECK(ObpCreateInfoLookasideList.Size == sizeof(OBJECT_CREATE_INFORMATION) &&
          ObpNameBufferLookasideList.Size == OBJECT_NAME_BUFFER_SIZE &&
          ObpCreateInfoLookasideList.MaximumDepth == 32 && ObpNameBufferLookasideList.MaximumDepth == 16);
    ULONG_PTR descriptor = (ULONG_PTR)PAGE_ALIGN(table);
    ULONG_PTR entries = table->TableCode;
    ULONG64 descriptor_pa = MmArm64QueryPhysicalAddress(descriptor);
    ULONG64 entries_pa = MmArm64QueryPhysicalAddress(entries);
    CHECK(descriptor == MM_BOOT_PAGED_POOL_BASE && entries == descriptor + PAGE_SIZE &&
          descriptor_pa != ~0ULL && entries_pa != ~0ULL && descriptor_pa != entries_pa &&
          !table->HandleCount);

    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    UNICODE_STRING pathname;
    RtlInitUnicodeString(&pathname, L"\\ObjectTypes");
    HANDLE handle;
    CHECK(ObArm64OpenBootObjectByName(NULL, &pathname, OBJ_KERNEL_HANDLE, ObpDirectoryObjectType,
                                     DIRECTORY_QUERY, &handle) == STATUS_SUCCESS);
    CHECK(((ULONG_PTR)handle & MAXLONG) == 4 && table->HandleCount == 1);
    PVOID entry = NULL;
    OBJECT_HANDLE_INFORMATION information;
    CHECK(ObReferenceObjectByHandle(handle, DIRECTORY_QUERY, ObpDirectoryObjectType, KernelMode,
          &entry, &information) == STATUS_SUCCESS && entry == ObpTypeDirectoryObject &&
          information.GrantedAccess == DIRECTORY_QUERY);
    ObDereferenceObject(entry);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS && !table->HandleCount);
    check_security_cache();
    check_object_lookaside(LookasideCreateInfoList, &ObpCreateInfoLookasideList, NonPagedPool);
    check_object_lookaside(LookasideNameBufferList, &ObpNameBufferLookasideList, PagedPool);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Object bootstrap evidence table_pages="); XpTestHex((entries - descriptor) / PAGE_SIZE + 1);
    XpTestReport(" handle="); XpTestHex((ULONG_PTR)handle & MAXLONG);
    XpTestReport(" buckets="); XpTestHex(SECURITY_DESCRIPTOR_CACHE_ENTRIES);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: boot object state publishes the kernel handle table and runs original security descriptor caching\n");
}

extern POBJECT_TYPE ObpObjectTypes[];
extern POBJECT_DIRECTORY ObpTypeDirectoryObject;
NTSTATUS ObpAllocateObject(POBJECT_CREATE_INFORMATION CreateInfo, KPROCESSOR_MODE OwnershipMode,
    POBJECT_TYPE Type, PUNICODE_STRING Name, ULONG BodySize, POBJECT_HEADER *Header);
VOID FASTCALL ObpFreeObject(PVOID Object);

static UNICODE_STRING copy_name(PCWSTR text)
{
    UNICODE_STRING original, copy;
    RtlInitUnicodeString(&original, text);
    copy.MaximumLength = original.MaximumLength;
    copy.Buffer = ExAllocatePoolWithTag(PagedPool, copy.MaximumLength, 'mNbO');
    CHECK(copy.Buffer);
    RtlCopyUnicodeString(&copy, &original);
    return copy;
}

static BOOLEAN equal_name(PCUNICODE_STRING name, PCWSTR value)
{
    UNICODE_STRING expected;
    RtlInitUnicodeString(&expected, value);
    return name->Length == expected.Length &&
           RtlEqualMemory(name->Buffer, expected.Buffer, expected.Length);
}

VOID XpCheckBootObjectTypes(VOID)
{
    POBJECT_TYPE types[] = {ObpTypeObjectType, ObpDirectoryObjectType, ObpSymbolicLinkObjectType,
                           SeTokenObjectType, PsProcessType, PsThreadType};
    PCWSTR names[] = {L"Type", L"Directory", L"SymbolicLink", L"Token", L"Process", L"Thread"};
    CHECK(types[0] && ObpTypeDirectoryObject);
    PLIST_ENTRY creator_link = types[0]->TypeList.Flink;
    for (ULONG i = 0; i < (sizeof(types) / sizeof(*types)); ++i) {
        CHECK(types[i]);
        POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(types[i]);
        POBJECT_HEADER_NAME_INFO name = OBJECT_HEADER_TO_NAME_INFO(header);
        POBJECT_HEADER_CREATOR_INFO creator = OBJECT_HEADER_TO_CREATOR_INFO(header);
        CHECK(header->Type == types[0] && header->PointerCount == 1 && !header->HandleCount &&
              (header->Flags & (OB_FLAG_KERNEL_OBJECT | OB_FLAG_PERMANENT_OBJECT)) ==
                (OB_FLAG_KERNEL_OBJECT | OB_FLAG_PERMANENT_OBJECT));
        CHECK(types[i]->Index == i + 1 && types[i] == ObpObjectTypes[i] &&
              types[i]->DefaultObject == (i < 4 ? &ObpDefaultObject : NULL) &&
              types[i]->TypeInfo.SecurityProcedure == SeDefaultObjectMethod);
        CHECK(name && creator && &creator->TypeList == creator_link &&
              name->Directory == ObpTypeDirectoryObject && name->Name.Buffer == types[i]->Name.Buffer && equal_name(&name->Name, names[i]));
        creator_link = creator_link->Flink;
    }
    CHECK(creator_link == &types[0]->TypeList && types[0]->TotalNumberOfObjects == 6);
    CHECK(types[0]->TypeInfo.MaintainTypeList && types[0]->TypeInfo.PoolType == NonPagedPool &&
          types[1]->TypeInfo.PoolType == PagedPool && types[2]->TypeInfo.PoolType == PagedPool);
    CHECK(!(types[1]->TypeInfo.ValidAccessMask & SYNCHRONIZE) &&
          !(types[2]->TypeInfo.ValidAccessMask & SYNCHRONIZE) &&
          types[2]->TypeInfo.ParseProcedure && types[2]->TypeInfo.DeleteProcedure);
    const OBJECT_TYPE_INITIALIZER *token = &SeTokenObjectType->TypeInfo;
    const GENERIC_MAPPING token_mapping = {TOKEN_READ, TOKEN_WRITE, TOKEN_EXECUTE, TOKEN_ALL_ACCESS};
    CHECK(token->PoolType == PagedPool && token->UseDefaultObject && token->SecurityRequired &&
          token->DeleteProcedure == SepTokenDeleteMethod && token->InvalidAttributes == OBJ_OPENLINK &&
          token->ValidAccessMask == (TOKEN_ALL_ACCESS | SYNCHRONIZE) &&
          RtlEqualMemory(&token->GenericMapping, &token_mapping, sizeof(token_mapping)));

    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    UNICODE_STRING name = copy_name(L"Alias");
    POBJECT_HEADER header;
    CHECK(ObpAllocateObject(NULL, KernelMode, types[2], &name, sizeof(OBJECT_SYMBOLIC_LINK),
                            &header) == STATUS_SUCCESS);
    POBJECT_SYMBOLIC_LINK link = (PVOID)&header->Body;
    CHECK(header->Type == types[2] && header->PointerCount == 1 &&
          (ULONG_PTR)link - MM_BOOT_PAGED_POOL_BASE < MM_BOOT_PAGED_POOL_PAGES * PAGE_SIZE &&
          types[2]->TotalNumberOfObjects == 1);
    RtlZeroMemory(link, sizeof(*link));
    link->LinkTarget = copy_name(L"\\Device\\Gic");

    XP_TEST_DESCRIPTOR *security = ExAllocatePoolWithTag(PagedPool, sizeof(*security), 'dSbO');
    CHECK(security);
    init_descriptor(security, 18);
    OB_SECURITY_METHOD method = types[2]->TypeInfo.SecurityProcedure;
    CHECK(method(link, AssignSecurityDescriptor, NULL, security, NULL, &header->SecurityDescriptor,
                 PagedPool, NULL) == STATUS_SUCCESS && header->SecurityDescriptor);
    CHECK(method(link, DeleteSecurityDescriptor, NULL, NULL, NULL, &header->SecurityDescriptor,
                 PagedPool, NULL) == STATUS_SUCCESS && !header->SecurityDescriptor);
    check_empty_security_cache();

    UNICODE_STRING complete = copy_name(L"\\Alias\\Timer");
    const ULONG prefix = sizeof(L"\\Alias") - sizeof(WCHAR);
    UNICODE_STRING remainder = {complete.Length - prefix, complete.MaximumLength - prefix,
                                complete.Buffer + prefix / sizeof(WCHAR)};
    PVOID target = link;
    NTSTATUS reparse = types[2]->TypeInfo.ParseProcedure(link, NULL, NULL, KernelMode, 0,
        &complete, &remainder, NULL, NULL, &target);
    CHECK(reparse == STATUS_REPARSE && !target && equal_name(&complete, L"\\Device\\Gic\\Timer"));
    ExFreePool(complete.Buffer);
    types[2]->TypeInfo.DeleteProcedure(link);
    CHECK(!link->LinkTarget.Buffer);
    ObpFreeObject(link);
    CHECK(!types[2]->TotalNumberOfObjects && types[2]->HighWaterNumberOfObjects == 1);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Object types evidence registered="); XpTestHex((sizeof(types) / sizeof(*types)));
    XpTestReport(" reparse="); XpTestHex(reparse);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP object types and real symbolic link reparse and cleanup\n");
}

static POBJECT_HEADER allocate_named_link(PCWSTR text)
{
    UNICODE_STRING name = copy_name(text);
    POBJECT_HEADER header = NULL;
    CHECK(ObpAllocateObject(NULL, KernelMode, ObpSymbolicLinkObjectType, &name,
                            sizeof(OBJECT_SYMBOLIC_LINK), &header) == STATUS_SUCCESS);
    POBJECT_SYMBOLIC_LINK link = (PVOID)&header->Body;
    RtlZeroMemory(link, sizeof(*link));
    link->LinkTarget = copy_name(L"\\ObjectTypes");
    return header;
}

VOID XpCheckBootNamespace(VOID)
{
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    POBJECT_DIRECTORY root = ObpRootDirectoryObject, types = ObpTypeDirectoryObject;
    CHECK(root && types && root != types);
    POBJECT_HEADER root_header = OBJECT_TO_OBJECT_HEADER(root);
    POBJECT_HEADER types_header = OBJECT_TO_OBJECT_HEADER(types);
    CHECK(root_header->Type == ObpDirectoryObjectType && types_header->Type == ObpDirectoryObjectType &&
          root_header->PointerCount == 2 && types_header->PointerCount == 8);
    CHECK((root_header->Flags & OB_FLAG_PERMANENT_OBJECT) &&
          (types_header->Flags & OB_FLAG_PERMANENT_OBJECT));

    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"objecttypes");
    PVOID found = ObArm64LookupBootDirectoryEntry(root, &name, OBJ_CASE_INSENSITIVE);
    CHECK(found == types);
    ObDereferenceObject(found);
    CHECK(!ObArm64LookupBootDirectoryEntry(root, &name, 0));
    CHECK(ObArm64RemoveBootDirectoryEntry(root, &name) == STATUS_ACCESS_DENIED);
    POBJECT_TYPE type_objects[] = {ObpTypeObjectType, ObpDirectoryObjectType,
                                  ObpSymbolicLinkObjectType, SeTokenObjectType, PsProcessType, PsThreadType};
    for (ULONG i = 0; i < sizeof(type_objects) / sizeof(*type_objects); ++i) {
        found = ObArm64LookupBootDirectoryEntry(types, &type_objects[i]->Name, 0);
        CHECK(found == type_objects[i] && OBJECT_TO_OBJECT_HEADER(found)->PointerCount == 2);
        ObDereferenceObject(found);
    }
    /* The actual ObCreateObjectType lookup observes the populated directory. */
    OBJECT_TYPE_INITIALIZER copied = ObpDirectoryObjectType->TypeInfo;
    POBJECT_TYPE duplicate_type = NULL;
    RtlInitUnicodeString(&name, L"directory");
    CHECK(ObCreateObjectType(&name, &copied, NULL, &duplicate_type) == STATUS_OBJECT_NAME_COLLISION &&
          !duplicate_type);

    POBJECT_HEADER first = allocate_named_link(L"\u00e4Alpha");
    POBJECT_HEADER second = allocate_named_link(L"slot69");
    CHECK(ObArm64InsertBootDirectoryEntry(root, first) == STATUS_SUCCESS);
    CHECK(ObArm64InsertBootDirectoryEntry(root, second) == STATUS_SUCCESS);
    /* These names collide under the original 37-bucket hash. */
    const ULONG bucket = 15;
    POBJECT_DIRECTORY_ENTRY chain = root->HashBuckets[bucket];
    CHECK(chain && chain->Object == &second->Body && chain->ChainLink &&
          chain->ChainLink->Object == &first->Body);
    RtlInitUnicodeString(&name, L"\u00c4ALPHA");
    CHECK(RtlUpcaseUnicodeChar(L'\u00e4') == L'\u00c4' &&
          !ObArm64LookupBootDirectoryEntry(root, &name, 0));
    PVOID pinned = ObArm64LookupBootDirectoryEntry(root, &name, OBJ_CASE_INSENSITIVE);
    CHECK(pinned == &first->Body && first->PointerCount == 2 &&
          root->HashBuckets[bucket]->Object == pinned);

    POBJECT_HEADER duplicate = allocate_named_link(L"\u00c4ALPHA");
    CHECK(ObArm64InsertBootDirectoryEntry(root, duplicate) == STATUS_OBJECT_NAME_COLLISION);
    ObDereferenceObject(&duplicate->Body);
    CHECK(ObArm64RemoveBootDirectoryEntry(root, &name) == STATUS_SUCCESS);
    CHECK(!ObArm64LookupBootDirectoryEntry(root, &name, OBJ_CASE_INSENSITIVE) &&
          first->PointerCount == 1 && !OBJECT_HEADER_TO_NAME_INFO(first)->Directory);

    PEX_WORK_QUEUE queue = &ExWorkerQueue[HyperCriticalWorkQueue];
    ULONG processed = queue->WorkItemsProcessed;
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);
    KIRQL previous;
    KeRaiseIrql(APC_LEVEL, &previous);
    CHECK(ObfDereferenceObject(pinned) == 0);
    KeLowerIrql(previous);

    RtlInitUnicodeString(&name, L"slot69");
    CHECK(ObArm64RemoveBootDirectoryEntry(root, &name) == STATUS_SUCCESS);
    CHECK(!ObArm64LookupBootDirectoryEntry(root, &name, OBJ_CASE_INSENSITIVE));

    KEVENT park;
    LARGE_INTEGER grace;
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    grace.QuadPart = -1000000;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &grace) == STATUS_TIMEOUT);
    KiArm64DisableTimerDispatch();
    CHECK(queue->WorkItemsProcessed == processed + 1 && !ObpRemoveObjectList &&
          !ObpSymbolicLinkObjectType->TotalNumberOfObjects && root_header->PointerCount == 2 &&
          types_header->PointerCount == 8);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Object namespace evidence types="); XpTestHex(ObpTypeObjectType->TotalNumberOfObjects);
    XpTestReport(" bucket="); XpTestHex(bucket);
    XpTestReport(" upcase="); XpTestHex(RtlUpcaseUnicodeChar(L'\u00e4'));
    XpTestReport(" deferred="); XpTestHex(queue->WorkItemsProcessed - processed);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: XP root and ObjectTypes namespace, NLS lookup and real deferred object deletion\n");
}

static VOID check_path(HANDLE root, PCWSTR text, POBJECT_TYPE type, NTSTATUS expected, PVOID object)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, text);
    PVOID found = (PVOID)1;
    NTSTATUS status = ObArm64LookupBootObjectName(root, &name, OBJ_CASE_INSENSITIVE, type, &found);
    if (status != expected || found != object) {
        XpTestReport("Object path mismatch ");
        for (PCWSTR cursor = text; *cursor; ++cursor) {
            CHAR character[2] = { (CHAR)*cursor, 0 };
            XpTestReport(character);
        }
        XpTestReport(" status="); XpTestHex((ULONG)status);
        XpTestReport(" expected="); XpTestHex((ULONG)expected);
        XpTestReport(" found="); XpTestHex((ULONG_PTR)found);
        XpTestReport(" expected_object="); XpTestHex((ULONG_PTR)object); XpTestReport("\n");
    }
    CHECK(status == expected && found == object);
    if (found) ObDereferenceObject(found);
}

static POBJECT_HEADER publish_link(PCWSTR text, PCWSTR target)
{
    POBJECT_HEADER header = allocate_named_link(text);
    POBJECT_SYMBOLIC_LINK link = (PVOID)&header->Body;
    ExFreePool(link->LinkTarget.Buffer);
    link->LinkTarget = copy_name(target);
    CHECK(ObArm64InsertBootDirectoryEntry(ObpRootDirectoryObject, header) == STATUS_SUCCESS);
    return header;
}

VOID XpCheckBootObjectPaths(VOID)
{
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && !ObpKernelHandleTable->HandleCount);
    ULONG root_references = OBJECT_TO_OBJECT_HEADER(ObpRootDirectoryObject)->PointerCount;
    ULONG types_references = OBJECT_TO_OBJECT_HEADER(ObpTypeDirectoryObject)->PointerCount;

    check_path(NULL, L"\\", ObpDirectoryObjectType, STATUS_SUCCESS, ObpRootDirectoryObject);
    check_path(NULL, L"\\oBJeCtTyPeS\\Directory", ObpTypeObjectType, STATUS_SUCCESS, ObpDirectoryObjectType);
    check_path(NULL, L"ObjectTypes", NULL, STATUS_OBJECT_PATH_SYNTAX_BAD, NULL);
    check_path(NULL, L"\\Absent\\Type", NULL, STATUS_OBJECT_PATH_NOT_FOUND, NULL);
    check_path(NULL, L"\\ObjectTypes\\Absent", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    check_path(NULL, L"\\ObjectTypes\\Directory\\Child", NULL, STATUS_OBJECT_TYPE_MISMATCH, NULL);
    check_path(NULL, L"\\ObjectTypes\\\\Directory", NULL, STATUS_OBJECT_NAME_INVALID, NULL);

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\ObjectTypes");
    HANDLE directory = NULL;
    CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_KERNEL_HANDLE, ObpDirectoryObjectType,
                                    SYNCHRONIZE, &directory) == STATUS_ACCESS_DENIED && !directory);
    CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_KERNEL_HANDLE, ObpDirectoryObjectType,
                                    GENERIC_READ, &directory) == STATUS_SUCCESS);
    CHECK((LONG)(ULONG_PTR)directory < 0 && ObpKernelHandleTable->HandleCount == 1 &&
          OBJECT_TO_OBJECT_HEADER(ObpTypeDirectoryObject)->HandleCount == 1);
    PVOID referenced = NULL;
    OBJECT_HANDLE_INFORMATION info;
    CHECK(ObReferenceObjectByHandle(directory, DIRECTORY_QUERY, ObpDirectoryObjectType,
          KernelMode, &referenced, &info) == STATUS_SUCCESS && referenced == ObpTypeDirectoryObject &&
          info.GrantedAccess == (STANDARD_RIGHTS_READ | DIRECTORY_QUERY | DIRECTORY_TRAVERSE) &&
          !info.HandleAttributes);
    ObDereferenceObject(referenced);
    CHECK(ObReferenceObjectByHandle(directory, 0, ObpDirectoryObjectType, UserMode,
          &referenced, NULL) == STATUS_INVALID_HANDLE && !referenced);
    CHECK(ObReferenceObjectByHandle(directory, 0, ObpTypeObjectType, KernelMode,
          &referenced, NULL) == STATUS_OBJECT_TYPE_MISMATCH && !referenced);
    CHECK(ObArm64LookupBootObjectName(NULL, &name, OBJ_FORCE_ACCESS_CHECK, ObpDirectoryObjectType,
                                    &referenced) == STATUS_NOT_SUPPORTED && !referenced);
    check_path(directory, L"Type", ObpTypeObjectType, STATUS_SUCCESS, ObpTypeObjectType);
    check_path(directory, L"", ObpDirectoryObjectType, STATUS_SUCCESS, ObpTypeDirectoryObject);
    check_path(directory, L"\\Type", ObpTypeObjectType, STATUS_OBJECT_PATH_SYNTAX_BAD, NULL);

    POBJECT_HEADER alias = publish_link(L"Alias", L"\\ObjectTypes");
    publish_link(L"Hop", L"\\Alias");
    publish_link(L"Cycle", L"\\Cycle");
    check_path(NULL, L"\\Alias\\Directory", ObpTypeObjectType, STATUS_SUCCESS, ObpDirectoryObjectType);
    check_path(NULL, L"\\Hop\\SymbolicLink", ObpTypeObjectType, STATUS_SUCCESS, ObpSymbolicLinkObjectType);
    check_path(NULL, L"\\Cycle", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    check_path(NULL, L"\\Hop\\Absent", ObpTypeObjectType, STATUS_OBJECT_NAME_NOT_FOUND, NULL);

    RtlInitUnicodeString(&name, L"\\Alias");
    HANDLE link = NULL;
    CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_KERNEL_HANDLE, ObpSymbolicLinkObjectType,
                                    SYMBOLIC_LINK_QUERY, &link) == STATUS_SUCCESS &&
          alias->HandleCount == 1 && alias->PointerCount == 2);
    RtlInitUnicodeString(&name, L"Alias");
    CHECK(ObArm64RemoveBootDirectoryEntry(ObpRootDirectoryObject, &name) == STATUS_SUCCESS &&
          alias->HandleCount == 1 && alias->PointerCount == 1);
    CHECK(ObReferenceObjectByHandle(link, SYMBOLIC_LINK_QUERY, ObpSymbolicLinkObjectType,
          KernelMode, &referenced, NULL) == STATUS_SUCCESS && referenced == &alias->Body);
    ObDereferenceObject(referenced);
    CHECK(ObArm64CloseBootObjectHandle(link) == STATUS_SUCCESS);
    CHECK(ObArm64CloseBootObjectHandle(link) == STATUS_INVALID_HANDLE);
    check_path(NULL, L"\\Alias", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    RtlInitUnicodeString(&name, L"Hop");
    CHECK(ObArm64RemoveBootDirectoryEntry(ObpRootDirectoryObject, &name) == STATUS_SUCCESS);
    RtlInitUnicodeString(&name, L"Cycle");
    CHECK(ObArm64RemoveBootDirectoryEntry(ObpRootDirectoryObject, &name) == STATUS_SUCCESS);

    CHECK(ObArm64CloseBootObjectHandle(directory) == STATUS_SUCCESS);
    check_path(directory, L"Type", ObpTypeObjectType, STATUS_INVALID_HANDLE, NULL);
    CHECK(!ObpKernelHandleTable->HandleCount && !ObpDirectoryObjectType->TotalNumberOfHandles &&
          !ObpSymbolicLinkObjectType->TotalNumberOfHandles && !ObpSymbolicLinkObjectType->TotalNumberOfObjects);
    CHECK(OBJECT_TO_OBJECT_HEADER(ObpRootDirectoryObject)->PointerCount == root_references &&
          OBJECT_TO_OBJECT_HEADER(ObpTypeDirectoryObject)->PointerCount == types_references);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Object paths evidence directory="); XpTestHex((ULONG_PTR)directory);
    XpTestReport(" link="); XpTestHex((ULONG_PTR)link);
    XpTestReport(" access="); XpTestHex(info.GrantedAccess);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP multi-component lookup, symbolic reparse and typed kernel handle lifetime\n");
}

static PSECURITY_DESCRIPTOR copy_test_descriptor(const XP_TEST_DESCRIPTOR *source)
{
    PSECURITY_DESCRIPTOR descriptor = ExAllocatePoolWithTag(PagedPool, sizeof(*source), 'dSbO');
    CHECK(descriptor);
    RtlCopyMemory(descriptor, source, sizeof(*source));
    return descriptor;
}

VOID XpCheckBootObjectInsertion(VOID)
{
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    CHECK(!ObpKernelHandleTable->HandleCount && KeGetCurrentIrql() == PASSIVE_LEVEL);
    ULONG references = OBJECT_TO_OBJECT_HEADER(ObpTypeDirectoryObject)->PointerCount;
    HANDLE parent, first, reopened, failed;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\ObjectTypes");
    CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_KERNEL_HANDLE, ObpDirectoryObjectType,
                                    DIRECTORY_TRAVERSE, &parent) == STATUS_SUCCESS);

    XP_TEST_DESCRIPTOR descriptor, duplicate;
    init_descriptor(&descriptor, 18);
    init_descriptor(&duplicate, 19);
    publish_link(L"I", L"\\ObjectTypes");
    POBJECT_HEADER original = allocate_named_link(L"\\I\\BootPublished");
    CHECK(ObArm64InsertBootObject(NULL, &original->Body, OBJ_KERNEL_HANDLE,
          copy_test_descriptor(&descriptor), GENERIC_READ, &first) == STATUS_SUCCESS);
    RtlInitUnicodeString(&name, L"I");
    CHECK(ObArm64RemoveBootDirectoryEntry(ObpRootDirectoryObject, &name) == STATUS_SUCCESS);
    POBJECT_HEADER_NAME_INFO name_info = OBJECT_HEADER_TO_NAME_INFO(original);
    CHECK(name_info->Directory == ObpTypeDirectoryObject && equal_name(&name_info->Name, L"BootPublished"));
    CHECK(original->PointerCount == 2 && original->HandleCount == 1 &&
          ObpSymbolicLinkObjectType->TotalNumberOfObjects == 1);
    EX_FAST_REF security = {.Object = original->SecurityDescriptor};
    PSECURITY_DESCRIPTOR assigned = ExFastRefGetObject(security);
    CHECK(assigned && RtlEqualMemory(assigned, &descriptor, sizeof(descriptor)));
    check_path(parent, L"BootPublished", ObpSymbolicLinkObjectType, STATUS_SUCCESS, &original->Body);

    POBJECT_HEADER candidate = allocate_named_link(L"bootpublished");
    NTSTATUS collision = ObArm64InsertBootObject(parent, &candidate->Body,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, SYMBOLIC_LINK_QUERY, &failed);
    CHECK(collision == STATUS_OBJECT_NAME_COLLISION && !failed);
    candidate = allocate_named_link(L"bootpublished");
    NTSTATUS openif = ObArm64InsertBootObject(parent, &candidate->Body,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_OPENIF, copy_test_descriptor(&duplicate),
        SYMBOLIC_LINK_QUERY, &reopened);
    CHECK(openif == STATUS_OBJECT_NAME_EXISTS && reopened && reopened != first);
    PVOID object;
    CHECK(ObReferenceObjectByHandle(reopened, SYMBOLIC_LINK_QUERY, ObpSymbolicLinkObjectType,
          KernelMode, &object, NULL) == STATUS_SUCCESS && object == &original->Body);
    ObDereferenceObject(object);
    security.Object = original->SecurityDescriptor;
    CHECK(original->PointerCount == 3 && original->HandleCount == 2 &&
          ExFastRefGetObject(security) == assigned &&
          ObpSymbolicLinkObjectType->TotalNumberOfObjects == 1);

    candidate = allocate_named_link(L"DeniedInsertion");
    NTSTATUS denied = ObArm64InsertBootObject(parent, &candidate->Body, OBJ_KERNEL_HANDLE,
        copy_test_descriptor(&duplicate), SYNCHRONIZE, &failed);
    CHECK(denied == STATUS_ACCESS_DENIED && !failed);
    check_path(parent, L"DeniedInsertion", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    ObReferenceObject(&original->Body);
    CHECK(ObArm64CloseBootObjectHandle(first) == STATUS_SUCCESS && original->HandleCount == 1);
    check_path(parent, L"BootPublished", ObpSymbolicLinkObjectType, STATUS_SUCCESS, &original->Body);
    CHECK(ObArm64CloseBootObjectHandle(reopened) == STATUS_SUCCESS);
    CHECK(!original->HandleCount && original->PointerCount == 1 && !name_info->QueryReferences &&
          !name_info->Directory && !name_info->Name.Buffer &&
          ObpSymbolicLinkObjectType->TotalNumberOfObjects == 1);
    check_path(parent, L"BootPublished", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    CHECK(equal_name(&((POBJECT_SYMBOLIC_LINK)&original->Body)->LinkTarget, L"\\ObjectTypes") &&
          RtlEqualMemory(assigned, &descriptor, sizeof(descriptor)));
    candidate = allocate_named_link(L"BootPublished");
    HANDLE replacement;
    CHECK(ObArm64InsertBootObject(parent, &candidate->Body, OBJ_KERNEL_HANDLE, NULL,
          SYMBOLIC_LINK_QUERY, &replacement) == STATUS_SUCCESS);
    check_path(parent, L"BootPublished", ObpSymbolicLinkObjectType, STATUS_SUCCESS, &candidate->Body);
    CHECK(ObArm64CloseBootObjectHandle(replacement) == STATUS_SUCCESS);
    ObDereferenceObject(&original->Body);
    check_empty_security_cache();

    /* Fill the real handle leaf and reserve the remaining paged-pool pages.
     * The live name and candidate allocations leave room in the small-pool page. */
    enum { leaf_handles = PAGE_SIZE / sizeof(HANDLE_TABLE_ENTRY) - 1 };
    static HANDLE handles[leaf_handles - 1];
    static PVOID pages[MM_BOOT_PAGED_POOL_PAGES];
    RtlInitUnicodeString(&name, L"\\ObjectTypes");
    for (ULONG i = 0; i < leaf_handles - 1; ++i) {
        CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_KERNEL_HANDLE, ObpDirectoryObjectType,
                                        DIRECTORY_TRAVERSE, &handles[i]) == STATUS_SUCCESS);
    }
    CHECK(ObpKernelHandleTable->HandleCount == leaf_handles &&
          ObpKernelHandleTable->NextHandleNeedingPool == (leaf_handles + 1) * 4);
    candidate = allocate_named_link(L"ExhaustedInsertion");
    PSECURITY_DESCRIPTOR captured = copy_test_descriptor(&descriptor);
    ULONG reserved = 0;
    for (; reserved < MM_BOOT_PAGED_POOL_PAGES; ++reserved) {
        pages[reserved] = MiAllocatePoolPages(PagedPool, PAGE_SIZE, 0);
        if (!pages[reserved]) break;
    }
    CHECK(reserved && reserved < MM_BOOT_PAGED_POOL_PAGES);
    ULONG pool_failures = ExPoolFailures;
    /* Keep a test reference to prove the original parser reached publication,
     * assigned security, then removed the name when the leaf failed to grow. */
    ObReferenceObject(&candidate->Body);
    NTSTATUS exhausted = ObArm64InsertBootObject(parent, &candidate->Body, OBJ_KERNEL_HANDLE,
        captured, SYMBOLIC_LINK_QUERY, &failed);
    CHECK(exhausted == STATUS_INSUFFICIENT_RESOURCES && !failed && ExPoolFailures == pool_failures + 1);
    CHECK(candidate->SecurityDescriptor && candidate->PointerCount == 1 && !candidate->HandleCount &&
          !OBJECT_HEADER_TO_NAME_INFO(candidate)->Directory &&
          !OBJECT_HEADER_TO_NAME_INFO(candidate)->Name.Buffer &&
          ObpKernelHandleTable->HandleCount == leaf_handles &&
          ObpKernelHandleTable->NextHandleNeedingPool == (leaf_handles + 1) * 4);
    ObDereferenceObject(&candidate->Body);
    for (ULONG i = 0; i < reserved; ++i) CHECK(MiFreePoolPages(pages[i]) == 1);
    check_path(parent, L"ExhaustedInsertion", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    for (ULONG i = 0; i < leaf_handles - 1; ++i)
        CHECK(ObArm64CloseBootObjectHandle(handles[i]) == STATUS_SUCCESS);
    CHECK(ObArm64CloseBootObjectHandle(parent) == STATUS_SUCCESS);
    CHECK(!ObpKernelHandleTable->HandleCount && !ObpDirectoryObjectType->TotalNumberOfHandles &&
          !ObpSymbolicLinkObjectType->TotalNumberOfHandles && !ObpSymbolicLinkObjectType->TotalNumberOfObjects &&
          OBJECT_TO_OBJECT_HEADER(ObpTypeDirectoryObject)->PointerCount == references);
    check_empty_security_cache();
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(after.Free == before.Free && after.Allocated == before.Allocated);
    XpTestReport("Object insertion evidence collision="); XpTestHex((ULONG)collision);
    XpTestReport(" openif="); XpTestHex((ULONG)openif);
    XpTestReport(" access_failure="); XpTestHex((ULONG)denied);
    XpTestReport(" handle_failure="); XpTestHex((ULONG)exhausted);
    XpTestReport(" handles="); XpTestHex(leaf_handles);
    XpTestReport(" pool_pages="); XpTestHex(reserved);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original named object publication, cached security and handle exhaustion rollback\n");
}

static ETHREAD namespace_workers[2];
static HANDLE closing_handle;

static VOID close_or_unlink_worker(PKSTART_ROUTINE routine, PVOID parameter)
{
    PETHREAD thread = parameter;
    NTSTATUS status;
    if (thread == &namespace_workers[0]) {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, L"ClosingRace");
        status = ObArm64RemoveBootDirectoryEntry(ObpTypeDirectoryObject, &name);
    } else {
        status = ObArm64CloseBootObjectHandle(closing_handle);
    }
    PsArm64ExitBootThread(thread, status);
}

VOID XpCheckBootObjectClose(PKPROCESS process)
{
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG stack_count = process->StackCount;
    POBJECT_DIRECTORY directory = ObpTypeDirectoryObject;
    POBJECT_HEADER parent = OBJECT_TO_OBJECT_HEADER(directory);
    ULONG directory_references = parent->PointerCount;
    POBJECT_HEADER object = allocate_named_link(L"\\ObjectTypes\\ClosingRace");
    POBJECT_HEADER_NAME_INFO name = OBJECT_HEADER_TO_NAME_INFO(object);
    ObReferenceObject(&object->Body);
    CHECK(ObArm64InsertBootObject(NULL, &object->Body, OBJ_KERNEL_HANDLE, NULL,
          SYMBOLIC_LINK_QUERY, &closing_handle) == STATUS_SUCCESS);
    CHECK(name->QueryReferences == 1 && object->PointerCount == 3 && object->HandleCount == 1);
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&directory->Lock);
    for (ULONG i = 0; i < sizeof(namespace_workers) / sizeof(*namespace_workers); ++i) {
        PETHREAD thread = &namespace_workers[i];
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        NTSTATUS status = KeInitializeThread(&thread->Tcb, NULL, close_or_unlink_worker,
                                             NULL, thread, NULL, NULL, process);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        CHECK(status == STATUS_SUCCESS);
        KeSetPriorityThread(&thread->Tcb, 19);
        KeReadyThread(&thread->Tcb);
        CHECK(directory->Lock.Waiting && thread->Tcb.State == Waiting &&
              thread->Tcb.WaitReason == Executive);
    }
    /* The first waiter will remove the entry. Close has already retired its
     * handle and holds a temporary name reference while queued behind it. */
    ULONG lookup_refs = name->QueryReferences;
    CHECK(lookup_refs == 2 && !object->HandleCount && !ObpKernelHandleTable->HandleCount);
    ExReleasePushLockExclusive(&directory->Lock);
    KeLeaveCriticalRegion();
    for (ULONG i = 0; i < sizeof(namespace_workers) / sizeof(*namespace_workers); ++i) {
        PETHREAD thread = &namespace_workers[i];
        CHECK(KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
        CHECK(thread->Tcb.State == Terminated && thread->ExitStatus == STATUS_SUCCESS);
    }
    CHECK(!name->QueryReferences && !name->Directory && !name->Name.Buffer && object->PointerCount == 1 &&
          parent->PointerCount == directory_references && !directory->Lock.Value);
    check_path(NULL, L"\\ObjectTypes\\ClosingRace", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    ObDereferenceObject(&object->Body);
    CHECK(!ObpSymbolicLinkObjectType->TotalNumberOfHandles &&
          !ObpSymbolicLinkObjectType->TotalNumberOfObjects);

    KEVENT park;
    LARGE_INTEGER grace;
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    grace.QuadPart = -1000000;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &grace) == STATUS_TIMEOUT);
    KiArm64DisableTimerDispatch();
    CHECK(process->StackCount == stack_count && !PsReaperList &&
          !namespace_workers[0].Tcb.InitialStack && !namespace_workers[1].Tcb.InitialStack);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(after.Free == before.Free && after.Allocated == before.Allocated);
    XpTestReport("Object close evidence competing_waiters="); XpTestHex(2);
    XpTestReport(" held_name_refs="); XpTestHex(lookup_refs);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original last-handle name removal, retained references and contended unlink\n");
}

typedef struct _XP_EXPECTED_ACE {
    PSID Sid;
    ACCESS_MASK Access;
} XP_EXPECTED_ACE;

static VOID check_boot_sid(PSID sid, UCHAR authority, UCHAR count, ULONG rid, ULONG second)
{
    SID_IDENTIFIER_AUTHORITY expected = {{0, 0, 0, 0, 0, authority}};
    CHECK(RtlValidSid(sid) && ((PISID)sid)->SubAuthorityCount == count &&
          RtlEqualMemory(&((PISID)sid)->IdentifierAuthority, &expected, sizeof(expected)) &&
          *RtlSubAuthoritySid(sid, 0) == rid);
    if (count == 2) CHECK(*RtlSubAuthoritySid(sid, 1) == second);
}

static VOID check_boot_dacl(PSECURITY_DESCRIPTOR descriptor, PACL expected,
                            const XP_EXPECTED_ACE *entries, ULONG count)
{
    PACL acl = NULL;
    BOOLEAN present = FALSE, defaulted = TRUE;
    CHECK(RtlGetDaclSecurityDescriptor(descriptor, &present, &acl, &defaulted) == STATUS_SUCCESS);
    CHECK(present && !defaulted && acl == expected && RtlValidAcl(acl) &&
          acl->AclRevision == ACL_REVISION2 && acl->AceCount == count &&
          acl->AclSize <= ExQueryPoolBlockSize(acl, &defaulted) && !defaulted);
    const UCHAR *end = (const UCHAR *)(acl + 1);
    for (ULONG i = 0; i < count; ++i) {
        PVOID entry;
        CHECK(RtlGetAce(acl, i, &entry) == STATUS_SUCCESS && entry == end);
        const ACCESS_ALLOWED_ACE *ace = entry;
        CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !ace->Header.AceFlags &&
              ace->Mask == entries[i].Access && RtlEqualSid((PSID)&ace->SidStart, entries[i].Sid));
        end += ace->Header.AceSize;
    }
    for (const UCHAR *cursor = end; cursor < (const UCHAR *)acl + acl->AclSize; ++cursor)
        CHECK(!*cursor);
}

VOID XpCheckBootSecurityGlobals(VOID)
{
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && SeExports &&
          SeExports->SeWorldSid == SeWorldSid && SeExports->SeLocalSystemSid == SeLocalSystemSid &&
          SeExports->SeAliasAdminsSid == SeAliasAdminsSid && SeExports->SeRestrictedSid == SeRestrictedSid);
    check_boot_sid(SeWorldSid, 1, 1, SECURITY_WORLD_RID, 0);
    check_boot_sid(SeLocalSystemSid, 5, 1, SECURITY_LOCAL_SYSTEM_RID, 0);
    check_boot_sid(SeAliasAdminsSid, 5, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);
    check_boot_sid(SeRestrictedSid, 5, 1, SECURITY_RESTRICTED_CODE_RID, 0);
    check_boot_sid(SeExports->SeLocalServiceSid, 5, 1, SECURITY_LOCAL_SERVICE_RID, 0);
    CHECK(SeExports->SeCreateTokenPrivilege.LowPart == SE_CREATE_TOKEN_PRIVILEGE &&
          SeExports->SeCreatePermanentPrivilege.LowPart == SE_CREATE_PERMANENT_PRIVILEGE &&
          SeExports->SeSecurityPrivilege.LowPart == SE_SECURITY_PRIVILEGE &&
          !SeExports->SeSecurityPrivilege.HighPart);
    PPRIVILEGE_SET set;
    SepAssemblePrivileges(2, TRUE, TRUE, &set);
    CHECK(set && set->PrivilegeCount == 2 && !set->Control);
    const LUID_AND_ATTRIBUTES *privileges =
        (PVOID)((PUCHAR)set + FIELD_OFFSET(PRIVILEGE_SET, Privilege));
    CHECK(RtlEqualLuid(&privileges[0].Luid, &SeExports->SeSecurityPrivilege) &&
          RtlEqualLuid(&privileges[1].Luid, &SeExports->SeTakeOwnershipPrivilege) &&
          privileges[0].Attributes == SE_PRIVILEGE_USED_FOR_ACCESS &&
          privileges[1].Attributes == SE_PRIVILEGE_USED_FOR_ACCESS);
    ExFreePool(set);

    XP_EXPECTED_ACE entries[] = {
        {SeWorldSid, GENERIC_EXECUTE}, {SeLocalSystemSid, GENERIC_ALL},
        {SeAliasAdminsSid, GENERIC_ALL}, {SeRestrictedSid, GENERIC_EXECUTE},
    };
    check_boot_dacl(SePublicDefaultSd, SePublicDefaultDacl, entries, 3);
    check_boot_dacl(SePublicDefaultUnrestrictedSd, SePublicDefaultUnrestrictedDacl, entries, 4);
    entries[0].Access = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE;
    entries[3].Access = GENERIC_READ | GENERIC_EXECUTE;
    check_boot_dacl(SePublicOpenSd, SePublicOpenDacl, entries, 3);
    check_boot_dacl(SePublicOpenUnrestrictedSd, SePublicOpenUnrestrictedDacl, entries, 4);
    entries[2].Access = GENERIC_READ | GENERIC_EXECUTE | READ_CONTROL;
    check_boot_dacl(SeSystemDefaultSd, SeSystemDefaultDacl, entries + 1, 2);
    XP_EXPECTED_ACE unrestricted[] = {{SeWorldSid, GENERIC_ALL}, {SeRestrictedSid, GENERIC_ALL}};
    check_boot_dacl(SeUnrestrictedSd, SeUnrestrictedDacl, unrestricted, 2);

    LUID first, second;
    ExAllocateLocallyUniqueId(&first);
    ExAllocateLocallyUniqueId(&second);
    CHECK(first.LowPart == 1003 && second.LowPart == first.LowPart + 1 &&
          !first.HighPart && !second.HighPart && ExpLuid.QuadPart == 1005);

    ULONG size = 0;
    CHECK(RtlAbsoluteToSelfRelativeSD(SePublicDefaultSd, NULL, &size) == STATUS_BUFFER_TOO_SMALL &&
          size == sizeof(SECURITY_DESCRIPTOR_RELATIVE) + SePublicDefaultDacl->AclSize);
    PSECURITY_DESCRIPTOR relative = ExAllocatePoolWithTag(PagedPool, size, 'dSbO');
    CHECK(relative && RtlAbsoluteToSelfRelativeSD(SePublicDefaultSd, relative, &size) == STATUS_SUCCESS);
    POBJECT_HEADER object = allocate_named_link(L"\\ObjectTypes\\Phase0Security");
    HANDLE handle;
    CHECK(ObArm64InsertBootObject(NULL, &object->Body, OBJ_KERNEL_HANDLE, relative,
          SYMBOLIC_LINK_QUERY, &handle) == STATUS_SUCCESS);
    EX_FAST_REF security = {.Object = object->SecurityDescriptor};
    relative = ExFastRefGetObject(security);
    CHECK(relative && ((PISECURITY_DESCRIPTOR_RELATIVE)relative)->Control == (SE_SELF_RELATIVE | SE_DACL_PRESENT) &&
          RtlLengthSecurityDescriptor(relative) == size);
    BOOLEAN present, defaulted;
    PACL dacl;
    CHECK(RtlGetDaclSecurityDescriptor(relative, &present, &dacl, &defaulted) == STATUS_SUCCESS &&
          present && !defaulted && dacl && dacl != SePublicDefaultDacl &&
          dacl->AclSize == SePublicDefaultDacl->AclSize &&
          RtlEqualMemory(dacl, SePublicDefaultDacl, dacl->AclSize));
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);
    check_path(NULL, L"\\ObjectTypes\\Phase0Security", NULL, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    check_empty_security_cache();
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Phase 0 security evidence dacl_count="); XpTestHex(6);
    XpTestReport(" ace_count="); XpTestHex(SePublicDefaultDacl->AceCount + SePublicDefaultUnrestrictedDacl->AceCount +
        SePublicOpenDacl->AceCount + SePublicOpenUnrestrictedDacl->AceCount +
        SeSystemDefaultDacl->AceCount + SeUnrestrictedDacl->AceCount);
    XpTestReport(" first_luid="); XpTestHex(first.LowPart);
    XpTestReport(" second_luid="); XpTestHex(second.LowPart);
    XpTestReport(" relative_size="); XpTestHex(size);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP Phase 0 security globals, default ACLs, LUID allocation and object security\n");
}

static VOID run_phase1_ob_control(PKSTART_ROUTINE routine)
{
    HANDLE handle;
    CHECK(PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL, NULL, routine, NULL) == STATUS_SUCCESS);
    PETHREAD thread;
    CHECK(ObReferenceObjectByHandle(handle, SYNCHRONIZE, PsThreadType, KernelMode,
                                    (PVOID *)&thread, NULL) == STATUS_SUCCESS);
    CHECK(KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS &&
          thread->ExitStatus == STATUS_SUCCESS);
    KEVENT delay;
    LARGE_INTEGER timeout = {.QuadPart = -100000};
    KeInitializeEvent(&delay, SynchronizationEvent, FALSE);
    CHECK(KeWaitForSingleObject(&delay, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT &&
          !thread->Tcb.InitialStack);
    ObDereferenceObject(thread);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);
}

static VOID object_phase1_collision(PVOID unused)
{
    UNREFERENCED_PARAMETER(unused);
    CHECK(PsGetCurrentProcess() == PsInitialSystemProcess);
    POBJECT_HEADER root = OBJECT_TO_OBJECT_HEADER(ObpRootDirectoryObject);
    ULONG root_refs = root->PointerCount;
    ULONG dirs = ObpDirectoryObjectType->TotalNumberOfObjects;
    ULONG links = ObpSymbolicLinkObjectType->TotalNumberOfObjects;
    PGENERAL_LOOKASIDE create = KeGetCurrentPrcb()->PPLookasideList[LookasideCreateInfoList].P;
    NTSTATUS status = ObArm64InitializePhase1();
    CHECK(status == STATUS_OBJECT_NAME_COLLISION &&
          !ObSystemDeviceMap && !PsGetCurrentProcess()->DeviceMap && !root->SecurityDescriptor &&
          root->PointerCount == root_refs && ObpDirectoryObjectType->TotalNumberOfObjects == dirs &&
          ObpSymbolicLinkObjectType->TotalNumberOfObjects == links &&
          KeGetCurrentPrcb()->PPLookasideList[LookasideCreateInfoList].P == create);
    check_path(NULL, L"\\KernelObjects", ObpDirectoryObjectType, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    check_path(NULL, L"\\GLOBAL??", ObpDirectoryObjectType, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    XpTestReport("Object Phase 1 rollback evidence collision="); XpTestHex((ULONG)status);
    XpTestReport("\n");
}

VOID XpCheckBootObPhase1Rollback(VOID)
{
    POBJECT_HEADER link = publish_link(L"DosDevices", L"\\ObjectTypes");
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);
    run_phase1_ob_control(object_phase1_collision);
    KiArm64DisableTimerDispatch();

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"DosDevices");
    CHECK(!(link->Flags & OB_FLAG_PERMANENT_OBJECT) &&
          ObArm64RemoveBootDirectoryEntry(ObpRootDirectoryObject, &name) == STATUS_SUCCESS);
}

static PACL phase1_dacl(PVOID object, ULONG aces)
{
    EX_FAST_REF security = {.Object = OBJECT_TO_OBJECT_HEADER(object)->SecurityDescriptor};
    PSECURITY_DESCRIPTOR descriptor = ExFastRefGetObject(security);
    BOOLEAN present, defaulted;
    PACL dacl;
    CHECK(descriptor && RtlGetDaclSecurityDescriptor(descriptor, &present, &dacl, &defaulted) == STATUS_SUCCESS &&
          present && !defaulted && dacl && RtlValidAcl(dacl) && dacl->AceCount == aces);
    return dacl;
}

static VOID verify_phase1_object_namespace(PVOID unused)
{
    UNREFERENCED_PARAMETER(unused);
    PEPROCESS system = PsGetCurrentProcess();
    PDEVICE_MAP map = system->DeviceMap;
    CHECK(system == PsInitialSystemProcess && map && map == ObSystemDeviceMap && map->ReferenceCount == 1 &&
          !map->DriveMap && !map->GlobalDosDevicesDirectory && map->DosDevicesDirectory->DeviceMap == map);
    POBJECT_DIRECTORY dos = map->DosDevicesDirectory, root = ObpRootDirectoryObject;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"KernelObjects");
    POBJECT_DIRECTORY kernel = ObArm64LookupBootDirectoryEntry(root, &name, 0);
    CHECK(kernel && OBJECT_TO_OBJECT_HEADER(kernel)->Flags & OB_FLAG_PERMANENT_OBJECT &&
          OBJECT_TO_OBJECT_HEADER(dos)->Flags & OB_FLAG_PERMANENT_OBJECT);
    check_path(NULL, L"\\??", ObpDirectoryObjectType, STATUS_SUCCESS, dos);
    check_path(NULL, L"\\GLOBAL??", ObpDirectoryObjectType, STATUS_SUCCESS, dos);
    check_path(NULL, L"\\??\\Global", ObpDirectoryObjectType, STATUS_SUCCESS, dos);
    check_path(NULL, L"\\DosDevices", ObpDirectoryObjectType, STATUS_SUCCESS, dos);
    check_path(NULL, L"\\DosDevices\\GLOBALROOT\\KernelObjects", ObpDirectoryObjectType, STATUS_SUCCESS, kernel);
    check_path(NULL, L"\\??\\GLOBALROOT\\ObjectTypes\\Process", ObpTypeObjectType, STATUS_SUCCESS, PsProcessType);
    CHECK(map->ReferenceCount == 1);

    /* Later Phase 1 services must be able to publish under secured parents
     * through the existing trusted kernel insertion entry. */
    POBJECT_HEADER transient = allocate_named_link(L"\\KernelObjects\\Phase1Transient");
    XP_TEST_DESCRIPTOR input;
    init_descriptor(&input, 18);
    HANDLE transient_handle;
    CHECK(ObArm64InsertBootObject(NULL, &transient->Body, OBJ_KERNEL_HANDLE,
                                    copy_test_descriptor(&input), SYMBOLIC_LINK_QUERY,
                                    &transient_handle) == STATUS_SUCCESS);
    check_path(NULL, L"\\KernelObjects\\Phase1Transient", ObpSymbolicLinkObjectType,
               STATUS_SUCCESS, &transient->Body);
    CHECK(ObArm64CloseBootObjectHandle(transient_handle) == STATUS_SUCCESS);
    check_path(NULL, L"\\KernelObjects\\Phase1Transient", ObpSymbolicLinkObjectType,
               STATUS_OBJECT_NAME_NOT_FOUND, NULL);

    BOOLEAN present, defaulted;
    PACL expected;
    CHECK(RtlGetDaclSecurityDescriptor(SePublicDefaultUnrestrictedSd, &present, &expected, &defaulted) == STATUS_SUCCESS &&
          present && expected);
    phase1_dacl(root, expected->AceCount);
    PACL kernel_acl = phase1_dacl(kernel, 3);
    PACL dos_acl = phase1_dacl(dos, 3);
    PVOID ace;
    CHECK(RtlGetAce(kernel_acl, 2, &ace) == STATUS_SUCCESS &&
          ((PACCESS_ALLOWED_ACE)ace)->Mask == (DIRECTORY_QUERY | DIRECTORY_TRAVERSE | READ_CONTROL) &&
          RtlEqualSid(SeWorldSid, &((PACCESS_ALLOWED_ACE)ace)->SidStart));
    CHECK(RtlGetAce(dos_acl, 2, &ace) == STATUS_SUCCESS &&
          ((PACCESS_ALLOWED_ACE)ace)->Mask == DIRECTORY_ALL_ACCESS &&
          ((PACE_HEADER)ace)->AceFlags == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE));
    ObDereferenceObject(kernel);

    PKPRCB prcb = KeGetCurrentPrcb();
    PGENERAL_LOOKASIDE create = prcb->PPLookasideList[LookasideCreateInfoList].P;
    PGENERAL_LOOKASIDE names = prcb->PPLookasideList[LookasideNameBufferList].P;
    CHECK(create != &ObpCreateInfoLookasideList && names != &ObpNameBufferLookasideList &&
          prcb->PPLookasideList[LookasideCreateInfoList].L == &ObpCreateInfoLookasideList &&
          prcb->PPLookasideList[LookasideNameBufferList].L == &ObpNameBufferLookasideList &&
          create->Type == NonPagedPool && names->Type == PagedPool);
    ULONG allocations = create->TotalAllocates;
    PVOID object;
    CHECK(ObCreateObject(KernelMode, PsThreadType, NULL, KernelMode, NULL, sizeof(ETHREAD),
                          0, 0, &object) == STATUS_SUCCESS);
    RtlZeroMemory(object, sizeof(ETHREAD));
    ObDereferenceObject(object);
    CHECK(create->TotalAllocates == allocations + 1 && QueryDepthSList(&create->ListHead));
    CHECK(ObArm64InitializePhase1() == STATUS_INVALID_DEVICE_STATE);
    XpTestReport("Object Phase 1 evidence directories="); XpTestHex(ObpDirectoryObjectType->TotalNumberOfObjects);
    XpTestReport(" links="); XpTestHex(ObpSymbolicLinkObjectType->TotalNumberOfObjects);
    XpTestReport(" map_refs="); XpTestHex(map->ReferenceCount);
    XpTestReport(" create_allocs="); XpTestHex(create->TotalAllocates - allocations);
    XpTestReport("\n");
}

VOID XpCheckBootObPhase1(VOID)
{
    check_path(NULL, L"\\??", ObpDirectoryObjectType, STATUS_OBJECT_NAME_NOT_FOUND, NULL);
    run_phase1_ob_control(verify_phase1_object_namespace);
    XpTestReport("PASS: XP Phase 1 object namespace, System DOS map and processor lookasides\n");
}
