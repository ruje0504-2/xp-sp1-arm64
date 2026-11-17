/* Start the System thread at the original Phase 1 entry prefix. */
NTSTATUS KiArm64StartPhase1(PXP_ARM64_BOOT_BLOCK block, PHANDLE thread_handle)
{
    if (!thread_handle) return STATUS_INVALID_PARAMETER;
    *thread_handle = NULL;
    if (!block || &block->Nt != KeLoaderBlock || !PsInitialSystemProcess ||
        !KeGetCurrentThread() || KeGetCurrentIrql() != PASSIVE_LEVEL || started)
        return STATUS_INVALID_DEVICE_STATE;

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    started = TRUE;
    NTSTATUS status = PsCreateSystemThread(thread_handle, THREAD_ALL_ACCESS,
        &attributes, NULL, NULL, Phase1Initialization, &block->Nt);
    if (!NT_SUCCESS(status)) started = FALSE;
    return status;
}
