/* Phase 1 named keyed events use the existing trusted object namespace. */
static NTSTATUS allocate_keyed_event(POBJECT_ATTRIBUTES attributes, PVOID *object)
{
    if (attributes && attributes->Length != sizeof(*attributes)) return STATUS_INVALID_PARAMETER;
    if (attributes && attributes->ObjectName) {
        if (attributes->SecurityQualityOfService || PsGetCurrentThread()->ImpersonationInfo ||
            attributes->Attributes & ~(OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT | OBJ_OPENIF))
            return STATUS_NOT_SUPPORTED;
        return ObArm64AllocateBootUnicodeObject(attributes->ObjectName, ExpKeyedEventObjectType,
                                                sizeof(KEYED_EVENT_OBJECT), object);
    }
    return ObCreateObject(KernelMode, ExpKeyedEventObjectType, attributes, KernelMode, NULL,
                          sizeof(KEYED_EVENT_OBJECT), 0, 0, object);
}

static NTSTATUS insert_keyed_event(POBJECT_ATTRIBUTES attributes, PVOID object, ACCESS_MASK access,
                                   PHANDLE handle)
{
    if (!attributes || !attributes->ObjectName)
        return ObInsertObject(object, NULL, access, 0, NULL, handle);

    PSECURITY_DESCRIPTOR relative;
    NTSTATUS status = ObArm64CaptureBootTokenSecurity(ExpKeyedEventObjectType,
                    attributes->SecurityDescriptor, &relative);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(object);
        return status;
    }
    return ObArm64InsertBootObject(attributes->RootDirectory, object,
              attributes->Attributes | OBJ_KERNEL_HANDLE, relative, access, handle);
}
