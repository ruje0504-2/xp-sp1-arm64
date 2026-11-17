/* The object owns the full register record even if all creator handles close
 * before first dispatch. Only the private user TTBR0 maps the native TEB. */
typedef struct {
    ETHREAD Thread;
    KARM64_USER_ENTRY Entry;
} XP_ARM64_USER_THREAD;

static NTSTATUS initialize_user_teb(PETHREAD thread, const XP_ARM64_USER_START *start)
{
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(thread->ThreadsProcess);
    SIZE_T size = ROUND_TO_PAGES(sizeof(TEB));
    PTEB initial = ExAllocatePoolWithTag(PagedPool, sizeof(*initial), 'tUsP');
    if (!initial) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(initial, sizeof(*initial));
    initial->NtTib.Self = (PNT_TIB)start->Teb;
    initial->NtTib.StackBase = (PVOID)start->StackBase;
    initial->NtTib.StackLimit = (PVOID)start->StackLimit;
    initial->DeallocationStack = (PVOID)start->StackLimit;
    initial->ClientId = thread->Cid;
    initial->ProcessEnvironmentBlock = thread->ThreadsProcess->Peb;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64MapUserPages(space, start->Teb, size, PAGE_READWRITE);
    if (NT_SUCCESS(status)) {
        status = MmArm64CopyUserBuffer(space, start->Teb, initial, sizeof(*initial), TRUE);
        if (!NT_SUCCESS(status)) MmArm64UnmapUserPages(space, start->Teb, size);
        else thread->Tcb.Teb = (PVOID)start->Teb;
    }
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    ExFreePool(initial);
    return status;
}

VOID PsArm64ReleaseUserTeb(PETHREAD thread)
{
    if (!thread->Tcb.Teb) return;
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(thread->ThreadsProcess);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64UnmapUserPages(space, (ULONG_PTR)thread->Tcb.Teb, ROUND_TO_PAGES(sizeof(TEB)));
    thread->Tcb.Teb = NULL;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!NT_SUCCESS(status)) KeBugCheckEx(MEMORY_MANAGEMENT, 0xa643, (ULONG_PTR)thread, status, 0);
}

static VOID run_user_thread(PVOID parameter)
{
    KARM64_USER_ENTRY *entry = parameter;
    PETHREAD thread = PsGetCurrentThread();
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(thread->ThreadsProcess);
    entry->Space = space;
    entry->Frame.FpStateSaved = TRUE;
    entry->ApcDispatcher = PsArm64GetUserApcDispatcher(thread->ThreadsProcess);
    entry->ExceptionDispatcher = PsArm64GetUserExceptionDispatcher(thread->ThreadsProcess);
    entry->CallbackDispatcher = PsArm64GetUserCallbackDispatcher(thread->ThreadsProcess);
    if (PsArm64Win32ServicesEnabled(thread->ThreadsProcess))
        thread->Tcb.ServiceTable = &KeServiceDescriptorTableShadow[0];
    NTSTATUS status = KiArm64RunUserThread(entry);
    PsArm64ExitBootThread(thread, status);
}

static VOID user_thread_startup(PKSTART_ROUTINE routine, PVOID parameter)
{
    KeLowerIrql(PASSIVE_LEVEL);
    PETHREAD thread = PsGetCurrentThread();
    if (thread->CrossThreadFlags & PS_CROSS_THREAD_FLAGS_DEADTHREAD)
        PsArm64ExitBootThread(thread, STATUS_THREAD_IS_TERMINATING);
    routine(parameter);
    KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, (ULONG_PTR)thread, 0xa643, 0, 0);
}
