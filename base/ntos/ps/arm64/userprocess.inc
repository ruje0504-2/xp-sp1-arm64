/* Process and private MM owner have the same object-manager lifetime. */
typedef struct {
    EPROCESS Process;
    MM_ARM64_USER_SPACE Space;
    ULONG_PTR UserDispatchers[3];
    BOOLEAN Win32Services;
} XP_ARM64_USER_PROCESS;

MM_ARM64_USER_SPACE *PsArm64UserProcessSpace(PEPROCESS process)
{
    if (!process || !(process->Flags & PS_PROCESS_FLAGS_HAS_ADDRESS_SPACE) ||
        !PsIdleProcess || process->Pcb.DirectoryTableBase[0] == PsIdleProcess->Pcb.DirectoryTableBase[0])
        return NULL;
    MM_ARM64_USER_SPACE *space = &((XP_ARM64_USER_PROCESS *)process)->Space;
    if (!space->Root || space->Root != process->Pcb.DirectoryTableBase[0])
        KeBugCheckEx(MEMORY_MANAGEMENT, 0xa642, (ULONG_PTR)process, space->Root, 0);
    return space;
}

enum { ExceptionDispatcher, ApcDispatcher, CallbackDispatcher };

/* The loader installs mapped user-mode dispatchers before thread startup. */
static NTSTATUS set_user_dispatcher(PEPROCESS process, ULONG_PTR address, ULONG which)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(process);
    if (!space) return STATUS_INVALID_PARAMETER;
    PETHREAD current = PsGetCurrentThread();
    PspLockProcessExclusive(process, current);
    NTSTATUS status = STATUS_INVALID_DEVICE_STATE;
    if (!process->ActiveThreads && !(process->Flags & PS_PROCESS_FLAGS_PROCESS_DELETE)) {
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        if (MmArm64UserCodeAddress(space, address)) {
            XP_ARM64_USER_PROCESS *user = (XP_ARM64_USER_PROCESS *)process;
            user->UserDispatchers[which] = address;
            status = STATUS_SUCCESS;
        } else status = STATUS_ACCESS_VIOLATION;
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    }
    PspUnlockProcessExclusive(process, current);
    return status;
}

NTSTATUS PsArm64SetUserExceptionDispatcher(PEPROCESS process, ULONG_PTR address)
{
    return set_user_dispatcher(process, address, ExceptionDispatcher);
}

NTSTATUS PsArm64SetUserApcDispatcher(PEPROCESS process, ULONG_PTR address)
{
    return set_user_dispatcher(process, address, ApcDispatcher);
}

ULONG_PTR PsArm64GetUserApcDispatcher(PEPROCESS process)
{
    return PsArm64UserProcessSpace(process) ? ((XP_ARM64_USER_PROCESS *)process)->UserDispatchers[ApcDispatcher] : 0;
}

ULONG_PTR PsArm64GetUserExceptionDispatcher(PEPROCESS process)
{
    return PsArm64UserProcessSpace(process) ? ((XP_ARM64_USER_PROCESS *)process)->UserDispatchers[ExceptionDispatcher] : 0;
}

NTSTATUS PsArm64SetUserCallbackDispatcher(PEPROCESS process, ULONG_PTR address)
{
    return set_user_dispatcher(process, address, CallbackDispatcher);
}

ULONG_PTR PsArm64GetUserCallbackDispatcher(PEPROCESS process)
{
    return PsArm64UserProcessSpace(process) ? ((XP_ARM64_USER_PROCESS *)process)->UserDispatchers[CallbackDispatcher] : 0;
}

NTSTATUS PsArm64EnableWin32Services(PEPROCESS process)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !KeServiceDescriptorTableShadow[WIN32K_SERVICE_INDEX].Base)
        return STATUS_INVALID_DEVICE_STATE;
    if (!PsArm64UserProcessSpace(process)) return STATUS_INVALID_PARAMETER;
    PETHREAD current = PsGetCurrentThread();
    PspLockProcessExclusive(process, current);
    NTSTATUS status = STATUS_INVALID_DEVICE_STATE;
    if (!process->ActiveThreads && !(process->Flags & PS_PROCESS_FLAGS_PROCESS_DELETE)) {
        ((XP_ARM64_USER_PROCESS *)process)->Win32Services = TRUE;
        status = STATUS_SUCCESS;
    }
    PspUnlockProcessExclusive(process, current);
    return status;
}

BOOLEAN PsArm64Win32ServicesEnabled(PEPROCESS process)
{
    return PsArm64UserProcessSpace(process) && ((XP_ARM64_USER_PROCESS *)process)->Win32Services;
}

static NTSTATUS PsArm64InitializeUserProcessSpace(PEPROCESS process, ULONG_PTR *roots)
{
    MM_ARM64_USER_SPACE *space = &((XP_ARM64_USER_PROCESS *)process)->Space;
    RtlZeroMemory(space, sizeof(*space));
    XP_ARM64_USER_PROCESS *user = (XP_ARM64_USER_PROCESS *)process;
    RtlZeroMemory(user->UserDispatchers, sizeof(user->UserDispatchers));
    user->Win32Services = FALSE;
    PXP_ARM64_BOOT_BLOCK boot = CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt);
    ExInitializeFastMutex(&process->WorkingSetLock);
    ExInitializeFastMutex(&process->AddressCreationLock);
    KeInitializeSpinLock(&process->HyperSpaceLock);
    InitializeListHead(&process->PhysicalVadList);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64CreateUserSpace(space, boot);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (NT_SUCCESS(status)) {
        roots[0] = space->Root;
        roots[1] = boot->Tables.KernelRoot;
    }
    return status;
}

VOID PsArm64ReleaseUserProcessSpace(PEPROCESS process)
{
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(process);
    if (!space) KeBugCheckEx(MEMORY_MANAGEMENT, 0xa642, (ULONG_PTR)process, 0, 0);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    MmArm64DestroyUserSpace(space);
    process->Pcb.DirectoryTableBase[0] = 0;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
