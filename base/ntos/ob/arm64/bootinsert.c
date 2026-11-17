/* Unnamed kernel insertion during startup, following obinsert.c ownership. */
#include "obp.h"
#include "bootobject.h"
#include "xp-token.h"
#include <seopaque.h>

NTSTATUS ObArm64CaptureBootTokenSecurity(POBJECT_TYPE type, PSECURITY_DESCRIPTOR explicit_descriptor,
                                        PSECURITY_DESCRIPTOR *result)
{
    *result = NULL;
    PEPROCESS process = PsGetCurrentProcess();
    PTOKEN token = ObFastReferenceObject(&process->Token);
    if (!token) return STATUS_NO_TOKEN;
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(token->TokenLock, TRUE);

    SECURITY_DESCRIPTOR absolute = {0};
    absolute.Revision = SECURITY_DESCRIPTOR_REVISION;
    absolute.Control = SE_DACL_PRESENT | SE_DACL_DEFAULTED;
    absolute.Owner = token->UserAndGroups[token->DefaultOwnerIndex].Sid;
    absolute.Group = token->PrimaryGroup;
    absolute.Dacl = token->DefaultDacl;
    NTSTATUS status = STATUS_SUCCESS;
    if (explicit_descriptor) {
        const SECURITY_DESCRIPTOR *captured = explicit_descriptor;
        if (captured->Control & SE_SACL_PRESENT) {
            status = STATUS_NOT_SUPPORTED;
            goto done;
        }
        PSID owner, group;
        PACL dacl;
        BOOLEAN present, defaulted;
        status = RtlGetOwnerSecurityDescriptor(explicit_descriptor, &owner, &defaulted);
        if (!NT_SUCCESS(status)) goto done;
        if (owner) absolute.Owner = owner;
        status = RtlGetGroupSecurityDescriptor(explicit_descriptor, &group, &defaulted);
        if (!NT_SUCCESS(status)) goto done;
        if (group) absolute.Group = group;
        status = RtlGetDaclSecurityDescriptor(explicit_descriptor, &present, &dacl, &defaulted);
        if (!NT_SUCCESS(status)) goto done;
        if (present) {
            absolute.Dacl = dacl;
            if (!defaulted) absolute.Control &= ~SE_DACL_DEFAULTED;
            absolute.Control |= captured->Control & SE_DACL_PROTECTED;
        }
    }

    ULONG length = 0;
    status = RtlAbsoluteToSelfRelativeSD(&absolute, NULL, &length);
    if (status != STATUS_BUFFER_TOO_SMALL) goto done;
    PSECURITY_DESCRIPTOR relative = ExAllocatePoolWithTag(PagedPool, length, 'mSbO');
    if (!relative) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    status = RtlAbsoluteToSelfRelativeSD(&absolute, relative, &length);
    if (NT_SUCCESS(status)) {
        BOOLEAN present, defaulted;
        PACL dacl;
        status = RtlGetDaclSecurityDescriptor(relative, &present, &dacl, &defaulted);
        if (NT_SUCCESS(status) && dacl) {
            if (!RtlValidAcl(dacl)) status = STATUS_INVALID_ACL;
            for (ULONG i = 0; NT_SUCCESS(status) && i < dacl->AceCount; ++i) {
                PVOID entry;
                status = RtlGetAce(dacl, i, &entry);
                if (!NT_SUCCESS(status)) break;
                PACCESS_ALLOWED_ACE ace = entry;
                if ((ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE &&
                     ace->Header.AceType != ACCESS_DENIED_ACE_TYPE) || ace->Header.AceFlags) {
                    status = STATUS_NOT_SUPPORTED;
                    break;
                }
                RtlMapGenericMask(&ace->Mask, &type->TypeInfo.GenericMapping);
            }
        }
    }
    if (NT_SUCCESS(status)) *result = relative;
    else ExFreePool(relative);

done:
    ExReleaseResourceLite(token->TokenLock);
    KeLeaveCriticalRegion();
    ObFastDereferenceObject(&process->Token, token);
    return status;
}

static NTSTATUS assign_unnamed_security(PVOID object, POBJECT_HEADER header,
                                        PSECURITY_DESCRIPTOR explicit_descriptor)
{
    POBJECT_TYPE type = header->Type;
    PSECURITY_DESCRIPTOR relative;
    NTSTATUS status = ObArm64CaptureBootTokenSecurity(type, explicit_descriptor, &relative);
    if (NT_SUCCESS(status)) {
        status = type->TypeInfo.SecurityProcedure(object, AssignSecurityDescriptor,
                    NULL, relative, NULL, &header->SecurityDescriptor,
                    type->TypeInfo.PoolType, &type->TypeInfo.GenericMapping);
        if (!NT_SUCCESS(status)) ExFreePool(relative);
    }
    return status;
}

#undef ObInsertObject
NTSTATUS ObInsertObject(PVOID object, PACCESS_STATE access_state, ACCESS_MASK access,
                        ULONG pointer_bias, PVOID *new_object, PHANDLE handle)
{
    if (handle) *handle = NULL;
    if (new_object) *new_object = NULL;
    if (!object || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;

    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(object);
    POBJECT_TYPE type = header->Type;
    if (!(header->Flags & OB_FLAG_NEW_OBJECT) || !header->ObjectCreateInfo || header->SecurityDescriptor)
        return STATUS_INVALID_PARAMETER;
    POBJECT_CREATE_INFORMATION info = header->ObjectCreateInfo;
    ULONG attributes = info->Attributes;
    POBJECT_HEADER_QUOTA_INFO quota = OBJECT_HEADER_TO_QUOTA_INFO(header);
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    if (access_state || KeGetPreviousMode() != KernelMode || OBJECT_HEADER_TO_NAME_INFO(header) ||
        info->RootDirectory || (info->Attributes & ~OBJ_KERNEL_HANDLE) || info->SecurityQos ||
        PsGetCurrentThread()->ImpersonationInfo || !ObpKernelHandleTable ||
        !PsGetCurrentProcess()->ObjectTable ||
        type->TypeInfo.SecurityProcedure != SeDefaultObjectMethod)
        goto failed;

    status = STATUS_INVALID_PARAMETER;
    if ((!handle && (access || pointer_bias || new_object || !type->TypeInfo.SecurityRequired)) ||
        pointer_bias >= (ULONG)(MAXLONG - header->PointerCount) ||
        (quota && quota->PagedPoolCharge > MAXULONG - quota->SecurityDescriptorCharge))
        goto failed;

    if (type->TypeInfo.SecurityRequired) {
        status = assign_unnamed_security(object, header, info->SecurityDescriptor);
        if (!NT_SUCCESS(status)) goto failed;
    }

    /* The header union now belongs to the quota block. The local creation
     * record must be released even if charging fails after the transition. */
    header->ObjectCreateInfo = NULL;
    BOOLEAN is_new;
    ObpLockObject(header);
    status = ObpChargeQuotaForObject(header, type, &is_new);
    ObpUnlockObject(header);
    ObpFreeObjectCreateInformation(info);
    if (NT_SUCCESS(status) && handle) {
        if (access & MAXIMUM_ALLOWED)
            access = (access & ~MAXIMUM_ALLOWED) | GENERIC_ALL;
        status = ObArm64CreateBootHandle(object, access, (attributes & OBJ_KERNEL_HANDLE) != 0, handle);
        if (NT_SUCCESS(status)) {
            if (pointer_bias) ObReferenceObjectEx(object, pointer_bias);
            if (new_object) *new_object = object;
        }
    }
    if (handle || !NT_SUCCESS(status)) ObDereferenceObject(object);
    return status;

failed:
    ObDereferenceObject(object);
    return status;
}
