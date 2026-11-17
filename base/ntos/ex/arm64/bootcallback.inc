/* Namespace adapters for the original callback package before Se Phase 1. */
static NTSTATUS allocate_callback(POBJECT_ATTRIBUTES attributes, PVOID *body)
{
    if (attributes->ObjectName)
        return ObArm64AllocateBootUnicodeObject(attributes->ObjectName, ExCallbackObjectType,
                                                sizeof(CALLBACK_OBJECT), body);
    return ObCreateObject(KernelMode, ExCallbackObjectType, attributes, KernelMode,
                          NULL, sizeof(CALLBACK_OBJECT), 0, 0, body);
}

static NTSTATUS insert_callback(PCALLBACK_OBJECT callback, POBJECT_ATTRIBUTES attributes, PHANDLE handle)
{
    if (!attributes->ObjectName)
        return ObInsertObject(callback, NULL, CALLBACK_MODIFY_STATE, 0, NULL, handle);

    PSECURITY_DESCRIPTOR security;
    NTSTATUS status = ObArm64CaptureBootTokenSecurity(ExCallbackObjectType, NULL, &security);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(callback);
        return status;
    }
    return ObArm64InsertBootObject(attributes->RootDirectory, callback,
            attributes->Attributes | OBJ_OPENIF | OBJ_KERNEL_HANDLE,
            security, CALLBACK_MODIFY_STATE, handle);
}
