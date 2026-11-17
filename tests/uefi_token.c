#include "xp-rm.h"
#include "xp-token.h"
#include <seopaque.h>
#include <ntobapi.h>
#include "../base/ntos/mm/arm64/bootframes.h"
#include "../base/ntos/ob/arm64/bootobject.h"
#include "../base/ntos/ps/arm64/bootprocess.h"
#include "uefi_test.h"

extern LIST_ENTRY SepLsaQueue;
extern ERESOURCE SepLsaQueueLock;

VOID XpCheckBootSystemToken(VOID)
{
    PEPROCESS process = PsGetCurrentProcess();
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    PVOID rejected = (PVOID)1;
    CHECK(ObCreateObject(KernelMode, NULL, NULL, KernelMode, NULL, 0, 0, 0, &rejected) ==
          STATUS_INVALID_PARAMETER && !rejected);
    rejected = (PVOID)1;
    CHECK(ObCreateObject(UserMode, SeTokenObjectType, NULL, UserMode, NULL, sizeof(TOKEN),
          0, 0, &rejected) == STATUS_NOT_SUPPORTED && !rejected);
    PTOKEN token = ExFastRefGetObject(process->Token);
    ULONG bias = ExFastRefGetAdditionalReferenceCount();
    CHECK(token && ExFastRefGetUnusedReferences(process->Token) == bias);
    CHECK(IsListEmpty(&SepLsaQueue) && !SepLsaQueueLock.ActiveCount &&
          !PsGetCurrentThread()->ImpersonationInfo &&
          !(PsGetCurrentThread()->CrossThreadFlags & PS_CROSS_THREAD_FLAGS_IMPERSONATING));
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(token);
    CHECK(header->Type == SeTokenObjectType && SeTokenObjectType->TotalNumberOfObjects == 1 &&
          header->PointerCount == (LONG)(1 + bias) && !header->HandleCount &&
          (header->Flags & OB_FLAG_NEW_OBJECT) && !(header->Flags & OB_FLAG_KERNEL_OBJECT) &&
          !OBJECT_HEADER_TO_NAME_INFO(header) && !header->SecurityDescriptor);

    CHECK(token->TokenType == TokenPrimary && !token->SessionId && !token->TokenInUse &&
          RtlEqualLuid(&token->AuthenticationId, &SeSystemAuthenticationId) &&
          token->ModifiedId.LowPart == 1001 && token->TokenId.LowPart == 1002 &&
          !token->ModifiedId.HighPart && !token->TokenId.HighPart &&
          RtlEqualMemory(&token->TokenSource, &SeSystemTokenSource, sizeof(TOKEN_SOURCE)));
    CHECK(token->TokenLock && !token->TokenLock->ActiveCount && token->DefaultOwnerIndex == 1 &&
          token->UserAndGroupCount == 4 && token->PrivilegeCount == 22 &&
          !token->RestrictedSids && !token->RestrictedSidCount &&
          (token->TokenFlags & (TOKEN_HAS_ADMIN_GROUP | TOKEN_HAS_TRAVERSE_PRIVILEGE)) ==
            (TOKEN_HAS_ADMIN_GROUP | TOKEN_HAS_TRAVERSE_PRIVILEGE));
    PSID expected[] = {SeLocalSystemSid, SeAliasAdminsSid, SeWorldSid, SeAuthenticatedUsersSid};
    CHECK((PVOID)token->Privileges == &token->VariablePart &&
          (ULONG_PTR)token->UserAndGroups % sizeof(PVOID) == 0);
    for (ULONG i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        PSID copied = token->UserAndGroups[i].Sid;
        CHECK(copied != expected[i] && RtlValidSid(copied) && RtlEqualSid(copied, expected[i]));
    }
    CHECK(token->UserAndGroups[1].Attributes ==
            (SE_GROUP_OWNER | SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT) &&
          token->PrimaryGroup != SeLocalSystemSid && RtlEqualSid(token->PrimaryGroup, SeLocalSystemSid));
    CHECK(token->DefaultDacl && token->DefaultDacl != SeSystemDefaultDacl &&
          RtlValidAcl(token->DefaultDacl) && token->DefaultDacl->AclSize == SeSystemDefaultDacl->AclSize &&
          RtlEqualMemory(token->DefaultDacl, SeSystemDefaultDacl, token->DefaultDacl->AclSize));
    BOOLEAN traverse = FALSE;
    for (ULONG i = 0; i < token->PrivilegeCount; ++i) {
        PLUID_AND_ATTRIBUTES entry = &token->Privileges[i];
        if (RtlEqualLuid(&entry->Luid, &SeExports->SeChangeNotifyPrivilege))
            traverse = (entry->Attributes & SE_PRIVILEGE_ENABLED) != 0;
    }
    CHECK(traverse);

    /* XP retains the captured security descriptor until ObInsertObject in Phase 1. */
    POBJECT_CREATE_INFORMATION info = header->ObjectCreateInfo;
    CHECK(info && !info->Attributes && !info->RootDirectory && !info->SecurityQos &&
          info->ProbeMode == KernelMode && info->PagedPoolCharge > token->DynamicCharged);
    PSECURITY_DESCRIPTOR descriptor = info->SecurityDescriptor;
    CHECK(descriptor && (((PISECURITY_DESCRIPTOR_RELATIVE)descriptor)->Control & SE_SELF_RELATIVE));
    PACL dacl = NULL;
    BOOLEAN present, defaulted;
    PSID owner, group;
    CHECK(RtlGetOwnerSecurityDescriptor(descriptor, &owner, &defaulted) == STATUS_SUCCESS &&
          !defaulted && RtlEqualSid(owner, SeAliasAdminsSid));
    CHECK(RtlGetGroupSecurityDescriptor(descriptor, &group, &defaulted) == STATUS_SUCCESS &&
          !defaulted && RtlEqualSid(group, SeAliasAdminsSid));
    CHECK(RtlGetDaclSecurityDescriptor(descriptor, &present, &dacl, &defaulted) == STATUS_SUCCESS &&
          present && !defaulted && dacl && RtlValidAcl(dacl) && dacl->AceCount == 1);
    PVOID entry;
    CHECK(RtlGetAce(dacl, 0, &entry) == STATUS_SUCCESS);
    const ACCESS_ALLOWED_ACE *ace = entry;
    CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == TOKEN_ALL_ACCESS &&
          RtlEqualSid((PSID)&ace->SidStart, SeLocalSystemSid));

    PSEP_LOGON_SESSION_REFERENCES session = SepLogonSessions[SepLogonSessionIndex(&token->AuthenticationId)];
    CHECK(session && RtlEqualLuid(&session->LogonId, &token->AuthenticationId) &&
          session->ReferenceCount == 1);
    /* Consume the cached bias with live borrows to exercise the original refill. */
    for (ULONG i = 0; i < bias; ++i) CHECK(ObFastReferenceObject(&process->Token) == token);
    CHECK(ExFastRefGetUnusedReferences(process->Token) == bias &&
          header->PointerCount == (LONG)(1 + 2 * bias));
    for (ULONG i = 0; i < bias; ++i) ObFastDereferenceObject(&process->Token, token);
    CHECK(ExFastRefGetObject(process->Token) == token &&
          ExFastRefGetUnusedReferences(process->Token) == bias &&
          header->PointerCount == (LONG)(1 + bias) && session->ReferenceCount == 1);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);

    XpTestReport("System token evidence auth="); XpTestHex(token->AuthenticationId.LowPart);
    XpTestReport(" token="); XpTestHex(token->TokenId.LowPart);
    XpTestReport(" modified="); XpTestHex(token->ModifiedId.LowPart);
    XpTestReport(" sids="); XpTestHex(token->UserAndGroupCount);
    XpTestReport(" privileges="); XpTestHex(token->PrivilegeCount);
    XpTestReport(" session_refs="); XpTestHex(session->ReferenceCount);
    XpTestReport(" fast_refs="); XpTestHex(bias);
    XpTestReport(" descriptor_bytes="); XpTestHex(RtlLengthSecurityDescriptor(descriptor));
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original Phase 0 system token, captured descriptor and process fast references\n");
}

extern ACCESS_MASK SepProcessAccessesToAudit;
extern PSECURITY_DESCRIPTOR SepProcessAuditSd, SepImportantProcessSd;
extern PSID SeLocalServiceSid, SeNetworkServiceSid;
extern POBJECT_TYPE ExEventObjectType;
NTSTATUS SepInitProcessAuditSd(VOID);

static PACL object_dacl(PSECURITY_DESCRIPTOR descriptor, ULONG entries)
{
    PACL dacl;
    BOOLEAN present, defaulted;
    CHECK(descriptor && RtlGetDaclSecurityDescriptor(descriptor, &present, &dacl, &defaulted) ==
          STATUS_SUCCESS && present && !defaulted && dacl && RtlValidAcl(dacl) && dacl->AceCount == entries);
    return dacl;
}

static VOID check_allow_ace(PACL acl, ULONG index, PSID sid, ACCESS_MASK mask)
{
    PACCESS_ALLOWED_ACE ace;
    CHECK(RtlGetAce(acl, index, (PVOID *)&ace) == STATUS_SUCCESS &&
          ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !ace->Header.AceFlags &&
          ace->Mask == mask && RtlEqualSid(&ace->SidStart, sid));
}

static VOID check_anonymous_token(PTOKEN token, BOOLEAN everyone)
{
    CHECK(token && token->UserAndGroupCount == (everyone ? 2 : 1) &&
          token->TokenType == TokenPrimary && !token->PrivilegeCount && !token->TokenInUse &&
          !token->RestrictedSidCount && !token->DefaultOwnerIndex && token->ExpirationTime.QuadPart > 0 &&
          RtlEqualLuid(&token->AuthenticationId, &SeAnonymousAuthenticationId) &&
          RtlEqualSid(token->UserAndGroups[0].Sid, SeAnonymousLogonSid) &&
          !token->UserAndGroups[0].Attributes && RtlEqualSid(token->PrimaryGroup, SeAnonymousLogonSid));
    if (everyone)
        CHECK(RtlEqualSid(token->UserAndGroups[1].Sid, SeWorldSid) &&
              token->UserAndGroups[1].Attributes ==
                  (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED));
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(token);
    CHECK(header->Type == SeTokenObjectType && !header->HandleCount && header->PointerCount == 1 &&
          (header->Flags & OB_FLAG_NEW_OBJECT) && header->ObjectCreateInfo &&
          !header->SecurityDescriptor);
    PSECURITY_DESCRIPTOR descriptor = header->ObjectCreateInfo->SecurityDescriptor;
    PSID owner, group;
    BOOLEAN defaulted;
    CHECK(RtlGetOwnerSecurityDescriptor(descriptor, &owner, &defaulted) == STATUS_SUCCESS &&
          !defaulted && RtlEqualSid(owner, SeWorldSid) &&
          RtlGetGroupSecurityDescriptor(descriptor, &group, &defaulted) == STATUS_SUCCESS &&
          !defaulted && RtlEqualSid(group, SeWorldSid));
    PACL dacl = object_dacl(descriptor, 2);
    check_allow_ace(dacl, 0, SeWorldSid, TOKEN_ALL_ACCESS);
    check_allow_ace(dacl, 1, SeAnonymousLogonSid, TOKEN_ALL_ACCESS);
    CHECK(dacl != token->DefaultDacl && dacl->AclSize == token->DefaultDacl->AclSize &&
          RtlEqualMemory(dacl, token->DefaultDacl, dacl->AclSize));
}

static VOID check_process_audit_sd(VOID)
{
    CHECK(!SepProcessAccessesToAudit && !SepProcessAuditSd && !SepImportantProcessSd);
    SepProcessAccessesToAudit = PROCESS_CREATE_THREAD | PROCESS_SET_INFORMATION;
    CHECK(SepInitProcessAuditSd() == STATUS_SUCCESS && SepProcessAuditSd && SepImportantProcessSd);
    PACL sacl;
    BOOLEAN present, defaulted;
    CHECK(RtlGetSaclSecurityDescriptor(SepProcessAuditSd, &present, &sacl, &defaulted) == STATUS_SUCCESS &&
          present && !defaulted && sacl && RtlValidAcl(sacl) && sacl->AceCount == 1);
    PSYSTEM_AUDIT_ACE ace;
    CHECK(RtlGetAce(sacl, 0, (PVOID *)&ace) == STATUS_SUCCESS &&
          ace->Header.AceType == SYSTEM_AUDIT_ACE_TYPE &&
          ace->Header.AceFlags == (SUCCESSFUL_ACCESS_ACE_FLAG | FAILED_ACCESS_ACE_FLAG) &&
          ace->Mask == SepProcessAccessesToAudit && RtlEqualSid(&ace->SidStart, SeWorldSid));
    PACL dacl = object_dacl(SepImportantProcessSd, 3);
    check_allow_ace(dacl, 0, SeLocalSystemSid, 1);
    check_allow_ace(dacl, 1, SeLocalServiceSid, 1);
    check_allow_ace(dacl, 2, SeNetworkServiceSid, 1);
    SepProcessAccessesToAudit = 0;
    CHECK(SepInitProcessAuditSd() == STATUS_SUCCESS && !SepProcessAuditSd);
    ExFreePool(SepImportantProcessSd);
    SepImportantProcessSd = NULL;
}

static VOID check_security_publication_rollback(VOID)
{
    UNICODE_STRING name, child;
    OBJECT_ATTRIBUTES attributes;
    RtlInitUnicodeString(&name, L"\\XpSeRollback");
    RtlInitUnicodeString(&child, L"Unpublished");
    InitializeObjectAttributes(&attributes, &name, OBJ_PERMANENT, NULL, SePublicDefaultSd);
    ULONG directories = ObpDirectoryObjectType->TotalNumberOfObjects;
    ULONG events = ExEventObjectType->TotalNumberOfObjects;
    HANDLE parent, handle;
    CHECK(NtCreateDirectoryObject(&parent, DIRECTORY_ALL_ACCESS, &attributes) == STATUS_SUCCESS);
    CHECK(NtCreateDirectoryObject(&handle, DIRECTORY_ALL_ACCESS, &attributes) ==
          STATUS_OBJECT_NAME_COLLISION && !handle);
    InitializeObjectAttributes(&attributes, &child, OBJ_PERMANENT, parent, SePublicDefaultSd);
    CHECK(NtCreateEvent(&handle, 0x04000000, &attributes, NotificationEvent, FALSE) ==
          STATUS_ACCESS_DENIED && !handle && ExEventObjectType->TotalNumberOfObjects == events);
    PVOID object;
    CHECK(ObArm64LookupBootObjectName(parent, &child, 0, ExEventObjectType, &object) ==
          STATUS_OBJECT_NAME_NOT_FOUND && !object);
    CHECK(ObArm64MakeBootObjectTemporary(parent) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(parent) == STATUS_SUCCESS);
    CHECK(ObArm64LookupBootObjectName(NULL, &name, 0, ObpDirectoryObjectType, &object) ==
          STATUS_OBJECT_NAME_NOT_FOUND && !object && ObpDirectoryObjectType->TotalNumberOfObjects == directories);
}

VOID XpCheckBootSecurityPhase1(VOID)
{
    PTOKEN system = ExFastRefGetObject(PsInitialSystemProcess->Token);
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(system);
    EX_FAST_REF security = {.Object = header->SecurityDescriptor};
    CHECK(system == PspBootAccessToken && system == ExFastRefGetObject(PsIdleProcess->Token) &&
          system->TokenInUse && !(header->Flags & OB_FLAG_NEW_OBJECT) && !header->HandleCount &&
          SeTokenObjectType->TotalNumberOfObjects == 3 && !SeTokenObjectType->TotalNumberOfHandles);
    check_allow_ace(object_dacl(ExFastRefGetObject(security), 1), 0, SeLocalSystemSid, TOKEN_ALL_ACCESS);

    PTOKEN everyone = SeAnonymousLogonToken;
    PTOKEN alone = SeAnonymousLogonTokenNoEveryone;
    check_anonymous_token(everyone, TRUE);
    check_anonymous_token(alone, FALSE);
    CHECK(everyone != alone && !RtlEqualLuid(&everyone->TokenId, &alone->TokenId));
    PSEP_LOGON_SESSION_REFERENCES session = SepLogonSessions[SepLogonSessionIndex(&everyone->AuthenticationId)];
    while (session && !RtlEqualLuid(&session->LogonId, &everyone->AuthenticationId)) session = session->Next;
    CHECK(session && session->ReferenceCount == 2);

    UNICODE_STRING directory_name, event_name;
    RtlInitUnicodeString(&directory_name, L"\\security");
    RtlInitUnicodeString(&event_name, L"lsa_authentication_initialized");
    POBJECT_DIRECTORY directory;
    CHECK(ObArm64LookupBootObjectName(NULL, &directory_name, OBJ_CASE_INSENSITIVE,
          ObpDirectoryObjectType, (PVOID *)&directory) == STATUS_SUCCESS);
    header = OBJECT_TO_OBJECT_HEADER(directory);
    security.Object = header->SecurityDescriptor;
    CHECK(!header->HandleCount && (header->Flags & OB_FLAG_PERMANENT_OBJECT));
    PACL dacl = object_dacl(ExFastRefGetObject(security), 3);
    check_allow_ace(dacl, 0, SeLocalSystemSid, DIRECTORY_ALL_ACCESS);
    check_allow_ace(dacl, 1, SeAliasAdminsSid, DIRECTORY_QUERY | DIRECTORY_TRAVERSE | READ_CONTROL);
    check_allow_ace(dacl, 2, SeWorldSid, DIRECTORY_TRAVERSE);

    HANDLE parent, event_handle;
    CHECK(ObArm64OpenBootObjectByName(NULL, &directory_name, OBJ_CASE_INSENSITIVE, ObpDirectoryObjectType,
          DIRECTORY_ALL_ACCESS, &parent) == STATUS_SUCCESS);
    CHECK(ObArm64OpenBootObjectByName(parent, &event_name, OBJ_CASE_INSENSITIVE, ExEventObjectType,
          EVENT_ALL_ACCESS, &event_handle) == STATUS_SUCCESS);
    PKEVENT event;
    CHECK(ObReferenceObjectByHandle(event_handle, EVENT_MODIFY_STATE, ExEventObjectType, KernelMode,
          (PVOID *)&event, NULL) == STATUS_SUCCESS);
    header = OBJECT_TO_OBJECT_HEADER(event);
    security.Object = header->SecurityDescriptor;
    CHECK(header->HandleCount == 1 && (header->Flags & OB_FLAG_PERMANENT_OBJECT) &&
          event->Header.Type == EventNotificationObject && !KeReadStateEvent(event));
    BOOLEAN present, defaulted;
    PACL public_dacl;
    CHECK(RtlGetDaclSecurityDescriptor(SePublicDefaultSd, &present, &public_dacl, &defaulted) ==
          STATUS_SUCCESS && present && public_dacl);
    dacl = object_dacl(ExFastRefGetObject(security), public_dacl->AceCount);
    for (ULONG i = 0; i < public_dacl->AceCount; ++i) {
        PACCESS_ALLOWED_ACE original;
        CHECK(RtlGetAce(public_dacl, i, (PVOID *)&original) == STATUS_SUCCESS);
        ACCESS_MASK mask = original->Mask;
        RtlMapGenericMask(&mask, &ExEventObjectType->TypeInfo.GenericMapping);
        check_allow_ace(dacl, i, &original->SidStart, mask);
    }

    LARGE_INTEGER immediate = {0};
    CHECK(KeWaitForSingleObject(event, Executive, KernelMode, FALSE, &immediate) == STATUS_TIMEOUT &&
          KeSetEvent(event, 0, FALSE) == 0 &&
          KeWaitForSingleObject(event, Executive, KernelMode, FALSE, &immediate) == STATUS_SUCCESS &&
          KeReadStateEvent(event) && KeResetEvent(event) == 1);
    CHECK(ObArm64CloseBootObjectHandle(event_handle) == STATUS_SUCCESS && !header->HandleCount &&
          ObArm64CloseBootObjectHandle(parent) == STATUS_SUCCESS);
    UNICODE_STRING event_path;
    RtlInitUnicodeString(&event_path, L"\\Security\\LSA_AUTHENTICATION_INITIALIZED");
    PVOID reopened;
    CHECK(ObArm64LookupBootObjectName(NULL, &event_path, OBJ_CASE_INSENSITIVE, ExEventObjectType,
          &reopened) == STATUS_SUCCESS && reopened == event);
    ObDereferenceObject(reopened);
    ObDereferenceObject(event);
    ObDereferenceObject(directory);
    check_process_audit_sd();
    check_security_publication_rollback();
    XpTestReport("Security Phase 1 evidence anonymous_each="); XpTestHex(everyone->UserAndGroupCount);
    XpTestReport(" anonymous_only="); XpTestHex(alone->UserAndGroupCount);
    XpTestReport(" session_refs="); XpTestHex(session->ReferenceCount);
    XpTestReport(" directory_aces="); XpTestHex(3);
    XpTestReport(" event_aces="); XpTestHex(public_dacl->AceCount); XpTestReport("\n");
    XpTestReport("PASS: original XP Se Phase 1 tokens, Security directory, LSA event and process audit descriptors\n");
}
