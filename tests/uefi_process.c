#include "../base/ntos/ex/arm64/bootwin32.h"
#include "../base/ntos/ke/arm64/userentry.h"
#include "../base/ntos/ps/arm64/bootprocess.h"
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/hal/arm64/gicv2.h"
#include "../base/ntos/hal/arm64/virtioinput.h"
#include "../base/ntos/mm/arm64/bootframes_private.h"
#include "../base/ntos/mm/arm64/bootsyspte.h"
#include "../base/ntos/mm/arm64/bootimage.h"
#include <ntimage.h>
#include <ntobapi.h>
#include "../base/ntos/ob/arm64/bootobject.h"
#include "../base/ntos/ex/arm64/bootworkers.h"
#include <atom.h>
#include "xp-token.h"
#include "xp-obcache.h"
#include "uefi_test.h"

extern POBJECT_TYPE ExEventObjectType;
extern FAST_MUTEX PspActiveProcessMutex;
extern LIST_ENTRY HandleTableListHead;
extern EPROCESS_QUOTA_BLOCK PspDefaultQuotaBlock;

VOID PspInheritQuota(PEPROCESS NewProcess, PEPROCESS Parent);

static PEPROCESS allocate_handle_owner(VOID)
{
    PEPROCESS process;
    CHECK(ObCreateObject(KernelMode, PsProcessType, NULL, UserMode, NULL, sizeof(*process),
          0, 0, (PVOID *)&process) == STATUS_SUCCESS);
    RtlZeroMemory(process, sizeof(*process));
    ULONG_PTR roots[2] = {PsIdleProcess->Pcb.DirectoryTableBase[0], PsIdleProcess->Pcb.DirectoryTableBase[1]};
    KeInitializeProcess(&process->Pcb, NORMAL_BASE_PRIORITY, KeActiveProcessors, roots, FALSE);
    ExInitializeRundownProtection(&process->RundownProtect);
    PspInheritQuota(process, PsInitialSystemProcess);
    CHECK(process->QuotaBlock == &PspDefaultQuotaBlock);
    return process;
}

typedef struct {
    WORK_QUEUE_ITEM Item;
    PEPROCESS Owner;
    KEVENT Done;
} HANDLE_RUNDOWN_WORK;

static VOID rundown_handle_owner(PVOID parameter)
{
    HANDLE_RUNDOWN_WORK *work = parameter;
    ObKillProcess(work->Owner);
    KeSetEvent(&work->Done, 0, FALSE);
}

static VOID check_process_handle_tables(VOID)
{
    PEPROCESS a = allocate_handle_owner(), b = allocate_handle_owner();
    HANDLE source[2], private[2];
    PKEVENT events[2];
    for (ULONG i = 0; i < 2; ++i) {
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        CHECK(NtCreateEvent(&source[i], EVENT_ALL_ACCESS, &attributes, NotificationEvent, FALSE) == STATUS_SUCCESS &&
              ObReferenceObjectByHandle(source[i], EVENT_QUERY_STATE, ExEventObjectType, KernelMode,
              (PVOID *)&events[i], NULL) == STATUS_SUCCESS);
    }
    PEPROCESS_QUOTA_ENTRY q = &PspDefaultQuotaBlock.QuotaEntry[PsPagedPool];
    SIZE_T initial = q->Usage, limit = q->Limit;
    q->Limit = initial + sizeof(HANDLE_TABLE);
    NTSTATUS status = ObInitProcess(NULL, a);
    q->Limit = limit;
    CHECK(status == STATUS_INSUFFICIENT_RESOURCES && !a->ObjectTable &&
          !a->QuotaUsage[PsPagedPool] && q->Usage == initial);
    CHECK(ObInitProcess(NULL, a) == STATUS_SUCCESS && ObInitProcess(NULL, b) == STATUS_SUCCESS &&
          a->ObjectTable != b->ObjectTable && a->ObjectTable->QuotaProcess == a &&
          b->ObjectTable->QuotaProcess == b && a->QuotaUsage[PsPagedPool] == sizeof(HANDLE_TABLE) + PAGE_SIZE);
    CHECK(ObArm64CreateProcessHandle(a, events[0], EVENT_QUERY_STATE, &private[0]) == STATUS_SUCCESS &&
          ObArm64CreateProcessHandle(b, events[1], EVENT_MODIFY_STATE, &private[1]) == STATUS_SUCCESS &&
          private[0] == private[1] && (ULONG_PTR)private[0] == 4);
    for (ULONG i = 0; i < 2; ++i) {
        PEPROCESS proc = i ? b : a;
        PHANDLE_TABLE table = ObReferenceProcessHandleTable(proc);
        CHECK(table == proc->ObjectTable);
        KeEnterCriticalRegion();
        PHANDLE_TABLE_ENTRY entry = ExMapHandleToPointer(table, private[i]);
        CHECK(entry && entry->Object == OBJECT_TO_OBJECT_HEADER(events[i]) &&
              entry->GrantedAccess == (i ? EVENT_MODIFY_STATE : EVENT_QUERY_STATE));
        ExUnlockHandleTableEntry(table, entry);
        KeLeaveCriticalRegion();
        ObDereferenceProcessHandleTable(proc);
    }
    for (ULONG i = 0; i < PAGE_SIZE / sizeof(HANDLE_TABLE_ENTRY); ++i) {
        HANDLE more;
        CHECK(ObArm64CreateProcessHandle(a, events[0], EVENT_QUERY_STATE, &more) == STATUS_SUCCESS);
    }
    ULONG count = a->ObjectTable->HandleCount;
    SIZE_T charge = a->QuotaUsage[PsPagedPool];
    CHECK((a->ObjectTable->TableCode & 3) == 1 && charge > b->QuotaUsage[PsPagedPool]);
    PHANDLE_TABLE held = ObReferenceProcessHandleTable(a);
    CHECK(held == a->ObjectTable);
    HANDLE_RUNDOWN_WORK work;
    work.Owner = a;
    KeInitializeEvent(&work.Done, NotificationEvent, FALSE);
    ExInitializeWorkItem(&work.Item, rundown_handle_owner, &work);
    ExQueueWorkItem(&work.Item, DelayedWorkQueue);
    LARGE_INTEGER delay = {.QuadPart = -10000}, timeout = {.QuadPart = -10000000};
    ULONGLONG deadline = KeQueryInterruptTime() + 10000000;
    while (!(__atomic_load_n(&a->RundownProtect.Count, __ATOMIC_ACQUIRE) & EX_RUNDOWN_ACTIVE)) {
        CHECK(KeQueryInterruptTime() < deadline);
        CHECK(KeDelayExecutionThread(KernelMode, FALSE, &delay) == STATUS_SUCCESS);
    }
    CHECK(!KeReadStateEvent(&work.Done) && !ObReferenceProcessHandleTable(a) &&
          a->ObjectTable == held && held->HandleCount == count);
    ObDereferenceProcessHandleTable(a);
    CHECK(KeWaitForSingleObject(&work.Done, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS);
    CHECK(!a->ObjectTable && !ObReferenceProcessHandleTable(a) && !a->QuotaUsage[PsPagedPool] &&
          OBJECT_TO_OBJECT_HEADER(events[0])->HandleCount == 1 && b->ObjectTable->HandleCount == 1);
    HANDLE rejected;
    CHECK(ObArm64CreateProcessHandle(a, events[0], EVENT_QUERY_STATE, &rejected) ==
          STATUS_PROCESS_IS_TERMINATING && !rejected);
    ObDereferenceObject(a);
    ObDereferenceObject(b); /* Final process deletion sweeps the remaining private handle. */
    CHECK(q->Usage == initial && OBJECT_TO_OBJECT_HEADER(events[1])->HandleCount == 1);
    for (ULONG i = 0; i < 2; ++i) {
        ObDereferenceObject(events[i]);
        CHECK(ObArm64CloseBootObjectHandle(source[i]) == STATUS_SUCCESS);
    }
    XpTestReport("Process tables evidence handle="); XpTestHex((ULONG_PTR)private[0]);
    XpTestReport(" swept="); XpTestHex(count);
    XpTestReport(" charge="); XpTestHex(charge); XpTestReport("\n");
    XpTestReport("PASS: XP process-owned handles, isolated lookup, quota rollback and exit sweep\n");
}

extern const UCHAR XpUserImage[], XpUserImageEnd[];
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, ExceptionAddress) == 16);
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, NumberParameters) == 24);
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, ExceptionInformation) == 32);
C_ASSERT(FIELD_OFFSET(TEB, ProcessEnvironmentBlock) == 0x60);
C_ASSERT(FIELD_OFFSET(PEB, ImageBaseAddress) == 0x10);
C_ASSERT(FIELD_OFFSET(TEB, NtTib.Self) == 0x30);
C_ASSERT(FIELD_OFFSET(TEB, ClientId) == 0x40);

typedef struct {
    KAPC Apc;
    ULONG Delivered;
    ULONG Rundown;
} USER_APC_PROBE;

static VOID check_apc_kernel_routine(PKAPC apc, PKNORMAL_ROUTINE *routine, PVOID *context,
                                     PVOID *argument1, PVOID *argument2)
{
    USER_APC_PROBE *probe = CONTAINING_RECORD(apc, USER_APC_PROBE, Apc);
    CHECK(KeGetCurrentIrql() == APC_LEVEL && *routine && *context && *argument1);
    ++probe->Delivered;
    *argument2 = (PVOID)((ULONG_PTR)*argument2 + 1);
}

static VOID check_apc_rundown(PKAPC apc)
{
    USER_APC_PROBE *probe = CONTAINING_RECORD(apc, USER_APC_PROBE, Apc);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && !apc->Inserted);
    ++probe->Rundown;
}

static VOID wait_for_user_delay(MM_ARM64_USER_SPACE *space, ULONG_PTR report, PKTHREAD thread,
                                ULONG64 wanted_stage)
{
    ULONGLONG deadline = KeQueryInterruptTime() + 20000000;
    LARGE_INTEGER poll = {.QuadPart = -10000};
    for (;;) {
        ULONG64 stage, daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        NTSTATUS status = MmArm64CopyUserBuffer(space, report + 176, &stage, sizeof(stage), FALSE);
        BOOLEAN waiting = thread->State == Waiting && thread->WaitMode == UserMode &&
                          thread->Alertable == (wanted_stage == 2);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        CHECK(NT_SUCCESS(status) && thread->State != Terminated && stage <= wanted_stage);
        if (stage == wanted_stage && waiting) return;
        CHECK(KeQueryInterruptTime() < deadline && KeDelayExecutionThread(KernelMode, FALSE, &poll) == STATUS_SUCCESS);
    }
}

static ULONG callback_unwound;

static ULONG_PTR check_win32_callback(ULONG_PTR api, ULONG_PTR cookie, ULONG_PTR unused2, ULONG_PTR unused3,
                                    ULONG_PTR unused4, ULONG_PTR unused5, ULONG_PTR unused6, ULONG_PTR unused7,
                                    ULONG_PTR unused8, ULONG_PTR unused9, ULONG_PTR unused10,
                                    ULONG_PTR unused11, ULONG_PTR unused12, ULONG_PTR unused13, ULONG_PTR unused14)
{
    UNREFERENCED_PARAMETER(unused2);
    UNREFERENCED_PARAMETER(unused3);
    UNREFERENCED_PARAMETER(unused4);
    UNREFERENCED_PARAMETER(unused5);
    UNREFERENCED_PARAMETER(unused6);
    UNREFERENCED_PARAMETER(unused7);
    UNREFERENCED_PARAMETER(unused8);
    UNREFERENCED_PARAMETER(unused9);
    UNREFERENCED_PARAMETER(unused10);
    if (api < 1 || api > 4 || cookie != 0x1234 + api) return STATUS_INVALID_PARAMETER;
    CHECK(KeGetPreviousMode() == UserMode);
    if (api == 2) {
        LARGE_INTEGER duration = {.QuadPart = -10000};
        CHECK(KeDelayExecutionThread(KernelMode, FALSE, &duration) == STATUS_SUCCESS &&
              KeGetPreviousMode() == UserMode);
    }
    ULONG64 input[2] = {cookie, 0x500 + api};
    PVOID parent = KeGetCurrentThread()->CallbackStack;
    PVOID output;
    ULONG length;
    NTSTATUS status = KeUserModeCallback((ULONG)api, input, sizeof(input), &output, &length);
    CHECK(KeGetCurrentThread()->CallbackStack == parent && KeGetPreviousMode() == UserMode);
    if (api >= 3) {
        CHECK(status == 42 && !output && !length);
        ++callback_unwound;
        return status;
    }
    CHECK(length == sizeof(ULONG64) && output &&
          status == (api == 1 ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL));
    ULONG64 value, daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS copy = MmArm64CopyUserBuffer(PsArm64UserProcessSpace(PsGetCurrentProcess()), (ULONG_PTR)output,
                                          &value, sizeof(value), FALSE);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(copy == STATUS_SUCCESS && value == 0x600 + api);
    return status;
}

static PKARM64_WIN32_SERVICE const win32_services[] = {check_win32_callback};

typedef struct {
    PEPROCESS Process;
    PETHREAD Creator, Remover;
    HANDLE Tid;
    KEVENT Entered;
    volatile BOOLEAN Removing;
    BOOLEAN ObservedWait;
    NTSTATUS RemoveStatus;
    ULONG Sequence;
} PS_NOTIFY_PROBE;

static PS_NOTIFY_PROBE *ps_notify_probe;

static VOID record_image_load(PUNICODE_STRING name, HANDLE pid, PIMAGE_INFO info)
{
    PS_NOTIFY_PROBE *probe = ps_notify_probe;
    if (pid != probe->Process->UniqueProcessId) return;
    CHECK(!probe->Sequence && KeGetCurrentIrql() == PASSIVE_LEVEL &&
          PsGetCurrentThread() == probe->Creator && !name && !info->SystemModeImage &&
          !info->ImageMappedToAllPids && info->ImageAddressingMode == IMAGE_ADDRESSING_MODE_32BIT &&
          info->ImageBase == probe->Process->SectionBaseAddress && probe->Process->Peb);
    const IMAGE_DOS_HEADER *dos = (const VOID *)XpUserImage;
    const IMAGE_NT_HEADERS64 *nt = (const VOID *)(XpUserImage + dos->e_lfanew);
    CHECK(info->ImageSize == nt->OptionalHeader.SizeOfImage);
    ++probe->Sequence;
}

static VOID record_process_notify(HANDLE parent, HANDLE pid, BOOLEAN create)
{
    PS_NOTIFY_PROBE *probe = ps_notify_probe;
    if (pid != probe->Process->UniqueProcessId) return;
    CHECK(parent == probe->Process->InheritedFromUniqueProcessId && KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (create)
        CHECK(probe->Sequence == 1 && PsGetCurrentThread() == probe->Creator &&
              probe->Process->ActiveThreads == 1);
    else
        CHECK(probe->Sequence == 4 && PsGetCurrentThread()->Cid.UniqueThread == probe->Tid &&
              !probe->Process->ActiveThreads && probe->Process->ObjectTable &&
              (probe->Process->Flags & PS_PROCESS_FLAGS_PROCESS_EXITING));
    ++probe->Sequence;
}

static VOID record_thread_notify(HANDLE pid, HANDLE tid, BOOLEAN create)
{
    PS_NOTIFY_PROBE *probe = ps_notify_probe;
    if (pid != probe->Process->UniqueProcessId) return;
    CHECK(tid && probe->Process->ActiveThreads == 1 && KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (create) {
        CHECK(probe->Sequence == 2 && PsGetCurrentThread() == probe->Creator);
        /* XP sends create notification before the thread's access is published. */
        PETHREAD unpublished = NULL;
        CHECK(PsLookupThreadByThreadId(tid, &unpublished) == STATUS_INVALID_PARAMETER && !unpublished);
        probe->Tid = tid;
    } else {
        CHECK(probe->Sequence == 3 && probe->Tid == tid && PsGetCurrentThread()->Cid.UniqueThread == tid &&
              PsGetCurrentThread()->Tcb.Teb);
    }
    ++probe->Sequence;
}

static VOID block_process_notify(HANDLE parent, HANDLE pid, BOOLEAN create)
{
    UNREFERENCED_PARAMETER(parent);
    PS_NOTIFY_PROBE *probe = ps_notify_probe;
    if (pid != probe->Process->UniqueProcessId) return;
    CHECK(create && !probe->ObservedWait);
    KeSetEvent(&probe->Entered, 0, FALSE);
    LARGE_INTEGER delay = {.QuadPart = -10000};
    ULONG64 deadline = KeQueryInterruptTime() + 5000000;
    while (!probe->Removing || probe->Remover->Tcb.State != Waiting)
        CHECK(KeQueryInterruptTime() < deadline &&
              KeDelayExecutionThread(KernelMode, FALSE, &delay) == STATUS_SUCCESS);
    CHECK(probe->RemoveStatus == STATUS_PENDING && probe->Remover->Tcb.WaitReason == Executive &&
          probe->Remover->Tcb.WaitBlockList && probe->Remover->Tcb.WaitBlockList->Object != &probe->Entered);
    probe->ObservedWait = TRUE;
}

static VOID remove_process_notify(PVOID context)
{
    PS_NOTIFY_PROBE *probe = context;
    LARGE_INTEGER deadline = {.QuadPart = -10000000};
    CHECK(KeWaitForSingleObject(&probe->Entered, Executive, KernelMode, FALSE, &deadline) == STATUS_SUCCESS);
    probe->Removing = TRUE;
    probe->RemoveStatus = PsSetCreateProcessNotifyRoutine(block_process_notify, TRUE);
    CHECK(probe->ObservedWait && probe->RemoveStatus == STATUS_SUCCESS);
}

static VOID check_child_thread(PEPROCESS process, HANDLE process_handle)
{
    XP_ARM64_USER_START start = {0x20000, 0x51000, 0x50000, 0x70000, 0x10000};
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(process);
    SIZE_T length = (ULONG_PTR)XpUserImageEnd - (ULONG_PTR)XpUserImage;
    ULONG_PTR entry, peb = 0x90000;
    PS_NOTIFY_PROBE notify = {.Process = process, .Creator = PsGetCurrentThread(), .RemoveStatus = STATUS_PENDING};
    KeInitializeEvent(&notify.Entered, SynchronizationEvent, FALSE);
    ps_notify_probe = &notify;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE remover;
    CHECK(PsCreateSystemThread(&remover, THREAD_ALL_ACCESS, &attributes, NULL, NULL,
                               remove_process_notify, &notify) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(remover, SYNCHRONIZE, PsThreadType, KernelMode,
                                    (PVOID *)&notify.Remover, NULL) == STATUS_SUCCESS);
    CHECK(PsSetCreateProcessNotifyRoutine(record_process_notify, FALSE) == STATUS_SUCCESS &&
          PsSetCreateProcessNotifyRoutine(block_process_notify, FALSE) == STATUS_SUCCESS &&
          PsSetCreateThreadNotifyRoutine(record_thread_notify) == STATUS_SUCCESS &&
          PsSetLoadImageNotifyRoutine(record_image_load) == STATUS_SUCCESS);
    CHECK(PsArm64InitializeProcessImage(process, XpUserImage, sizeof(IMAGE_DOS_HEADER), peb, &entry) ==
          STATUS_INVALID_IMAGE_FORMAT && !entry && !process->Peb);
    CHECK(PsArm64InitializeProcessImage(process, XpUserImage, length, start.Argument, &entry) ==
          STATUS_CONFLICTING_ADDRESSES && !entry && !process->SectionBaseAddress && !process->Peb && !notify.Sequence);
    CHECK(PsArm64InitializeProcessImage(process, XpUserImage, length, peb, &entry) == STATUS_SUCCESS);
    start.Pc = entry;
    CHECK(PsArm64SetUserExceptionDispatcher(process, peb) == STATUS_ACCESS_VIOLATION &&
          PsArm64SetUserExceptionDispatcher(process, entry+4) == STATUS_SUCCESS &&
          PsArm64SetUserApcDispatcher(process, entry+8) == STATUS_SUCCESS &&
          PsArm64SetUserCallbackDispatcher(process, entry+16) == STATUS_SUCCESS);
    CHECK(KeAddSystemServiceTable((PULONG_PTR)win32_services, NULL, 1, NULL, WIN32K_SERVICE_INDEX) &&
          PsArm64EnableWin32Services(process) == STATUS_SUCCESS);
    CHECK(PsArm64InitializeProcessImage(process, XpUserImage, length, peb, &entry) == STATUS_INVALID_DEVICE_STATE && !entry);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    PEB copied;
    CHECK(MmArm64CopyUserBuffer(space, peb, &copied, sizeof(copied), FALSE) == STATUS_SUCCESS &&
          copied.ImageBaseAddress == process->SectionBaseAddress && process->Peb == (PPEB)peb &&
          copied.NumberOfProcessors == KeNumberProcessors && copied.OSBuildNumber == 2600 &&
          copied.OSMajorVersion == 5 && copied.OSMinorVersion == 1 &&
          copied.ImageSubsystem == IMAGE_SUBSYSTEM_NATIVE && copied.ImageSubsystemMajorVersion == 5 &&
          copied.ImageSubsystemMinorVersion == 1 && copied.ProcessHeaps == (PVOID *)(peb + sizeof(PEB)) &&
          copied.MaximumNumberOfHeaps == (PAGE_SIZE - sizeof(PEB)) / sizeof(PVOID));
    CHECK(MmArm64CopyUserBuffer(space, (ULONG_PTR)copied.ImageBaseAddress, &entry, sizeof(entry), TRUE) ==
          STATUS_ACCESS_VIOLATION && MmArm64CopyUserBuffer(space, start.Pc, &entry, sizeof(entry), TRUE) ==
          STATUS_ACCESS_VIOLATION);
    CHECK(MmArm64MapUserPages(space, start.StackLimit, PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    HANDLE handle;
    CLIENT_ID cid;
    ULONG_PTR teb = start.Teb;
    start.Teb = (ULONG_PTR)copied.ImageBaseAddress;
    CHECK(PsArm64CreateUserThread(process_handle, &start, &handle, &cid) == STATUS_CONFLICTING_ADDRESSES &&
          !handle && !cid.UniqueThread && !process->ActiveThreads && !process->Pcb.Header.SignalState && notify.Sequence == 1);
    start.Teb = teb;
    CHECK(PsArm64CreateUserThread(process_handle, &start, &handle, &cid) == STATUS_SUCCESS &&
          cid.UniqueProcess == process->UniqueProcessId && cid.UniqueThread == notify.Tid && notify.ObservedWait);
    LARGE_INTEGER remove_limit = {.QuadPart = -10000000};
    CHECK(KeWaitForSingleObject(&notify.Remover->Tcb, Executive, KernelMode, FALSE, &remove_limit) == STATUS_SUCCESS &&
          notify.Remover->ExitStatus == STATUS_SUCCESS && notify.RemoveStatus == STATUS_SUCCESS);
    ObDereferenceObject(notify.Remover);
    CHECK(ObArm64CloseBootObjectHandle(remover) == STATUS_SUCCESS);
    PETHREAD thread;
    CHECK(ObReferenceObjectByHandle(handle, SYNCHRONIZE, PsThreadType, KernelMode, (PVOID *)&thread, NULL) ==
          STATUS_SUCCESS && thread->ThreadsProcess == process && thread->Cid.UniqueThread == cid.UniqueThread &&
          !(thread->CrossThreadFlags & PS_CROSS_THREAD_FLAGS_SYSTEM));
    PETHREAD published;
    CHECK(PsLookupThreadByThreadId(cid.UniqueThread, &published) == STATUS_SUCCESS && published == thread);
    ObDereferenceObject(published);
    USER_APC_PROBE apcs[3] = {0};
    for (ULONG index = 0; index < (sizeof(apcs) / sizeof(apcs[0])); ++index) {
        wait_for_user_delay(space, start.Argument, &thread->Tcb, index + 1);
        KeInitializeApc(&apcs[index].Apc, &thread->Tcb, OriginalApcEnvironment, check_apc_kernel_routine,
                        check_apc_rundown, (PKNORMAL_ROUTINE)(start.Pc + 12), UserMode, (PVOID)start.Argument);
        CHECK(KeInsertQueueApc(&apcs[index].Apc, (PVOID)(ULONG_PTR)(0x100 + index),
                              (PVOID)(ULONG_PTR)(0x200 + index), 0));
    }
    LARGE_INTEGER timeout = {.QuadPart = -20000000};
    CHECK(KeWaitForSingleObject(&process->Pcb, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS &&
          KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS);
    CHECK(process->ExitStatus == 42 && thread->ExitStatus == 42 && !process->ActiveThreads &&
          !process->ObjectTable && !thread->Tcb.Teb && thread->Tcb.State == Terminated &&
          !process->Pcb.StackCount && IsListEmpty(&process->Pcb.ThreadListHead));
    CHECK(!thread->Tcb.CallbackStack && callback_unwound == 2 && KeRemoveSystemServiceTable(WIN32K_SERVICE_INDEX));
    ULONG64 result[36] = {0};
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument, result, sizeof(result), FALSE) == STATUS_SUCCESS &&
          MmArm64CopyUserBuffer(space, teb, result, sizeof(result), FALSE) == STATUS_NOT_MAPPED_VIEW);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(result[0] == teb && result[1] == (ULONG_PTR)cid.UniqueProcess && result[2] == (ULONG_PTR)cid.UniqueThread &&
          result[3] && result[4] && (ULONG)result[5] == (ULONG)STATUS_CANT_TERMINATE_SELF);
    CHECK(result[6] == peb && result[7] == (ULONG_PTR)copied.ImageBaseAddress &&
          result[8] == IMAGE_DOS_SIGNATURE && result[9] == start.Pc && result[10] == 42);
    CHECK(result[11] >= start.Pc && result[11] < start.Pc + PAGE_SIZE && result[12] == result[7] &&
          result[13] == 1 && result[14] > result[11] && result[14] < start.Pc + PAGE_SIZE &&
          result[15] >= start.StackLimit && result[15] < result[21] && result[21] < start.StackBase &&
          (ULONG)result[16] == (ULONG)STATUS_DATATYPE_MISALIGNMENT &&
          (ULONG)result[17] == (ULONG)STATUS_ACCESS_VIOLATION && (ULONG)result[18] == (ULONG)STATUS_INVALID_PARAMETER &&
          result[19] == 0x5678 && result[20] == 0x1234);
    XpTestReport("User exception evidence fault="); XpTestHex(result[11]);
    XpTestReport(" address="); XpTestHex(result[12]); XpTestReport(" inner_sp="); XpTestHex(result[15]);
    XpTestReport(" outer_sp="); XpTestHex(result[21]); XpTestReport(" x0="); XpTestHex(result[19]);
    XpTestReport(" d7="); XpTestHex(result[20]); XpTestReport("\n");
    XpTestReport("PASS: native EL0 exception frames, nested breakpoint and NtContinue restoration\n");
    XpTestReport("Native image evidence peb="); XpTestHex(result[6]);
    XpTestReport(" base="); XpTestHex(result[7]); XpTestReport(" entry="); XpTestHex(result[9]);
    XpTestReport(" build="); XpTestHex(copied.OSBuildNumber); XpTestReport("\n");
    XpTestReport("PASS: linked ARM64 user image entry and TEB to PEB identity in EL0\n");
    CHECK(result[22] == 3 && result[23] == 2 && result[24] == STATUS_USER_APC &&
          result[25] == 0x100 && result[26] == 0x201 && result[27] == 0x101 && result[28] == 0x202 &&
          apcs[0].Delivered == 1 && !apcs[0].Rundown && apcs[1].Delivered == 1 && !apcs[1].Rundown &&
          !apcs[2].Delivered && apcs[2].Rundown == 1);
    XpTestReport("User APC evidence calls="); XpTestHex(result[23]);
    XpTestReport(" alertable_status="); XpTestHex(result[24]);
    XpTestReport(" rundown="); XpTestHex(apcs[2].Rundown); XpTestReport("\n");
    XpTestReport("PASS: native user APC test-alert, alertable wakeup and exit rundown\n");
    CHECK((ULONG)result[29] == (ULONG)STATUS_NO_CALLBACK_ACTIVE &&
          (ULONG)result[30] == (ULONG)STATUS_ACCESS_VIOLATION &&
          (ULONG)result[31] == (ULONG)STATUS_BUFFER_TOO_SMALL && result[32] == STATUS_SUCCESS &&
          result[33] >= start.StackLimit && result[33] < result[34] && result[34] < start.StackBase &&
          (ULONG)result[35] == (ULONG)STATUS_NO_CALLBACK_ACTIVE);
    XpTestReport("User callback evidence inner_status="); XpTestHex((ULONG)result[31]);
    XpTestReport(" outer_status="); XpTestHex(result[32]);
    XpTestReport(" inner_sp="); XpTestHex(result[33]); XpTestReport(" outer_sp="); XpTestHex(result[34]);
    XpTestReport(" unwind="); XpTestHex(callback_unwound); XpTestReport("\n");
    XpTestReport("PASS: nested native Win32 user callbacks and NtCallbackReturn\n");
    HANDLE late;
    CHECK(PsArm64CreateUserThread(process_handle, &start, &late, NULL) == STATUS_PROCESS_IS_TERMINATING && !late);
    /* The reaper drops its thread reference after retiring the stack. */
    extern BOOLEAN PsReaperActive;
    ULONGLONG deadline = KeQueryInterruptTime() + 10000000;
    LARGE_INTEGER delay = {.QuadPart = -10000};
    while (thread->Tcb.InitialStack || PsReaperActive) {
        CHECK(KeQueryInterruptTime() < deadline &&
              KeDelayExecutionThread(KernelMode, FALSE, &delay) == STATUS_SUCCESS);
    }
    XpTestReport("User thread evidence pid="); XpTestHex(result[1]); XpTestReport(" tid="); XpTestHex(result[2]);
    XpTestReport(" teb="); XpTestHex(result[0]); XpTestReport(" frequency="); XpTestHex(result[4]);
    XpTestReport(" exit="); XpTestHex(thread->ExitStatus); XpTestReport("\n");
    ObDereferenceObject(thread);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);
    XpTestReport("PASS: scheduled child EL0 thread, TEB identity, native service and last-thread process exit\n");
    CHECK(notify.Sequence == 5);
    CHECK(PsSetCreateProcessNotifyRoutine(record_process_notify, TRUE) == STATUS_SUCCESS &&
          PsRemoveCreateThreadNotifyRoutine(record_thread_notify) == STATUS_SUCCESS &&
          PsRemoveLoadImageNotifyRoutine(record_image_load) == STATUS_SUCCESS);
    CHECK(PsSetCreateProcessNotifyRoutine(block_process_notify, TRUE) == STATUS_PROCEDURE_NOT_FOUND &&
          PsSetCreateProcessNotifyRoutine(record_process_notify, TRUE) == STATUS_PROCEDURE_NOT_FOUND &&
          PsRemoveCreateThreadNotifyRoutine(record_thread_notify) == STATUS_PROCEDURE_NOT_FOUND &&
          PsRemoveLoadImageNotifyRoutine(record_image_load) == STATUS_PROCEDURE_NOT_FOUND);
    ps_notify_probe = NULL;
    XpTestReport("PS notify evidence callbacks="); XpTestHex(notify.Sequence);
    XpTestReport(" rundown_wait="); XpTestHex(notify.ObservedWait);
    XpTestReport(" removal="); XpTestHex(notify.RemoveStatus); XpTestReport("\n");
    XpTestReport("PASS: XP process, thread and image notifications with in-flight rundown\n");
}

static VOID check_child_process(VOID)
{
    HANDLE child_handle, grandchild_handle, denied;
    ULONG tokens = SeTokenObjectType->TotalNumberOfObjects;
    ULONG processes = PsProcessType->TotalNumberOfObjects;
    ULONG cids = PspCidTable->HandleCount;
    SIZE_T quota_before = PspDefaultQuotaBlock.QuotaEntry[PsPagedPool].Usage;
    CHECK(ObArm64CreateBootHandle(PsInitialSystemProcess, PROCESS_QUERY_INFORMATION, TRUE, &denied) ==
          STATUS_SUCCESS);
    CHECK(PsArm64CreateUserProcess(denied, &child_handle) == STATUS_ACCESS_DENIED && !child_handle);
    CHECK(ObArm64CloseBootObjectHandle(denied) == STATUS_SUCCESS);
    PEPROCESS_QUOTA_ENTRY quota = &PspDefaultQuotaBlock.QuotaEntry[PsPagedPool];
    SIZE_T limit = quota->Limit;
    quota->Limit = quota->Usage;
    NTSTATUS status = PsArm64CreateUserProcess(PspInitialSystemProcessHandle, &child_handle);
    quota->Limit = limit;
    CHECK(!NT_SUCCESS(status) && !child_handle && SeTokenObjectType->TotalNumberOfObjects == tokens &&
          PsProcessType->TotalNumberOfObjects == processes && PspCidTable->HandleCount == cids &&
          quota->Usage == quota_before);
    CHECK(PsArm64CreateUserProcess(PspInitialSystemProcessHandle, &child_handle) == STATUS_SUCCESS &&
          PsArm64CreateUserProcess(child_handle, &grandchild_handle) == STATUS_SUCCESS);
    PEPROCESS child, grandchild;
    CHECK(ObReferenceObjectByHandle(child_handle, 0, PsProcessType, KernelMode, (PVOID *)&child, NULL) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(grandchild_handle, 0, PsProcessType, KernelMode,
                                    (PVOID *)&grandchild, NULL) == STATUS_SUCCESS);
    HANDLE pid = child->UniqueProcessId, gpid = grandchild->UniqueProcessId;
    CHECK(pid && gpid && pid != gpid && child->InheritedFromUniqueProcessId == PsInitialSystemProcess->UniqueProcessId &&
          grandchild->InheritedFromUniqueProcessId == pid && child->ObjectTable->UniqueProcessId == pid &&
          grandchild->ObjectTable->UniqueProcessId == gpid && child->ObjectTable != grandchild->ObjectTable);
    PEPROCESS lookup;
    CHECK(PsLookupProcessByProcessId(pid, &lookup) == STATUS_SUCCESS && lookup == child);
    ObDereferenceObject(lookup);
    CHECK(PsLookupProcessByProcessId(gpid, &lookup) == STATUS_SUCCESS && lookup == grandchild);
    ObDereferenceObject(lookup);
    PTOKEN parent_token = ExFastRefGetObject(PsInitialSystemProcess->Token);
    PTOKEN a = ExFastRefGetObject(child->Token), b = ExFastRefGetObject(grandchild->Token);
    CHECK(tokens + 2 == SeTokenObjectType->TotalNumberOfObjects && a && b && a != b && a != parent_token &&
          a->TokenInUse && b->TokenInUse && !RtlEqualLuid(&a->TokenId, &parent_token->TokenId) &&
          !RtlEqualLuid(&a->TokenId, &b->TokenId) && RtlEqualLuid(&a->AuthenticationId, &b->AuthenticationId) &&
          RtlEqualLuid(&a->AuthenticationId, &parent_token->AuthenticationId) && a->UserAndGroupCount == b->UserAndGroupCount &&
          a->UserAndGroups != b->UserAndGroups && a->PrivilegeCount == b->PrivilegeCount &&
          a->Privileges != b->Privileges && !memcmp(a->Privileges, b->Privileges, a->PrivilegeCount * sizeof(*a->Privileges)) &&
          a->DynamicPart != b->DynamicPart && a->DefaultDacl != b->DefaultDacl &&
          a->DefaultDacl->AclSize == b->DefaultDacl->AclSize && !memcmp(a->DefaultDacl, b->DefaultDacl, a->DefaultDacl->AclSize));
    for (ULONG i = 0; i < a->UserAndGroupCount; ++i)
        CHECK(a->UserAndGroups[i].Sid != b->UserAndGroups[i].Sid &&
              RtlEqualSid(a->UserAndGroups[i].Sid, b->UserAndGroups[i].Sid));
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(grandchild);
    ULONG64 root = PsArm64UserProcessSpace(child)->Root;
    CHECK(root && space->Root != root && !PsArm64UserProcessSpace(PsInitialSystemProcess));
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    CHECK(MmArm64MapUserPages(space, 0x10000, PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(ObArm64CloseBootObjectHandle(child_handle) == STATUS_SUCCESS);
    ObDereferenceObject(child);
    CHECK(!NT_SUCCESS(PsLookupProcessByProcessId(pid, &lookup)) && b->TokenInUse && RtlValidSid(b->PrimaryGroup));
    check_child_thread(grandchild, grandchild_handle);
    CHECK(ObArm64CloseBootObjectHandle(grandchild_handle) == STATUS_SUCCESS);
    ObDereferenceObject(grandchild);
    CHECK(!NT_SUCCESS(PsLookupProcessByProcessId(gpid, &lookup)) && PspCidTable->HandleCount == cids &&
          quota->Usage == quota_before && SeTokenObjectType->TotalNumberOfObjects == tokens &&
          PsProcessType->TotalNumberOfObjects == processes);
    XpTestReport("Child process evidence pid="); XpTestHex((ULONG_PTR)pid);
    XpTestReport(" grandchild="); XpTestHex((ULONG_PTR)gpid);
    XpTestReport(" root="); XpTestHex(root); XpTestReport("\n");
    XpTestReport("PASS: XP child process CID, copied primary token, private address space and handle rundown\n");
}

static VOID unexpected_start(PKSTART_ROUTINE routine, PVOID context)
{
    KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, (ULONG_PTR)routine, (ULONG_PTR)context, 0, 0);
}

static VOID unscheduled_thread(PVOID context)
{
    KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, (ULONG_PTR)context, 0, 0, 0);
}

VOID XpCheckBootProcessObjects(PXP_ARM64_BOOT_BLOCK block)
{
    PEPROCESS idle = PsGetCurrentProcess();
    PTOKEN token = PspBootAccessToken;
    CHECK(idle == PsIdleProcess && idle == &block->InitialProcess && token &&
          token == ExFastRefGetObject(idle->Token) && !token->TokenInUse);
    CHECK(PspCidTable && PspCidTable->StrictFIFO && !PspCidTable->HandleCount &&
          IsListEmpty(&PspCidTable->HandleTableList) &&
          HandleTableListHead.Flink == &idle->ObjectTable->HandleTableList &&
          HandleTableListHead.Blink == &idle->ObjectTable->HandleTableList);
    CHECK(IsListEmpty(&PsActiveProcessHead) && IsListEmpty(&idle->ThreadListHead));
    CHECK(PsProcessType->TypeInfo.SecurityRequired && PsThreadType->TypeInfo.SecurityRequired &&
          PsProcessType->TypeInfo.ValidAccessMask == PROCESS_ALL_ACCESS &&
          PsThreadType->TypeInfo.ValidAccessMask == THREAD_ALL_ACCESS &&
          PsProcessType->TypeInfo.DeleteProcedure && PsThreadType->TypeInfo.DeleteProcedure);
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG stacks_before = RtlNumberOfSetBits(&block->Tables.StackBitmap);
    LONG token_refs = OBJECT_TO_OBJECT_HEADER(token)->PointerCount;
    PEPROCESS_QUOTA_BLOCK quota = idle->QuotaBlock;
    CHECK(quota == &PspDefaultQuotaBlock);
    ULONG quota_refs = quota->ReferenceCount;
    SIZE_T paged_usage = quota->QuotaEntry[PsPagedPool].Usage;
    SIZE_T nonpaged_usage = quota->QuotaEntry[PsNonPagedPool].Usage;

    PEPROCESS process = NULL;
    PETHREAD thread = NULL;
    CHECK(ObCreateObject(KernelMode, PsProcessType, NULL, UserMode, NULL, sizeof(*process),
          0, 0, (PVOID *)&process) == STATUS_SUCCESS && process);
    RtlZeroMemory(process, sizeof(*process));
    ULONG_PTR roots[2] = {0};
    KeEnterCriticalRegion();
    MmInitializeHandBuiltProcess(process, roots);
    KeLeaveCriticalRegion();
    CHECK(roots[0] && roots[1] && roots[0] == block->Tables.Root && roots[1] == block->Tables.KernelRoot &&
          process->WorkingSetLock.Count == 1 && process->AddressCreationLock.Count == 1 &&
          process->Vm.WorkingSetSize == idle->Vm.WorkingSetSize &&
          process->Vm.VmWorkingSetList == idle->Vm.VmWorkingSetList);
    KeInitializeProcess(&process->Pcb, idle->Pcb.BasePriority, idle->Pcb.Affinity, roots, FALSE);
    ExInitializePushLock(&process->ProcessLock);
    ExInitializeRundownProtection(&process->RundownProtect);
    InitializeListHead(&process->ThreadListHead);
    process->GrantedAccess = PROCESS_ALL_ACCESS;
    CHECK(PspInitializeProcessSecurity(NULL, process) == STATUS_SUCCESS);
    CHECK(ExFastRefGetObject(process->Token) == token && token->TokenInUse &&
          OBJECT_TO_OBJECT_HEADER(token)->PointerCount ==
            token_refs + 1 + ExFastRefGetAdditionalReferenceCount());
    HANDLE handle;
    PVOID inserted;
    CHECK(ObInsertObject(process, NULL, GENERIC_READ, 1, &inserted, &handle) == STATUS_SUCCESS &&
          handle && (LONG_PTR)handle > 0 && inserted == process);
    POBJECT_HEADER process_header = OBJECT_TO_OBJECT_HEADER(process);
    CHECK(!(process_header->Flags & OB_FLAG_NEW_OBJECT) && process_header->QuotaBlockCharged == quota &&
          process_header->PointerCount == 2 && process_header->HandleCount == 1 &&
          quota->ReferenceCount == quota_refs + 1);
    SIZE_T paged_delta = quota->QuotaEntry[PsPagedPool].Usage - paged_usage;
    SIZE_T nonpaged_delta = quota->QuotaEntry[PsNonPagedPool].Usage - nonpaged_usage;
    CHECK(paged_delta > 0 && nonpaged_delta >= sizeof(EPROCESS));
    EX_FAST_REF security = {.Object = process_header->SecurityDescriptor};
    PSECURITY_DESCRIPTOR descriptor = ExFastRefGetObject(security);
    CHECK(descriptor);
    PSID owner, group;
    PACL dacl;
    BOOLEAN present, defaulted;
    CHECK(RtlGetOwnerSecurityDescriptor(descriptor, &owner, &defaulted) == STATUS_SUCCESS &&
          RtlEqualSid(owner, SeAliasAdminsSid));
    CHECK(RtlGetGroupSecurityDescriptor(descriptor, &group, &defaulted) == STATUS_SUCCESS &&
          RtlEqualSid(group, SeLocalSystemSid));
    CHECK(RtlGetDaclSecurityDescriptor(descriptor, &present, &dacl, &defaulted) == STATUS_SUCCESS &&
          present && defaulted && dacl && dacl->AceCount == 2);
    PVOID ace;
    CHECK(RtlGetAce(dacl, 0, &ace) == STATUS_SUCCESS && ((PACCESS_ALLOWED_ACE)ace)->Mask == PROCESS_ALL_ACCESS);
    PVOID by_handle = NULL;
    CHECK(ObReferenceObjectByHandle(handle, PROCESS_QUERY_INFORMATION, PsProcessType,
          KernelMode, &by_handle, NULL) == STATUS_SUCCESS && by_handle == process);
    ObDereferenceObject(by_handle);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS && process_header->PointerCount == 1);

    HANDLE_TABLE_ENTRY cid_entry = {0};
    cid_entry.Object = process;
    HANDLE pid = ExCreateHandle(PspCidTable, &cid_entry);
    CHECK(pid);
    process->UniqueProcessId = pid;
    ExAcquireFastMutex(&PspActiveProcessMutex);
    InsertTailList(&PsActiveProcessHead, &process->ActiveProcessLinks);
    ExReleaseFastMutex(&PspActiveProcessMutex);

    OBJECT_ATTRIBUTES explicit_attributes;
    InitializeObjectAttributes(&explicit_attributes, NULL, 0, NULL, SePublicDefaultSd);
    CHECK(ObCreateObject(KernelMode, PsThreadType, &explicit_attributes, UserMode, NULL, sizeof(*thread),
          0, 0, (PVOID *)&thread) == STATUS_SUCCESS && thread);
    RtlZeroMemory(thread, sizeof(*thread));
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = KeInitThread(&thread->Tcb, NULL, unexpected_start, unscheduled_thread,
                                  NULL, NULL, NULL, &process->Pcb);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(status == STATUS_SUCCESS && thread->Tcb.InitialStack &&
          RtlNumberOfSetBits(&block->Tables.StackBitmap) == stacks_before + 1);
    ExInitializeRundownProtection(&thread->RundownProtect);
    ObReferenceObject(process);
    thread->ThreadsProcess = process;
    thread->Cid.UniqueProcess = pid;
    thread->GrantedAccess = THREAD_ALL_ACCESS;
    cid_entry.Object = thread;
    HANDLE tid = ExCreateHandle(PspCidTable, &cid_entry);
    CHECK(tid && tid != pid && PspCidTable->HandleCount == 2);
    thread->Cid.UniqueThread = tid;
    CHECK(ObInsertObject(thread, NULL, 0, 0, NULL, NULL) == STATUS_SUCCESS &&
          !(OBJECT_TO_OBJECT_HEADER(thread)->Flags & OB_FLAG_NEW_OBJECT) &&
          OBJECT_TO_OBJECT_HEADER(thread)->QuotaBlockCharged == quota &&
          quota->ReferenceCount == quota_refs + 2);
    security.Object = OBJECT_TO_OBJECT_HEADER(thread)->SecurityDescriptor;
    descriptor = ExFastRefGetObject(security);
    CHECK(descriptor && RtlGetDaclSecurityDescriptor(descriptor, &present, &dacl, &defaulted) == STATUS_SUCCESS &&
          present && !defaulted && dacl && dacl->AceCount == 3);
    CHECK(RtlGetAce(dacl, 0, &ace) == STATUS_SUCCESS &&
          ((PACCESS_ALLOWED_ACE)ace)->Mask == PsThreadType->TypeInfo.GenericMapping.GenericExecute);

    PEPROCESS found_process = NULL;
    PETHREAD found_thread = NULL;
    CHECK(PsLookupProcessByProcessId(pid, &found_process) == STATUS_SUCCESS && found_process == process);
    ObDereferenceObject(found_process);
    CHECK(PsLookupThreadByThreadId(tid, &found_thread) == STATUS_SUCCESS && found_thread == thread);
    ObDereferenceObject(found_thread);
    CHECK(PsLookupProcessThreadByCid(&thread->Cid, &found_process, &found_thread) == STATUS_SUCCESS &&
          found_process == process && found_thread == thread);
    ObDereferenceObject(found_thread);
    ObDereferenceObject(found_process);
    CLIENT_ID wrong = {tid, tid};
    CHECK(PsLookupProcessThreadByCid(&wrong, NULL, &found_thread) == STATUS_INVALID_CID);
    CHECK(PsLookupProcessByProcessId(tid, &found_process) == STATUS_INVALID_PARAMETER &&
          PsLookupThreadByThreadId(pid, &found_thread) == STATUS_INVALID_PARAMETER);
    CHECK(OBJECT_TO_OBJECT_HEADER(process)->PointerCount == 2 &&
          OBJECT_TO_OBJECT_HEADER(thread)->PointerCount == 1);
    ObDereferenceObject(thread);
    CHECK(PsLookupThreadByThreadId(tid, &found_thread) == STATUS_INVALID_PARAMETER &&
          PspCidTable->HandleCount == 1 && OBJECT_TO_OBJECT_HEADER(process)->PointerCount == 1 &&
          RtlNumberOfSetBits(&block->Tables.StackBitmap) == stacks_before);
    ObDereferenceObject(process);
    CHECK(PsLookupProcessByProcessId(pid, &found_process) == STATUS_INVALID_PARAMETER &&
          !PspCidTable->HandleCount && IsListEmpty(&PsActiveProcessHead) &&
          !PsProcessType->TotalNumberOfObjects && !PsThreadType->TotalNumberOfObjects &&
          !token->TokenInUse && OBJECT_TO_OBJECT_HEADER(token)->PointerCount == token_refs &&
          quota->ReferenceCount == quota_refs && quota->QuotaEntry[PsPagedPool].Usage == paged_usage &&
          quota->QuotaEntry[PsNonPagedPool].Usage == nonpaged_usage);
    for (ULONG i = 0; i < SECURITY_DESCRIPTOR_CACHE_ENTRIES; ++i)
        CHECK(IsListEmpty(&ObsSecurityDescriptorCache[i].Head));

    PVOID rejected = NULL;
    CHECK(ObCreateObject(KernelMode, PsProcessType, NULL, UserMode, NULL, sizeof(EPROCESS),
          0, 0, &rejected) == STATUS_SUCCESS && rejected);
    RtlZeroMemory(rejected, sizeof(EPROCESS));
    SIZE_T limit = quota->QuotaEntry[PsNonPagedPool].Limit;
    quota->QuotaEntry[PsNonPagedPool].Limit = nonpaged_usage;
    status = ObInsertObject(rejected, NULL, GENERIC_READ, 0, NULL, &handle);
    quota->QuotaEntry[PsNonPagedPool].Limit = limit;
    CHECK(status == STATUS_QUOTA_EXCEEDED && !handle && !PsProcessType->TotalNumberOfObjects &&
          quota->ReferenceCount == quota_refs && quota->QuotaEntry[PsPagedPool].Usage == paged_usage &&
          quota->QuotaEntry[PsNonPagedPool].Usage == nonpaged_usage);
    for (ULONG i = 0; i < SECURITY_DESCRIPTOR_CACHE_ENTRIES; ++i)
        CHECK(IsListEmpty(&ObsSecurityDescriptorCache[i].Head));
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Process phase 0 evidence pid="); XpTestHex((ULONG_PTR)pid);
    XpTestReport(" tid="); XpTestHex((ULONG_PTR)tid);
    XpTestReport(" token_refs="); XpTestHex(token_refs);
    XpTestReport(" cid_live_after="); XpTestHex(PspCidTable->HandleCount);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original process and thread types, private CID lookup and boot token handoff\n");
    XpTestReport("Unnamed insertion evidence paged="); XpTestHex(paged_delta);
    XpTestReport(" nonpaged="); XpTestHex(nonpaged_delta);
    XpTestReport(" quota_failure="); XpTestHex((ULONG)status);
    XpTestReport(" root0="); XpTestHex(roots[0]);
    XpTestReport(" root1="); XpTestHex(roots[1]); XpTestReport("\n");
    XpTestReport("PASS: unnamed object insertion, default security, quota rollback and shared boot address space\n");
}

VOID XpCheckBootSystemProcess(PXP_ARM64_BOOT_BLOCK block)
{
    PEPROCESS idle = PsIdleProcess;
    PTOKEN token = PspBootAccessToken;
    PHANDLE_TABLE table = idle->ObjectTable;
    PEPROCESS_QUOTA_BLOCK quota = idle->QuotaBlock;
    CHECK(idle == PsGetCurrentProcess() && !PsInitialSystemProcess &&
          !PspInitialSystemProcessHandle && !token->TokenInUse &&
          !PspCidTable->HandleCount && !PsProcessType->TotalNumberOfObjects &&
          IsListEmpty(&PsActiveProcessHead));

    HANDLE owner = table->UniqueProcessId;
    ULONG handles = table->HandleCount;
    ULONG refs = quota->ReferenceCount;
    ULONG processes = quota->ProcessCount;
    SIZE_T paged = quota->QuotaEntry[PsPagedPool].Usage;
    SIZE_T nonpaged = quota->QuotaEntry[PsNonPagedPool].Usage;
    SIZE_T limit = quota->QuotaEntry[PsNonPagedPool].Limit;
    LONG_PTR token_refs = OBJECT_TO_OBJECT_HEADER(token)->PointerCount;
    LUID modified_before = token->ModifiedId;

    /* Fail after CID, list insertion and the paged portion of quota
     * charging. Both the process-owned and header-owned refs must unwind. */
    quota->QuotaEntry[PsNonPagedPool].Limit = nonpaged;
    NTSTATUS rejected = PsArm64InitializeSystemProcess();
    quota->QuotaEntry[PsNonPagedPool].Limit = limit;
    CHECK(rejected == STATUS_QUOTA_EXCEEDED && !PsInitialSystemProcess &&
          !PspInitialSystemProcessHandle && table->UniqueProcessId == owner &&
          table->HandleCount == handles && IsListEmpty(&PsActiveProcessHead) &&
          !PspCidTable->HandleCount && !PsProcessType->TotalNumberOfObjects &&
          !token->TokenInUse && OBJECT_TO_OBJECT_HEADER(token)->PointerCount == token_refs &&
          RtlEqualLuid(&token->ModifiedId, &modified_before) &&
          quota->ReferenceCount == refs && quota->ProcessCount == processes &&
          quota->QuotaEntry[PsPagedPool].Usage == paged &&
          quota->QuotaEntry[PsNonPagedPool].Usage == nonpaged);
    for (ULONG i = 0; i < SECURITY_DESCRIPTOR_CACHE_ENTRIES; ++i)
        CHECK(IsListEmpty(&ObsSecurityDescriptorCache[i].Head));

    LARGE_INTEGER before;
    KeQuerySystemTime(&before);
    CHECK(PsArm64InitializeSystemProcess() == STATUS_SUCCESS);
    PEPROCESS system = PsInitialSystemProcess;
    HANDLE handle = PspInitialSystemProcessHandle;
    CHECK(system && system != idle && (LONG_PTR)handle > 0);
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(system);
    CHECK(header->HandleCount == 1 && header->PointerCount == 2 &&
          header->QuotaBlockCharged == quota && header->SecurityDescriptor &&
          !(header->Flags & OB_FLAG_NEW_OBJECT) &&
          table->HandleCount == handles + 1 && PsProcessType->TotalNumberOfObjects == 1);
    CHECK(system->UniqueProcessId && !system->InheritedFromUniqueProcessId &&
          table->UniqueProcessId == system->UniqueProcessId &&
          system->ObjectTable == table && system->QuotaBlock == quota &&
          quota->ReferenceCount == refs + 2 && quota->ProcessCount == processes + 1);
    CHECK(system->GrantedAccess == PROCESS_ALL_ACCESS && system->ExitStatus == STATUS_PENDING &&
          !system->Peb && !system->SectionObject && !system->Session &&
          system->CreateTime.QuadPart >= before.QuadPart &&
          system->Pcb.BasePriority == NORMAL_BASE_PRIORITY &&
          system->Pcb.Affinity == KeActiveProcessors && system->Pcb.Affinity == 1 &&
          system->Pcb.ThreadQuantum == THREAD_QUANTUM &&
          system->Vm.Flags.MemoryPriority == MEMORY_PRIORITY_BACKGROUND &&
          system->Pcb.Header.Type == ProcessObject && system->Pcb.StackCount == MAXSHORT &&
          (system->Flags & PS_PROCESS_FLAGS_HAS_ADDRESS_SPACE) &&
          system->Pcb.DirectoryTableBase[0] == block->Tables.Root &&
          system->Pcb.DirectoryTableBase[1] == block->Tables.KernelRoot &&
          system->WorkingSetLock.Count == 1 && system->AddressCreationLock.Count == 1 &&
          system->Vm.VmWorkingSetList == idle->Vm.VmWorkingSetList);
    CHECK(ExFastRefGetObject(system->Token) == token && token->TokenInUse &&
          !token->SessionId && !RtlEqualLuid(&token->ModifiedId, &modified_before));
    CHECK(PsActiveProcessHead.Flink == &system->ActiveProcessLinks &&
          PsActiveProcessHead.Blink == &system->ActiveProcessLinks &&
          system->ActiveProcessLinks.Flink == &PsActiveProcessHead &&
          system->ActiveProcessLinks.Blink == &PsActiveProcessHead && PspCidTable->HandleCount == 1 &&
          IsListEmpty(&system->ThreadListHead) && IsListEmpty(&system->Pcb.ThreadListHead));
    POBJECT_NAME_INFORMATION audit_name = system->SeAuditProcessCreationInfo.ImageFileName;
    CHECK(!memcmp(idle->ImageFileName, "Idle", sizeof("Idle")) &&
          !memcmp(system->ImageFileName, "System", sizeof("System")) &&
          audit_name && !audit_name->Name.Length && !audit_name->Name.MaximumLength &&
          !audit_name->Name.Buffer);

    PVOID by_handle;
    PEPROCESS by_pid;
    OBJECT_HANDLE_INFORMATION info;
    CHECK(ObReferenceObjectByHandle(handle, PROCESS_ALL_ACCESS, PsProcessType, KernelMode,
                                    &by_handle, &info) == STATUS_SUCCESS &&
          by_handle == system && info.GrantedAccess == PROCESS_ALL_ACCESS);
    CHECK(PsLookupProcessByProcessId(system->UniqueProcessId, &by_pid) == STATUS_SUCCESS &&
          by_pid == system);
    ObDereferenceObject(by_pid);
    ObDereferenceObject(by_handle);
    CHECK(PsArm64InitializeSystemProcess() == STATUS_INVALID_DEVICE_STATE &&
          PsInitialSystemProcess == system && PspInitialSystemProcessHandle == handle &&
          header->PointerCount == 2 && PspCidTable->HandleCount == 1);

    XpTestReport("System process evidence pid="); XpTestHex((ULONG_PTR)system->UniqueProcessId);
    XpTestReport(" handle="); XpTestHex((ULONG_PTR)handle);
    XpTestReport(" owner="); XpTestHex((ULONG_PTR)table->UniqueProcessId);
    XpTestReport(" live_cids="); XpTestHex(PspCidTable->HandleCount);
    XpTestReport(" rollback="); XpTestHex((ULONG)rejected);
    XpTestReport(" quota_refs="); XpTestHex(quota->ReferenceCount - refs);
    XpTestReport(" root0="); XpTestHex(system->Pcb.DirectoryTableBase[0]);
    XpTestReport(" root1="); XpTestHex(system->Pcb.DirectoryTableBase[1]); XpTestReport("\n");
    XpTestReport("PASS: parentless XP System process published by CID and handle with shared boot resources\n");
}

typedef struct {
    KEVENT Started;
    KEVENT Continue;
    PXP_ARM64_BOOT_BLOCK Boot;
    PETHREAD Thread;
    ULONG Entries;
} SYSTEM_THREAD_CONTROL;

static VOID check_active_process(PEPROCESS current, PEPROCESS other, PXP_ARM64_BOOT_BLOCK boot)
{
    ULONG_PTR lower, upper;
    __asm__ volatile("mrs %0, ttbr0_el1\nmrs %1, ttbr1_el1" : "=r"(lower), "=r"(upper));
    CHECK(PsGetCurrentProcess() == current && KeGetCurrentIrql() == PASSIVE_LEVEL &&
          current->Pcb.ActiveProcessors == KeGetCurrentPrcb()->SetMember &&
          !other->Pcb.ActiveProcessors);
    CHECK((lower & 0x0000fffffffff000ULL) == boot->Tables.Root &&
          (upper & 0x0000fffffffff000ULL) == boot->Tables.KernelRoot);
}

static VOID system_thread_control(PVOID parameter)
{
    SYSTEM_THREAD_CONTROL *control = parameter;
    check_active_process(PsInitialSystemProcess, PsIdleProcess, control->Boot);
    CHECK(PsGetCurrentThread() == control->Thread &&
          PsGetCurrentThread()->ThreadsProcess == PsInitialSystemProcess &&
          PsInitialSystemProcess->ActiveThreads == 1);
    ULONG64 deadline = HalArm64ReadCounter() + control->Boot->Time.Frequency;
    while (!__atomic_load_n(&control->Thread->Tcb.KernelTime, __ATOMIC_RELAXED)) {
        CHECK(HalArm64ReadCounter() < deadline);
        __asm__ volatile("yield" ::: "memory");
    }
    ++control->Entries;
    KeSetEvent(&control->Started, 0, FALSE);
    CHECK(KeWaitForSingleObject(&control->Continue, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    check_active_process(PsInitialSystemProcess, PsIdleProcess, control->Boot);
    ++control->Entries;
}

VOID XpCheckBootSystemThread(PXP_ARM64_BOOT_BLOCK block)
{
    extern BOOLEAN PsReaperActive;
    extern PETHREAD PsReaperList;
    extern EX_WORK_QUEUE ExWorkerQueue[MaximumWorkQueue];
    PEPROCESS idle = PsIdleProcess, system = PsInitialSystemProcess;
    POBJECT_HEADER process_header = OBJECT_TO_OBJECT_HEADER(system);
    LONG_PTR process_refs = process_header->PointerCount;
    ULONG stacks = RtlNumberOfSetBits(&block->Tables.StackBitmap);
    PHANDLE_TABLE table = system->ObjectTable;
    ULONG handles = table->HandleCount;
    PEPROCESS_QUOTA_BLOCK quota = system->QuotaBlock;
    ULONG quota_refs = quota->ReferenceCount;
    SIZE_T paged = quota->QuotaEntry[PsPagedPool].Usage;
    SIZE_T nonpaged = quota->QuotaEntry[PsNonPagedPool].Usage;
    ULONG reaper_work = ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed;
    ULONG64 switches = KiArm64SchedulerSwitches;
    ULONG process_ticks = system->Pcb.KernelTime;
    SYSTEM_THREAD_CONTROL control = {.Boot = block};
    KeInitializeEvent(&control.Started, SynchronizationEvent, FALSE);
    KeInitializeEvent(&control.Continue, SynchronizationEvent, FALSE);
    KEVENT delay;
    KeInitializeEvent(&delay, SynchronizationEvent, FALSE);
    LARGE_INTEGER poll = {.QuadPart = -100000};

    check_active_process(idle, system, block);
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);
    CHECK(!system->ActiveThreads && system->Pcb.StackCount == MAXSHORT && !PsReaperActive);
    HANDLE handle;
    CLIENT_ID cid;
    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS created = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &attrs, NULL, &cid,
                                            system_thread_control, &control);
    if (!NT_SUCCESS(created)) {
        XpTestReport("PsCreateSystemThread status="); XpTestHex((ULONG)created); XpTestReport("\n");
    }
    CHECK(created == STATUS_SUCCESS);
    CHECK((LONG_PTR)handle < 0 && cid.UniqueProcess == system->UniqueProcessId &&
          cid.UniqueThread && cid.UniqueThread != cid.UniqueProcess);
    CHECK(ObReferenceObjectByHandle(handle, SYNCHRONIZE, PsThreadType, KernelMode,
                                    (PVOID *)&control.Thread, NULL) == STATUS_SUCCESS);
    PETHREAD thread = control.Thread;
    PETHREAD by_cid;
    CHECK(PsLookupProcessThreadByCid(&cid, NULL, &by_cid) == STATUS_SUCCESS && by_cid == thread);
    ObDereferenceObject(by_cid);
    CHECK(thread->Tcb.State == Ready && !thread->Tcb.ContextSwitches &&
          (thread->CrossThreadFlags & PS_CROSS_THREAD_FLAGS_SYSTEM) &&
          thread->Tcb.BasePriority == system->Pcb.BasePriority && thread->Tcb.Quantum == THREAD_QUANTUM &&
          thread->StartAddress == system_thread_control && PS_GET_THREAD_CREATE_TIME(thread) > 0 &&
          system->ActiveThreads == 1 && system->Pcb.StackCount == 1);
    CHECK(KeWaitForSingleObject(&control.Started, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    check_active_process(idle, system, block);
    CHECK(control.Entries == 1);
    CHECK(KeWaitForSingleObject(&delay, Executive, KernelMode, FALSE, &poll) == STATUS_TIMEOUT);
    CHECK(thread->Tcb.State == Waiting && thread->Tcb.WaitBlockList->Object == &control.Continue);
    KeSetEvent(&control.Continue, 0, FALSE);
    CHECK(KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    check_active_process(idle, system, block);
    CHECK(control.Entries == 2 && thread->Tcb.State == Terminated &&
          thread->ExitStatus == STATUS_SUCCESS && thread->Tcb.InitialStack &&
          (thread->CrossThreadFlags & PS_CROSS_THREAD_FLAGS_TERMINATED) &&
          !system->ActiveThreads && !system->Pcb.StackCount && PsReaperList == thread);
    ULONG entries = thread->Tcb.ContextSwitches;
    ULONG ticks = thread->Tcb.KernelTime;
    CHECK(entries >= 2 && ticks && system->Pcb.KernelTime - process_ticks == ticks);
    CHECK(KeWaitForSingleObject(&delay, Executive, KernelMode, FALSE, &poll) == STATUS_TIMEOUT);
    CHECK(!thread->Tcb.InitialStack && !PsReaperActive && !PsReaperList &&
          ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed == reaper_work + 1 &&
          OBJECT_TO_OBJECT_HEADER(thread)->PointerCount == 2 &&
          RtlNumberOfSetBits(&block->Tables.StackBitmap) == stacks);
    ObDereferenceObject(thread);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);
    CHECK(PsLookupThreadByThreadId(cid.UniqueThread, &by_cid) == STATUS_INVALID_PARAMETER &&
          PspCidTable->HandleCount == 1 && table->HandleCount == handles &&
          !PsThreadType->TotalNumberOfObjects && process_header->PointerCount == process_refs &&
          IsListEmpty(&system->ThreadListHead) && IsListEmpty(&system->Pcb.ThreadListHead) &&
          quota->ReferenceCount == quota_refs);

    /* Fail after KeStartThread, then let the real dead-thread path run. */
    SIZE_T limit = quota->QuotaEntry[PsNonPagedPool].Limit;
    quota->QuotaEntry[PsNonPagedPool].Limit = nonpaged;
    HANDLE rejected_handle;
    CLIENT_ID rejected_cid;
    NTSTATUS rejected = PsCreateSystemThread(&rejected_handle, THREAD_ALL_ACCESS, &attrs,
        PspInitialSystemProcessHandle, &rejected_cid, unscheduled_thread, NULL);
    quota->QuotaEntry[PsNonPagedPool].Limit = limit;
    CHECK(rejected == STATUS_QUOTA_EXCEEDED && !rejected_handle &&
          !rejected_cid.UniqueProcess && !rejected_cid.UniqueThread &&
          system->ActiveThreads == 1 && PspCidTable->HandleCount == 2);
    CHECK(KeWaitForSingleObject(&delay, Executive, KernelMode, FALSE, &poll) == STATUS_TIMEOUT);
    check_active_process(idle, system, block);
    CHECK(!PsReaperActive && !PsReaperList && !system->ActiveThreads && !system->Pcb.StackCount &&
          IsListEmpty(&system->ThreadListHead) && IsListEmpty(&system->Pcb.ThreadListHead) &&
          PspCidTable->HandleCount == 1 && table->HandleCount == handles &&
          !PsThreadType->TotalNumberOfObjects && process_header->PointerCount == process_refs &&
          quota->ReferenceCount == quota_refs && quota->QuotaEntry[PsPagedPool].Usage == paged &&
          quota->QuotaEntry[PsNonPagedPool].Usage == nonpaged &&
          RtlNumberOfSetBits(&block->Tables.StackBitmap) == stacks &&
          ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed == reaper_work + 2);

    KiArm64DisableTimerDispatch();
    XpTestReport("System thread evidence tid="); XpTestHex((ULONG_PTR)cid.UniqueThread);
    XpTestReport(" entries="); XpTestHex(entries);
    XpTestReport(" switches="); XpTestHex(KiArm64SchedulerSwitches - switches);
    XpTestReport(" ticks="); XpTestHex(ticks);
    XpTestReport(" reaps="); XpTestHex(ExWorkerQueue[HyperCriticalWorkQueue].WorkItemsProcessed - reaper_work);
    XpTestReport(" rollback="); XpTestHex((ULONG)rejected);
    XpTestReport(" live_cids="); XpTestHex(PspCidTable->HandleCount); XpTestReport("\n");
    XpTestReport("PASS: XP System thread creation, shared-TTBR dispatch, wait and executive reaping\n");
}

static VOID check_pagelk_refs(PFN_NUMBER first, ULONG pages, USHORT references)
{
    for (ULONG i = 0; i < pages; ++i) {
        const MMPFN *pfn = MI_PFN_ELEMENT(first + i);
        CHECK(pfn->u3.e1.PageLocation == ActiveAndValid && pfn->u3.e2.ReferenceCount == references);
    }
}

VOID XpCheckBootObPhase1Rollback(VOID);
VOID XpCheckBootObPhase1(VOID);

static VOID check_executive_atoms(VOID)
{
    extern ULONG RtlpAtomAllocateTag;
    CHECK(RtlpAtomAllocateTag == 'motA');
    PLIST_ENTRY tail = HandleTableListHead.Blink;
    PVOID table = NULL;
    CHECK(RtlCreateAtomTable(MAXULONG, &table) == STATUS_INVALID_PARAMETER && !table);
    CHECK(RtlCreateAtomTable(0, &table) == STATUS_SUCCESS && table &&
          tail == HandleTableListHead.Blink);
    RTL_ATOM first, duplicate, integer, lookup;
    CHECK(RtlAddAtomToAtomTable(table, L"\u00e4XP-Phase1", &first) == STATUS_SUCCESS &&
          first >= RTL_ATOM_MAXIMUM_INTEGER_ATOM);
    CHECK(RtlAddAtomToAtomTable(table, L"\u00c4xp-phase1", &duplicate) == STATUS_SUCCESS && duplicate == first);
    CHECK(RtlLookupAtomInAtomTable(table, L"\u00c4XP-PHASE1", &lookup) == STATUS_SUCCESS && lookup == first);
    CHECK(RtlAddAtomToAtomTable(table, L"#42", &integer) == STATUS_SUCCESS && integer == 42);
    PRTL_ATOM_TABLE atoms = table;
    CHECK(atoms->ExHandleTable->HandleCount == 1 && IsListEmpty(&atoms->ExHandleTable->HandleTableList));
    ULONG refs, flags;
    WCHAR name[24];
    ULONG bytes = sizeof(name);
    CHECK(RtlQueryAtomInAtomTable(table, first, &refs, &flags, name, &bytes) == STATUS_SUCCESS &&
          refs == 2 && !flags && bytes == sizeof(L"\u00e4XP-Phase1") - sizeof(WCHAR) &&
          !memcmp(name, L"\u00e4XP-Phase1", sizeof(L"\u00e4XP-Phase1")));
    CHECK(RtlDeleteAtomFromAtomTable(table, first) == STATUS_SUCCESS &&
          atoms->ExHandleTable->HandleCount == 1);
    CHECK(RtlDeleteAtomFromAtomTable(table, first) == STATUS_SUCCESS && !atoms->ExHandleTable->HandleCount);
    CHECK(RtlLookupAtomInAtomTable(table, L"\u00e4xp-phase1", &lookup) == STATUS_OBJECT_NAME_NOT_FOUND);
    bytes = sizeof(name);
    CHECK(RtlQueryAtomInAtomTable(table, integer, NULL, NULL, name, &bytes) == STATUS_SUCCESS &&
          bytes == 3 * sizeof(WCHAR) && !memcmp(name, L"#42", sizeof(L"#42")));
    CHECK(RtlDestroyAtomTable(table) == STATUS_SUCCESS && tail == HandleTableListHead.Blink);
    XpTestReport("Atom Phase 1 evidence string="); XpTestHex(first);
    XpTestReport(" integer="); XpTestHex(integer);
    XpTestReport(" duplicate_refs="); XpTestHex(refs);
    XpTestReport("\n");
}

typedef struct {
    WORK_QUEUE_ITEM Item;
    WORK_QUEUE_TYPE Queue;
    PULONG Processed;
    ULONG Ordinal;
    KEVENT Done;
} XP_WORK_CONTROL;

static VOID execute_work_control(PVOID parameter)
{
    XP_WORK_CONTROL *work = parameter;
    PKTHREAD thread = KeGetCurrentThread();
    CHECK(PsGetCurrentProcess() == PsInitialSystemProcess && PsGetCurrentThread()->ActiveExWorker &&
          thread->Queue == &ExWorkerQueue[work->Queue].WorkerQueue &&
          thread->KernelStackResident && KeGetCurrentIrql() == PASSIVE_LEVEL);
    CHECK(++*work->Processed == work->Ordinal);
    KeSetEvent(&work->Done, 0, FALSE);
}

static ULONG check_static_workers(PXP_ARM64_BOOT_BLOCK block)
{
    const ULONG critical_count = ExWorkerQueue[CriticalWorkQueue].Info.WorkerCount;
    const ULONG delayed_count = ExWorkerQueue[DelayedWorkQueue].Info.WorkerCount;
    CHECK(critical_count == 5 && delayed_count == 7 &&
          ExWorkerQueue[HyperCriticalWorkQueue].Info.WorkerCount == 1 &&
          PsInitialSystemProcess->ActiveThreads == critical_count + delayed_count + 2 &&
          RtlNumberOfSetBits(&block->Tables.StackBitmap) >= critical_count + delayed_count);
    ULONG queued[2] = {0}, processed[2] = {0};
    ULONG before[] = {ExWorkerQueue[CriticalWorkQueue].WorkItemsProcessed,
                      ExWorkerQueue[DelayedWorkQueue].WorkItemsProcessed};
    XP_WORK_CONTROL work[] = {
        {.Queue = CriticalWorkQueue}, {.Queue = DelayedWorkQueue},
        {.Queue = CriticalWorkQueue}, {.Queue = DelayedWorkQueue},
    };
    for (ULONG i = 0; i < sizeof(work) / sizeof(*work); ++i) {
        work[i].Processed = &processed[work[i].Queue];
        work[i].Ordinal = ++queued[work[i].Queue];
        KeInitializeEvent(&work[i].Done, SynchronizationEvent, FALSE);
        ExInitializeWorkItem(&work[i].Item, execute_work_control, &work[i]);
        ExQueueWorkItem(&work[i].Item, work[i].Queue);
    }
    for (ULONG i = 0; i < sizeof(work) / sizeof(*work); ++i) {
        LARGE_INTEGER timeout = {.QuadPart = -10000000};
        CHECK(KeWaitForSingleObject(&work[i].Done, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS &&
              !work[i].Item.List.Flink);
    }
    CHECK(processed[CriticalWorkQueue] == 2 && processed[DelayedWorkQueue] == 2 &&
          ExWorkerQueue[CriticalWorkQueue].WorkItemsProcessed == before[CriticalWorkQueue] + 2 &&
          ExWorkerQueue[DelayedWorkQueue].WorkItemsProcessed == before[DelayedWorkQueue] + 2);
    ULONG objects = PsThreadType->TotalNumberOfObjects;
    PVOID disposable;
    CHECK(ObCreateObject(KernelMode, PsThreadType, NULL, KernelMode, NULL, sizeof(ETHREAD),
                          0, 0, &disposable) == STATUS_SUCCESS);
    RtlZeroMemory(disposable, sizeof(ETHREAD));
    ObDereferenceObjectDeferDelete(disposable);
    CHECK(PsThreadType->TotalNumberOfObjects == objects + 1);
    XP_WORK_CONTROL drain = {.Queue = CriticalWorkQueue, .Processed = &processed[CriticalWorkQueue],
                             .Ordinal = processed[CriticalWorkQueue] + 1};
    KeInitializeEvent(&drain.Done, SynchronizationEvent, FALSE);
    ExInitializeWorkItem(&drain.Item, execute_work_control, &drain);
    ExQueueWorkItem(&drain.Item, CriticalWorkQueue);
    LARGE_INTEGER timeout = {.QuadPart = -10000000};
    CHECK(KeWaitForSingleObject(&drain.Done, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS &&
          PsThreadType->TotalNumberOfObjects == objects && !ObpRemoveObjectList &&
          ExWorkerQueue[CriticalWorkQueue].WorkItemsProcessed == before[CriticalWorkQueue] + 4);

    XpTestReport("Worker Phase 1 evidence critical="); XpTestHex(critical_count);
    XpTestReport(" delayed="); XpTestHex(delayed_count);
    XpTestReport(" work="); XpTestHex(processed[CriticalWorkQueue] + processed[DelayedWorkQueue]);
    XpTestReport(" slots="); XpTestHex(block->Tables.StackBitmap.SizeOfBitMap);
    XpTestReport(" deferred="); XpTestHex(ExWorkerQueue[CriticalWorkQueue].WorkItemsProcessed -
                                        before[CriticalWorkQueue] - processed[CriticalWorkQueue]);
    XpTestReport("\n");
    return critical_count + delayed_count;
}

static VOID signal_worker_event(PVOID parameter)
{
    KeSetEvent(parameter, 0, FALSE);
}

static VOID check_executive_event_wait(VOID)
{
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, L"\\ObjectTypes\\Event");
    PVOID type;
    CHECK(ExEventObjectType && ObArm64LookupBootObjectName(NULL, &path, OBJ_CASE_INSENSITIVE,
          ObpTypeObjectType, &type) == STATUS_SUCCESS && type == ExEventObjectType);
    ObDereferenceObject(type);

    HANDLE handles[2];
    PVOID events[2];
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    for (ULONG i = 0; i < 2; ++i) {
        CHECK(NtCreateEvent(&handles[i], EVENT_ALL_ACCESS, &attributes,
              SynchronizationEvent, FALSE) == STATUS_SUCCESS);
        CHECK(ObReferenceObjectByHandle(handles[i], SYNCHRONIZE | EVENT_MODIFY_STATE,
              ExEventObjectType, KernelMode, &events[i], NULL) == STATUS_SUCCESS);
    }

    LARGE_INTEGER zero = {0};
    KeSetEvent(events[1], 0, FALSE);
    CHECK(KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode, FALSE,
          &zero, NULL) == STATUS_WAIT_0 + 1 && !KeReadStateEvent(events[1]));
    KeSetEvent(events[1], 0, FALSE);
    WORK_QUEUE_ITEM signal;
    ExInitializeWorkItem(&signal, signal_worker_event, events[0]);
    ExQueueWorkItem(&signal, DelayedWorkQueue);
    LARGE_INTEGER limit = {.QuadPart = -10000000};
    CHECK(KeWaitForMultipleObjects(2, events, WaitAll, Executive, KernelMode, FALSE,
          &limit, NULL) == STATUS_WAIT_0 && !signal.List.Flink);
    for (ULONG i = 0; i < 2; ++i) {
        PKEVENT event = events[i];
        CHECK(!KeReadStateEvent(event) && IsListEmpty(&event->Header.WaitListHead));
        ObDereferenceObject(event);
        CHECK(ObArm64CloseBootObjectHandle(handles[i]) == STATUS_SUCCESS);
    }
    CHECK(ExEventObjectType->TotalNumberOfObjects == 1 && !ExEventObjectType->TotalNumberOfHandles);
}

typedef struct {
    WORK_QUEUE_ITEM Work;
    HANDLE Pair;
    PKEVENT Finished;
} EVENT_PAIR_WORK;

static VOID exchange_event_pair(PVOID context)
{
    EVENT_PAIR_WORK *work = context;
    CHECK(NtSetLowWaitHighEventPair(work->Pair) == STATUS_SUCCESS);
    KeSetEvent(work->Finished, 0, FALSE);
}

static VOID check_executive_dispatcher_types(VOID)
{
    extern POBJECT_TYPE ExEventPairObjectType, ExMutantObjectType;
    const struct {
        POBJECT_TYPE Type;
        PCWSTR Path;
    } types[] = {{ExEventPairObjectType, L"\\ObjectTypes\\EventPair"},
                 {ExMutantObjectType, L"\\ObjectTypes\\Mutant"}};
    for (ULONG i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        UNICODE_STRING path;
        PVOID found;
        RtlInitUnicodeString(&path, types[i].Path);
        CHECK(types[i].Type && ObArm64LookupBootObjectName(NULL, &path, OBJ_CASE_INSENSITIVE,
              ObpTypeObjectType, &found) == STATUS_SUCCESS && found == types[i].Type);
        ObDereferenceObject(found);
    }

    HANDLE pair, mutant;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    CHECK(NtCreateEventPair(&pair, EVENT_PAIR_ALL_ACCESS, &attributes) == STATUS_SUCCESS);
    CHECK(NtCreateMutant(&mutant, MUTANT_ALL_ACCESS, &attributes, TRUE) == STATUS_SUCCESS);
    CHECK(NtSetLowEventPair(mutant) == STATUS_OBJECT_TYPE_MISMATCH);
    PEEVENT_PAIR events;
    PKMUTANT lock;
    CHECK(ObReferenceObjectByHandle(pair, SYNCHRONIZE, ExEventPairObjectType, KernelMode,
          (PVOID *)&events, NULL) == STATUS_SUCCESS);
    CHECK(ObReferenceObjectByHandle(mutant, SYNCHRONIZE, ExMutantObjectType, KernelMode,
          (PVOID *)&lock, NULL) == STATUS_SUCCESS);

    KEVENT finished;
    KeInitializeEvent(&finished, SynchronizationEvent, FALSE);
    EVENT_PAIR_WORK exchange = {.Pair = pair, .Finished = &finished};
    ExInitializeWorkItem(&exchange.Work, exchange_event_pair, &exchange);
    ExQueueWorkItem(&exchange.Work, DelayedWorkQueue);
    CHECK(NtSetHighWaitLowEventPair(pair) == STATUS_SUCCESS);
    LARGE_INTEGER limit = {.QuadPart = -10000000};
    CHECK(KeWaitForSingleObject(&finished, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS);
    CHECK(!KeReadStateEvent(&events->KernelEventPair.EventHigh) &&
          !KeReadStateEvent(&events->KernelEventPair.EventLow) && !exchange.Work.List.Flink);
    ObDereferenceObject(events);
    CHECK(ObArm64CloseBootObjectHandle(pair) == STATUS_SUCCESS);

    PKTHREAD owner = KeGetCurrentThread();
    CHECK(lock->OwnerThread == owner && KeReadStateMutant(lock) == 0 &&
          owner->MutantListHead.Flink == &lock->MutantListEntry);
    LARGE_INTEGER zero = {0};
    CHECK(KeWaitForSingleObject(lock, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS &&
          KeReadStateMutant(lock) == -1);
    CHECK(KeReleaseMutant(lock, MUTANT_INCREMENT, FALSE, FALSE) == -1 &&
          KeReadStateMutant(lock) == 0);
    ObDereferenceObject(lock);
    CHECK(ObArm64CloseBootObjectHandle(mutant) == STATUS_SUCCESS &&
          IsListEmpty(&owner->MutantListHead));
    for (ULONG i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        CHECK(!types[i].Type->TotalNumberOfObjects && !types[i].Type->TotalNumberOfHandles);
    XpTestReport("PASS: XP Phase 1 EventPair exchange and Mutant ownership cleanup\n");
}

typedef struct {
    ULONG *Sequence;
    ULONG Position;
    KIRQL Irql;
} CALLBACK_ORDER;

static VOID record_callback_order(PVOID context, PVOID argument1, PVOID argument2)
{
    CALLBACK_ORDER *order = context;
    CHECK(argument1 == order->Sequence && argument2 == &record_callback_order &&
          KeGetCurrentIrql() == order->Irql && ++*order->Sequence == order->Position);
}

static VOID check_executive_callbacks(VOID)
{
    extern POBJECT_TYPE ExCallbackObjectType;
    const struct {
        PCWSTR Path;
        PCALLBACK_OBJECT Object;
    } callbacks[] = {{L"\\Callback\\SetSystemTime", ExCbSetSystemTime},
                     {L"\\Callback\\SetSystemState", ExCbSetSystemState},
                     {L"\\Callback\\PowerState", ExCbPowerState}};
    UNICODE_STRING name;
    PVOID found;
    RtlInitUnicodeString(&name, L"\\ObjectTypes\\Callback");
    CHECK(ExCallbackObjectType && ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE,
          ObpTypeObjectType, &found) == STATUS_SUCCESS && found == ExCallbackObjectType);
    ObDereferenceObject(found);
    RtlInitUnicodeString(&name, L"\\Callback");
    CHECK(ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE, ObpDirectoryObjectType,
          &found) == STATUS_SUCCESS);
    POBJECT_HEADER directory = OBJECT_TO_OBJECT_HEADER(found);
    EX_FAST_REF directory_security = {.Object = directory->SecurityDescriptor};
    PSECURITY_DESCRIPTOR descriptor = ExFastRefGetObject(directory_security);
    PACL actual, expected;
    BOOLEAN present, defaulted;
    CHECK((directory->Flags & OB_FLAG_PERMANENT_OBJECT) && descriptor &&
          RtlGetDaclSecurityDescriptor(SePublicDefaultSd, &present, &expected, &defaulted) == STATUS_SUCCESS &&
          present && expected &&
          RtlGetDaclSecurityDescriptor(descriptor, &present, &actual, &defaulted) == STATUS_SUCCESS &&
          present && actual && RtlValidAcl(actual) && actual->AceCount == expected->AceCount);
    for (ULONG i = 0; i < actual->AceCount; ++i) {
        PACCESS_ALLOWED_ACE ace, original;
        CHECK(RtlGetAce(actual, i, (PVOID *)&ace) == STATUS_SUCCESS &&
              RtlGetAce(expected, i, (PVOID *)&original) == STATUS_SUCCESS &&
              ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
              ace->Header.AceType == original->Header.AceType &&
              ace->Header.AceFlags == original->Header.AceFlags &&
              RtlEqualSid(&ace->SidStart, &original->SidStart));
        ACCESS_MASK mask = original->Mask;
        RtlMapGenericMask(&mask, &ObpDirectoryObjectType->TypeInfo.GenericMapping);
        CHECK(ace->Mask == mask);
    }
    ObDereferenceObject(found);

    OBJECT_ATTRIBUTES attributes;
    for (ULONG i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); ++i) {
        RtlInitUnicodeString(&name, callbacks[i].Path);
        InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
        PCALLBACK_OBJECT callback;
        CHECK(ExCreateCallback(&callback, &attributes, FALSE, FALSE) == STATUS_SUCCESS &&
              callback == callbacks[i].Object);
        POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(callback);
        CHECK((header->Flags & OB_FLAG_PERMANENT_OBJECT) && header->SecurityDescriptor &&
              !header->HandleCount && header->Type == ExCallbackObjectType);
        ObDereferenceObject(callback);
    }
    const ULONG persistent = ExCallbackObjectType->TotalNumberOfObjects;
    CHECK(persistent == sizeof(callbacks) / sizeof(callbacks[0]));

    ULONG sequence = 0;
    CALLBACK_ORDER first = {&sequence, 1, PASSIVE_LEVEL}, second = {&sequence, 2, PASSIVE_LEVEL};
    PVOID a = ExRegisterCallback(ExCbSetSystemTime, record_callback_order, &first);
    PVOID b = ExRegisterCallback(ExCbSetSystemTime, record_callback_order, &second);
    CHECK(a && b && a != b);
    ExNotifyCallback(ExCbSetSystemTime, &sequence, &record_callback_order);
    CHECK(sequence == 2);
    ExUnregisterCallback(a);
    second.Position = 3;
    second.Irql = DISPATCH_LEVEL;
    KIRQL previous;
    KeRaiseIrql(DISPATCH_LEVEL, &previous);
    ExNotifyCallback(ExCbSetSystemTime, &sequence, &record_callback_order);
    KeLowerIrql(previous);
    CHECK(sequence == 3);
    ExUnregisterCallback(b);
    ExNotifyCallback(ExCbSetSystemTime, &sequence, &record_callback_order);
    CHECK(sequence == 3);

    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    PCALLBACK_OBJECT exclusive;
    CHECK(ExCreateCallback(&exclusive, &attributes, TRUE, FALSE) == STATUS_SUCCESS);
    first.Position = 4;
    a = ExRegisterCallback(exclusive, record_callback_order, &first);
    CHECK(a && !ExRegisterCallback(exclusive, record_callback_order, &second));
    ExNotifyCallback(exclusive, &sequence, &record_callback_order);
    CHECK(sequence == 4);
    ExUnregisterCallback(a);
    ObDereferenceObject(exclusive);
    CHECK(ExCallbackObjectType->TotalNumberOfObjects == persistent &&
          !ExCallbackObjectType->TotalNumberOfHandles);
    XpTestReport("PASS: XP Phase 1 Callback namespace, FIFO notification and registration lifetime\n");
}

typedef struct {
    WORK_QUEUE_ITEM Work;
    PKSEMAPHORE Semaphore;
    KEVENT Done;
} SEMAPHORE_WAIT_WORK;

static VOID wait_for_executive_semaphore(PVOID context)
{
    SEMAPHORE_WAIT_WORK *work = context;
    LARGE_INTEGER timeout = {.QuadPart = -50000000};
    CHECK(KeWaitForSingleObject(work->Semaphore, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS);
    KeSetEvent(&work->Done, 0, FALSE);
}

static VOID check_executive_semaphore(VOID)
{
    extern POBJECT_TYPE ExSemaphoreObjectType;
    UNICODE_STRING name;
    PVOID found;
    RtlInitUnicodeString(&name, L"\\ObjectTypes\\Semaphore");
    CHECK(ExSemaphoreObjectType && ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE,
          ObpTypeObjectType, &found) == STATUS_SUCCESS && found == ExSemaphoreObjectType);
    ObDereferenceObject(found);

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE handle;
    CHECK(NtCreateSemaphore(&handle, SEMAPHORE_ALL_ACCESS, &attributes, 3, 2) == STATUS_INVALID_PARAMETER &&
          !handle);
    CHECK(NtCreateSemaphore(&handle, SEMAPHORE_ALL_ACCESS, &attributes, 1, 2) == STATUS_SUCCESS);
    PKSEMAPHORE semaphore;
    CHECK(ObReferenceObjectByHandle(handle, SYNCHRONIZE, ExSemaphoreObjectType, KernelMode,
          (PVOID *)&semaphore, NULL) == STATUS_SUCCESS);

    LONG previous = -1;
    CHECK(NtReleaseSemaphore(handle, 2, &previous) == STATUS_SEMAPHORE_LIMIT_EXCEEDED &&
          previous == -1 && semaphore->Header.SignalState == 1);
    LARGE_INTEGER zero = {0};
    CHECK(KeWaitForSingleObject(semaphore, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS &&
          semaphore->Header.SignalState == 0);

    SEMAPHORE_WAIT_WORK work = {.Semaphore = semaphore};
    KeInitializeEvent(&work.Done, NotificationEvent, FALSE);
    ExInitializeWorkItem(&work.Work, wait_for_executive_semaphore, &work);
    ExQueueWorkItem(&work.Work, DelayedWorkQueue);
    ULONGLONG deadline = KeQueryInterruptTime() + 10000000;
    LARGE_INTEGER step = {.QuadPart = -10000};
    while (IsListEmpty(&semaphore->Header.WaitListHead)) {
        CHECK(KeQueryInterruptTime() < deadline);
        CHECK(KeDelayExecutionThread(KernelMode, FALSE, &step) == STATUS_SUCCESS);
    }
    LARGE_INTEGER timeout = {.QuadPart = -10000000};
    CHECK(NtReleaseSemaphore(handle, 2, &previous) == STATUS_SUCCESS && !previous);
    CHECK(KeWaitForSingleObject(&work.Done, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS &&
          IsListEmpty(&semaphore->Header.WaitListHead) && semaphore->Header.SignalState == 1);
    ObDereferenceObject(semaphore);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS &&
          !ExSemaphoreObjectType->TotalNumberOfObjects && !ExSemaphoreObjectType->TotalNumberOfHandles);
    XpTestReport("PASS: XP Phase 1 Semaphore handle, limit exception and blocked waiter release\n");
}

typedef struct {
    HANDLE OneShotHandle;
    HANDLE PeriodicHandle;
    PKTIMER Periodic;
    PETHREAD Owner;
    KEVENT ApcArrived;
    KEVENT ReadyToExit;
    KEVENT ExitPermit;
    LARGE_INTEGER StartTime;
    ULONGLONG LastCallbackTime;
    ULONG Callbacks;
} TIMER_APC_CONTROL;

static VOID NTAPI record_timer_apc(PVOID context, ULONG low, LONG high)
{
    TIMER_APC_CONTROL *control = context;
    ULONGLONG time = ((ULONGLONG)(ULONG)high << 32) | low;
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && PsGetCurrentThread() == control->Owner &&
          time >= (ULONGLONG)control->StartTime.QuadPart && time > control->LastCallbackTime);
    control->LastCallbackTime = time;
    __atomic_add_fetch(&control->Callbacks, 1, __ATOMIC_RELAXED);
    KeSetEvent(&control->ApcArrived, 0, FALSE);
}

static VOID run_executive_timers(PVOID context)
{
    TIMER_APC_CONTROL *control = context;
    PETHREAD owner = PsGetCurrentThread();
    control->Owner = owner;
    CHECK(owner->ThreadsProcess == PsInitialSystemProcess && IsListEmpty(&owner->ActiveTimerListHead));
    KeQuerySystemTime(&control->StartTime);
    LARGE_INTEGER due = {.QuadPart = -50000}, limit = {.QuadPart = -10000000};
    HAL_ARM64_IRQ_STATS before, after;
    HalArm64GetIrqStats(&before);
    BOOLEAN previous = TRUE;
    CHECK(NtSetTimer(control->OneShotHandle, &due, record_timer_apc, control, FALSE, 0, &previous)
          == STATUS_SUCCESS && !previous);
    /* A running owner requires an APC SGI. A blocked owner can consume the
     * pending APC as part of its ordinary wait restart. */
    ULONG64 deadline = HalArm64ReadCounter() + before.Frequency;
    while (!__atomic_load_n(&control->Callbacks, __ATOMIC_RELAXED)) {
        CHECK(HalArm64ReadCounter() < deadline);
        __asm__ volatile("yield" ::: "memory");
    }
    HalArm64GetIrqStats(&after);
    CHECK(control->Callbacks == 1 && IsListEmpty(&owner->ActiveTimerListHead) &&
          after.ApcInterrupts > before.ApcInterrupts && KeReadStateEvent(&control->ApcArrived));
    KeClearEvent(&control->ApcArrived);
    TIMER_BASIC_INFORMATION info;
    ULONG bytes;
    CHECK(NtQueryTimer(control->OneShotHandle, TimerBasicInformation, &info, sizeof(info), &bytes) ==
          STATUS_SUCCESS && bytes == sizeof(info) && info.TimerState && info.RemainingTime.QuadPart <= 0);

    CHECK(NtSetTimer(control->PeriodicHandle, &due, record_timer_apc, control, FALSE, 25, &previous) ==
          STATUS_SUCCESS && !previous);
    for (ULONG i = 0; i < 2; ++i)
        CHECK(KeWaitForSingleObject(&control->ApcArrived, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS);
    CHECK(control->Callbacks >= 3 && !IsListEmpty(&owner->ActiveTimerListHead) &&
          control->Periodic->Header.Inserted && control->Periodic->Period == 25);
    BOOLEAN signaled = FALSE;
    CHECK(NtCancelTimer(control->PeriodicHandle, &signaled) == STATUS_SUCCESS && signaled &&
          !control->Periodic->Header.Inserted && IsListEmpty(&owner->ActiveTimerListHead));

    /* Keep a distant timer queued so only the exit path drops its APC reference. */
    due.QuadPart = -50000000;
    CHECK(NtSetTimer(control->PeriodicHandle, &due, record_timer_apc, control, FALSE, 25, &previous) ==
          STATUS_SUCCESS && previous);
    KeSetEvent(&control->ReadyToExit, 0, FALSE);
    CHECK(KeWaitForSingleObject(&control->ExitPermit, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS);
}

static VOID check_executive_timers(VOID)
{
    extern POBJECT_TYPE ExTimerObjectType;
    UNICODE_STRING name;
    PVOID found;
    RtlInitUnicodeString(&name, L"\\ObjectTypes\\Timer");
    CHECK(ExTimerObjectType && ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE,
          ObpTypeObjectType, &found) == STATUS_SUCCESS && found == ExTimerObjectType);
    ObDereferenceObject(found);

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    TIMER_APC_CONTROL control = {0};
    CHECK(NtCreateTimer(&control.OneShotHandle, TIMER_ALL_ACCESS, &attributes, (TIMER_TYPE)2) ==
          STATUS_INVALID_PARAMETER_4 && !control.OneShotHandle);
    CHECK(NtCreateTimer(&control.OneShotHandle, TIMER_ALL_ACCESS, &attributes, NotificationTimer) == STATUS_SUCCESS);
    CHECK(NtCreateTimer(&control.PeriodicHandle, TIMER_ALL_ACCESS, &attributes, NotificationTimer) == STATUS_SUCCESS);
    CHECK(ObReferenceObjectByHandle(control.PeriodicHandle, SYNCHRONIZE, ExTimerObjectType, KernelMode,
          (PVOID *)&control.Periodic, NULL) == STATUS_SUCCESS);
    KeInitializeEvent(&control.ApcArrived, SynchronizationEvent, FALSE);
    KeInitializeEvent(&control.ReadyToExit, NotificationEvent, FALSE);
    KeInitializeEvent(&control.ExitPermit, NotificationEvent, FALSE);

    HAL_ARM64_IRQ_STATS before, after;
    HalArm64GetIrqStats(&before);
    HANDLE handle;
    CHECK(PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &attributes, NULL, NULL,
          run_executive_timers, &control) == STATUS_SUCCESS);
    PETHREAD owner;
    CHECK(ObReferenceObjectByHandle(handle, SYNCHRONIZE, PsThreadType, KernelMode,
          (PVOID *)&owner, NULL) == STATUS_SUCCESS);
    LARGE_INTEGER limit = {.QuadPart = -10000000};
    CHECK(KeWaitForSingleObject(&control.ReadyToExit, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS);
    CHECK(control.Owner == owner && !IsListEmpty(&owner->ActiveTimerListHead) &&
          control.Periodic->Header.Inserted);
    ULONG callbacks = control.Callbacks;
    POBJECT_HEADER timer = OBJECT_TO_OBJECT_HEADER(control.Periodic);
    CHECK(timer->PointerCount == 3 && ObArm64CloseBootObjectHandle(control.PeriodicHandle) == STATUS_SUCCESS &&
          timer->HandleCount == 0 && timer->PointerCount == 2);
    KeSetEvent(&control.ExitPermit, 0, FALSE);
    CHECK(KeWaitForSingleObject(&owner->Tcb, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS &&
          owner->ExitStatus == STATUS_SUCCESS && owner->Tcb.State == Terminated &&
          IsListEmpty(&owner->ActiveTimerListHead) && IsListEmpty(&owner->Tcb.ApcState.ApcListHead[KernelMode]) &&
          !control.Periodic->Header.Inserted && timer->PointerCount == 1 && control.Callbacks == callbacks);
    ObDereferenceObject(control.Periodic);

    /* The original informational status still arms and delivers the running clock timer. */
    PKTIMER one_shot;
    CHECK(ObReferenceObjectByHandle(control.OneShotHandle, SYNCHRONIZE, ExTimerObjectType, KernelMode,
          (PVOID *)&one_shot, NULL) == STATUS_SUCCESS);
    LARGE_INTEGER due = {.QuadPart = -50000};
    BOOLEAN previous = FALSE;
    CHECK(NtSetTimer(control.OneShotHandle, &due, NULL, NULL, TRUE, 0, &previous) ==
          STATUS_TIMER_RESUME_IGNORED && previous);
    CHECK(KeWaitForSingleObject(one_shot, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS);
    CHECK(NtCancelTimer(control.OneShotHandle, &previous) == STATUS_SUCCESS && previous);
    ObDereferenceObject(one_shot);
    CHECK(ObArm64CloseBootObjectHandle(control.OneShotHandle) == STATUS_SUCCESS &&
          !ExTimerObjectType->TotalNumberOfObjects && !ExTimerObjectType->TotalNumberOfHandles);
    CHECK(!owner->Tcb.InitialStack);
    ObDereferenceObject(owner);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);
    HalArm64GetIrqStats(&after);
    CHECK(!after.LastFailure && after.Interrupts > before.Interrupts);
    XpTestReport("Executive Timer evidence callbacks="); XpTestHex(callbacks);
    XpTestReport(" clock_irqs="); XpTestHex(after.Interrupts - before.Interrupts);
    XpTestReport(" apc_sgis="); XpTestHex(after.ApcInterrupts - before.ApcInterrupts);
    XpTestReport("\n");
    XpTestReport("PASS: XP Phase 1 Timer handle, clock DPC/APC and thread exit rundown\n");
}

extern VOID XpSpinProfile(ULONG64 Deadline);
extern UCHAR XpProfileSampleBegin[], XpProfileSampleEnd[];

static VOID spin_profile_on_system(PVOID context)
{
    CHECK(PsGetCurrentProcess() == PsInitialSystemProcess);
    HAL_ARM64_IRQ_STATS irq;
    HalArm64GetIrqStats(&irq);
    XpSpinProfile(HalArm64ReadCounter() + irq.Frequency / 50);
    KeSetEvent(context, 0, FALSE);
}

static VOID run_system_profile_sample(VOID)
{
    KEVENT finished;
    KeInitializeEvent(&finished, SynchronizationEvent, FALSE);
    WORK_QUEUE_ITEM work;
    ExInitializeWorkItem(&work, spin_profile_on_system, &finished);
    ExQueueWorkItem(&work, DelayedWorkQueue);
    LARGE_INTEGER limit = {.QuadPart = -10000000};
    CHECK(KeWaitForSingleObject(&finished, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS);
}

static ULONG profile_hits(volatile ULONG *buffer, ULONG buckets)
{
    ULONG hits = 0;
    for (ULONG i = 0; i < buckets; ++i) hits += buffer[i];
    CHECK(buffer[buckets] == 0xfeedcafe);
    return hits;
}

static VOID check_executive_profile(VOID)
{
    extern POBJECT_TYPE ExProfileObjectType;
    extern LIST_ENTRY KiProfileListHead, KiProfileSourceListHead;
    PEPROCESS system = PsInitialSystemProcess;
    CHECK(PsGetCurrentProcess() == PsIdleProcess && IsListEmpty(&KiProfileListHead) &&
          IsListEmpty(&KiProfileSourceListHead) && IsListEmpty(&system->Pcb.ProfileListHead));
    UNICODE_STRING path;
    PVOID found;
    RtlInitUnicodeString(&path, L"\\ObjectTypes\\Profile");
    CHECK(ExProfileObjectType && ObArm64LookupBootObjectName(NULL, &path, OBJ_CASE_INSENSITIVE,
          ObpTypeObjectType, &found) == STATUS_SUCCESS && found == ExProfileObjectType);
    ObDereferenceObject(found);

    const SIZE_T range = (ULONG_PTR)XpProfileSampleEnd - (ULONG_PTR)XpProfileSampleBegin;
    const ULONG buckets = (ULONG)(range / sizeof(ULONG));
    CHECK(range == 4 * sizeof(ULONG));
    PULONG global = MiAllocatePoolPages(PagedPool, PAGE_SIZE, 0);
    PULONG local = MiAllocatePoolPages(NonPagedPool, PAGE_SIZE, 0);
    CHECK(global && local);
    global[buckets] = local[buckets] = 0xfeedcafe;
    const PFN_NUMBER locked = MmSystemLockPagesCount;
    const ULONG free_ptes = MiCountFreeSystemPtes(SystemPteSpace);
    LONG_PTR system_refs = OBJECT_TO_OBJECT_HEADER(system)->PointerCount;

    HANDLE bad;
    CHECK(NtCreateProfile(&bad, NULL, XpProfileSampleBegin, range + 1, 4,
          global, sizeof(ULONG), ProfileTime, 0) == STATUS_BUFFER_TOO_SMALL && !bad);
    CHECK(NtCreateProfile(&bad, NULL, XpProfileSampleBegin, range, 2,
          global, (ULONG)range, ProfileAlignmentFixup, 0) == STATUS_NOT_SUPPORTED && !bad);
    CHECK(NtCreateProfile(&bad, NULL, XpProfileSampleBegin, range, 2, (PULONG)XpProfileSampleBegin,
          (ULONG)range, ProfileTime, 0) == STATUS_SUCCESS);
    CHECK(NtStartProfile(bad) == STATUS_ACCESS_VIOLATION &&
          ObArm64CloseBootObjectHandle(bad) == STATUS_SUCCESS &&
          !ExProfileObjectType->TotalNumberOfObjects && MmSystemLockPagesCount == locked);

    HANDLE process, all, restricted;
    CHECK(ObArm64CreateBootHandle(system, PROCESS_QUERY_INFORMATION, TRUE, &process) == STATUS_SUCCESS);
    CHECK(NtCreateProfile(&all, NULL, XpProfileSampleBegin, range, 2,
          global, (ULONG)range, ProfileTime, 0) == STATUS_SUCCESS);
    CHECK(NtCreateProfile(&restricted, process, XpProfileSampleBegin, range, 2,
          local, (ULONG)range, ProfileTime, KeGetCurrentPrcb()->SetMember) == STATUS_SUCCESS);
    CHECK(ObArm64CloseBootObjectHandle(process) == STATUS_SUCCESS);
    CHECK(NtStopProfile(restricted) == STATUS_PROFILING_NOT_STARTED);
    CHECK(NtStartProfile(all) == STATUS_SUCCESS && NtStartProfile(all) == STATUS_PROFILING_NOT_STOPPED);
    CHECK(NtStartProfile(restricted) == STATUS_SUCCESS && MmSystemLockPagesCount == locked + 2 &&
          MiCountFreeSystemPtes(SystemPteSpace) == free_ptes - 2);
    HAL_ARM64_IRQ_STATS before, after;
    HalArm64GetIrqStats(&before);
    XpSpinProfile(HalArm64ReadCounter() + before.Frequency / 50);
    ULONG idle_hits = profile_hits(global, buckets);
    CHECK(idle_hits && !profile_hits(local, buckets));
    run_system_profile_sample();
    ULONG system_hits = profile_hits(local, buckets);
    ULONG total = profile_hits(global, buckets);
    CHECK(system_hits && total == idle_hits + system_hits);
    CHECK(NtStopProfile(all) == STATUS_SUCCESS && NtStopProfile(all) == STATUS_PROFILING_NOT_STARTED &&
          IsListEmpty(&KiProfileListHead) && !IsListEmpty(&KiProfileSourceListHead) &&
          MmSystemLockPagesCount == locked + 1 &&
          ObArm64CloseBootObjectHandle(all) == STATUS_SUCCESS);
    run_system_profile_sample();
    ULONG remaining_hits = profile_hits(local, buckets) - system_hits;
    CHECK(remaining_hits && profile_hits(global, buckets) == total);
    CHECK(ObArm64CloseBootObjectHandle(restricted) == STATUS_SUCCESS &&
          IsListEmpty(&KiProfileSourceListHead) && IsListEmpty(&system->Pcb.ProfileListHead) &&
          OBJECT_TO_OBJECT_HEADER(system)->PointerCount == system_refs &&
          MmSystemLockPagesCount == locked && MiCountFreeSystemPtes(SystemPteSpace) == free_ptes &&
          !ExProfileObjectType->TotalNumberOfObjects && !ExProfileObjectType->TotalNumberOfHandles);
    run_system_profile_sample();
    CHECK(profile_hits(local, buckets) == system_hits + remaining_hits && profile_hits(global, buckets) == total);
    HalArm64GetIrqStats(&after);
    CHECK(after.Interrupts - before.Interrupts >= total + remaining_hits && !after.LastFailure);
    CHECK(MiFreePoolPages(global) == 1 && MiFreePoolPages(local) == 1);

    XpTestReport("Profile evidence idle="); XpTestHex(idle_hits);
    XpTestReport(" system="); XpTestHex(system_hits);
    XpTestReport(" after_stop="); XpTestHex(remaining_hits);
    XpTestReport(" clock_irqs="); XpTestHex(after.Interrupts - before.Interrupts);
    XpTestReport("\n");
    XpTestReport("PASS: XP Phase 1 ProfileTime IRQ buckets, process filtering and active close cleanup\n");
}

static ULONGLONG check_local_uuid(const UUID *uuid, ULONG sequence, const CHAR *seed)
{
    CHECK((uuid->Data3 >> 12) == 1 && (uuid->Data4[0] & 0xc0) == 0x80 &&
          (((ULONG)(uuid->Data4[0] & 0x3f) << 8) | uuid->Data4[1]) == (sequence & 0x3fff) &&
          !memcmp(&uuid->Data4[2], seed, 6));
    return ((ULONGLONG)(uuid->Data3 & 0x0fff) << 48) | ((ULONGLONG)uuid->Data2 << 32) | uuid->Data1;
}

static VOID check_executive_uuids(VOID)
{
    extern LARGE_INTEGER ExpUuidLastTimeAllocated;
    extern BOOLEAN ExpUuidSequenceNumberValid, ExpUuidSequenceNumberNotSaved;
    CHECK(ExpUuidLastTimeAllocated.QuadPart > 0 && !ExpUuidSequenceNumberValid);
    ULONGLONG daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
    ULONG allocations = 0, sequence = 0;
    ULONGLONG end = 0;
    CHAR seed[6];
    for (;;) {
        ULARGE_INTEGER time;
        ULONG range, candidate;
        NTSTATUS status = NtAllocateUuids(&time, &range, &candidate, seed);
        if (status == STATUS_RETRY) break;
        CHECK(status == STATUS_SUCCESS && range && range <= 10000 &&
              time.QuadPart >= end && (!allocations || sequence == candidate) && allocations <= 1000);
        sequence = candidate;
        end = time.QuadPart + range;
        ++allocations;
    }
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    CHECK(allocations && end == (ULONGLONG)now.QuadPart &&
          ExpUuidSequenceNumberValid && ExpUuidSequenceNumberNotSaved && ((UCHAR)seed[0] & 0x80));
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");

    LARGE_INTEGER tick = {.QuadPart = -10000};
    CHECK(KeDelayExecutionThread(KernelMode, FALSE, &tick) == STATUS_SUCCESS);
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
    KeQuerySystemTime(&now);
    CHECK((ULONGLONG)now.QuadPart > end &&
          (ULONGLONG)now.QuadPart - end <= 10000000);
    while ((ULONGLONG)now.QuadPart - end > 10000) {
        ULARGE_INTEGER time;
        ULONG range, next;
        CHECK(NtAllocateUuids(&time, &range, &next, seed) == STATUS_SUCCESS &&
              range == 10000 && next == sequence && time.QuadPart == end);
        end += range;
    }
    const ULONG cached = (ULONG)((ULONGLONG)now.QuadPart - end);
    const ULONGLONG uuid_epoch_offset = 5748192000000000ULL;
    UUID uuid;
    for (ULONG i = 0; i < cached; ++i) {
        CHECK(ExUuidCreate(&uuid) == RPC_NT_UUID_LOCAL_ONLY);
        CHECK(check_local_uuid(&uuid, sequence, seed) == end + uuid_epoch_offset + i);
    }
    CHECK(ExUuidCreate(&uuid) == STATUS_RETRY);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");

    CHECK(KeDelayExecutionThread(KernelMode, FALSE, &tick) == STATUS_SUCCESS);
    CHECK(ExUuidCreate(&uuid) == RPC_NT_UUID_LOCAL_ONLY &&
          check_local_uuid(&uuid, sequence, seed) >= end + uuid_epoch_offset + cached);
    XpTestReport("UUID evidence allocations="); XpTestHex(allocations);
    XpTestReport(" cached="); XpTestHex(cached); XpTestReport("\n");
    XpTestReport("PASS: XP Phase 1 UUID ranges, local cache exhaustion and real clock refill\n");
}

typedef struct {
    WORK_QUEUE_ITEM Work;
    HANDLE Handle;
    PVOID Key;
    PETHREAD Thread;
    BOOLEAN Release;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    KEVENT Done;
} KEYED_EVENT_WORK;

static VOID run_keyed_operation(PVOID context)
{
    KEYED_EVENT_WORK *work = context;
    PETHREAD thread = PsGetCurrentThread();
    CHECK(thread->ThreadsProcess == PsInitialSystemProcess && IsListEmpty(&thread->KeyedWaitChain));
    PVOID old_key = thread->KeyedWaitValue;
    work->Thread = thread;
    work->Status = work->Release
        ? NtReleaseKeyedEvent(work->Handle, work->Key, FALSE, &work->Timeout)
        : NtWaitForKeyedEvent(work->Handle, work->Key, FALSE, &work->Timeout);
    CHECK(IsListEmpty(&thread->KeyedWaitChain) && thread->KeyedWaitValue == old_key &&
          !thread->KeyedWaitSemaphore.Header.SignalState);
    KeSetEvent(&work->Done, 0, FALSE);
}

static VOID queue_keyed_operation(KEYED_EVENT_WORK *work, HANDLE handle, ULONG_PTR key,
                                  BOOLEAN release, LONGLONG timeout)
{
    work->Handle = handle;
    work->Key = (PVOID)key;
    work->Release = release;
    work->Thread = NULL;
    work->Timeout.QuadPart = timeout;
    KeInitializeEvent(&work->Done, SynchronizationEvent, FALSE);
    ExInitializeWorkItem(&work->Work, run_keyed_operation, work);
    ExQueueWorkItem(&work->Work, DelayedWorkQueue);
}

static VOID check_executive_keyed_events(VOID)
{
    extern POBJECT_TYPE ExpKeyedEventObjectType;
    UNICODE_STRING name;
    PVOID found;
    RtlInitUnicodeString(&name, L"\\ObjectTypes\\KeyedEvent");
    CHECK(ExpKeyedEventObjectType && ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE,
          ObpTypeObjectType, &found) == STATUS_SUCCESS && found == ExpKeyedEventObjectType);
    ObDereferenceObject(found);

    RtlInitUnicodeString(&name, L"\\KernelObjects\\CritSecOutOfMemoryEvent");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    HANDLE global;
    CHECK(NtOpenKeyedEvent(&global, KEYEDEVENT_ALL_ACCESS, &attributes) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(global, READ_CONTROL, ExpKeyedEventObjectType, KernelMode,
                                   &found, NULL) == STATUS_SUCCESS);
    POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(found);
    EX_FAST_REF security = {.Object = header->SecurityDescriptor};
    PACL dacl;
    BOOLEAN present, defaulted;
    CHECK((header->Flags & OB_FLAG_PERMANENT_OBJECT) && ExFastRefGetObject(security) &&
          RtlGetDaclSecurityDescriptor(ExFastRefGetObject(security), &present, &dacl, &defaulted) ==
              STATUS_SUCCESS && present && !defaulted && dacl && RtlValidAcl(dacl) && dacl->AceCount == 3);
    PSID principals[3] = {SeAliasAdminsSid, SeLocalSystemSid, SeWorldSid};
    for (ULONG i = 0; i < 3; ++i) {
        PACCESS_ALLOWED_ACE ace;
        CHECK(RtlGetAce(dacl, i, (PVOID *)&ace) == STATUS_SUCCESS &&
              ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
              RtlEqualSid(&ace->SidStart, principals[i]) &&
              ace->Mask == (i == 2 ? KEYEDEVENT_WAIT | KEYEDEVENT_WAKE | READ_CONTROL : KEYEDEVENT_ALL_ACCESS));
    }
    ObDereferenceObject(found);

    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE private;
    CHECK(NtCreateKeyedEvent(&private, KEYEDEVENT_ALL_ACCESS, &attributes, 1) ==
          STATUS_INVALID_PARAMETER_4 && !private);
    CHECK(NtCreateKeyedEvent(&private, KEYEDEVENT_ALL_ACCESS, &attributes, 0) == STATUS_SUCCESS);
    CHECK(NtWaitForKeyedEvent(private, (PVOID)1, FALSE, NULL) == STATUS_INVALID_PARAMETER_1);

    LARGE_INTEGER limit = {.QuadPart = -10000000}, tick = {.QuadPart = -10000};
    for (ULONG i = 0; i < 2; ++i) {
        HANDLE use = i ? private : global;
        KEYED_EVENT_WORK first, peer;
        queue_keyed_operation(&first, use, 0x20, (BOOLEAN)i, limit.QuadPart);
        ULONGLONG deadline = KeQueryInterruptTime() + 10000000;
        while (!first.Thread || first.Thread->Tcb.State != Waiting ||
               first.Thread->Tcb.WaitBlockList->Object != &first.Thread->KeyedWaitSemaphore) {
            CHECK(KeQueryInterruptTime() < deadline);
            CHECK(KeDelayExecutionThread(KernelMode, FALSE, &tick) == STATUS_SUCCESS);
        }
        CHECK(!IsListEmpty(&first.Thread->KeyedWaitChain) &&
              first.Thread->KeyedWaitValue == (PVOID)(ULONG_PTR)(0x20 | i));
        queue_keyed_operation(&peer, use, 0x40, (BOOLEAN)!i, tick.QuadPart);
        CHECK(KeWaitForSingleObject(&peer.Done, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS &&
              peer.Status == STATUS_TIMEOUT && !KeReadStateEvent(&first.Done));
        queue_keyed_operation(&peer, use, 0x20, (BOOLEAN)!i, limit.QuadPart);
        CHECK(KeWaitForSingleObject(&first.Done, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS &&
              KeWaitForSingleObject(&peer.Done, Executive, KernelMode, FALSE, &limit) == STATUS_SUCCESS &&
              first.Status == STATUS_SUCCESS && peer.Status == STATUS_SUCCESS && first.Thread != peer.Thread);
    }
    CHECK(ObArm64CloseBootObjectHandle(private) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(global) == STATUS_SUCCESS &&
          ExpKeyedEventObjectType->TotalNumberOfObjects == 1 && !ExpKeyedEventObjectType->TotalNumberOfHandles);
    XpTestReport("PASS: XP Phase 1 KeyedEvent default ACL, bidirectional rendezvous and timeout cleanup\n");
}

static VOID check_win32_object_types(VOID)
{
    extern POBJECT_TYPE ExWindowStationObjectType, ExDesktopObjectType;
    CHECK(!PsGetCurrentProcess()->Session && ExWindowStationObjectType && ExDesktopObjectType &&
          ExWindowStationObjectType != ExDesktopObjectType);
    const struct {
        POBJECT_TYPE Type;
        PCWSTR Path;
        BOOLEAN Parses;
    } types[] = {{ExWindowStationObjectType, L"\\ObjectTypes\\WindowStation", TRUE},
                 {ExDesktopObjectType, L"\\ObjectTypes\\Desktop", FALSE}};
    for (ULONG i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        UNICODE_STRING name;
        PVOID found;
        RtlInitUnicodeString(&name, types[i].Path);
        POBJECT_TYPE type = types[i].Type;
        CHECK(ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE, ObpTypeObjectType,
              &found) == STATUS_SUCCESS && found == type);
        ObDereferenceObject(found);
        POBJECT_TYPE_INITIALIZER init = &type->TypeInfo;
        CHECK(init->SecurityRequired && init->PoolType == NonPagedPool &&
              init->ValidAccessMask == STANDARD_RIGHTS_REQUIRED &&
              init->InvalidAttributes == (OBJ_OPENLINK | OBJ_PERMANENT | OBJ_EXCLUSIVE) &&
              init->GenericMapping.GenericAll == STANDARD_RIGHTS_REQUIRED && init->OpenProcedure &&
              init->CloseProcedure && init->OkayToCloseProcedure && init->DeleteProcedure &&
              (init->ParseProcedure != NULL) == types[i].Parses);

        /* The first field is Win32k's session ID. This unpublished body
         * checks real OB dispatch failure while the provider is absent. */
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        PULONG body;
        CHECK(ObCreateObject(KernelMode, type, &attributes, KernelMode, NULL, sizeof(*body), 0, 0,
                             (PVOID *)&body) == STATUS_SUCCESS);
        *body = 0;
        CHECK(init->OpenProcedure(ObOpenHandle, PsGetCurrentProcess(), body, READ_CONTROL, 0) ==
              STATUS_INVALID_PARAMETER &&
              !init->OkayToCloseProcedure(PsGetCurrentProcess(), body, NULL, KernelMode));
        if (init->ParseProcedure) {
            UNICODE_STRING remaining;
            RtlInitUnicodeString(&remaining, L"\\Default");
            found = body;
            CHECK(init->ParseProcedure(body, ExDesktopObjectType, NULL, KernelMode, 0, &name,
                                       &remaining, NULL, NULL, &found) == STATUS_INVALID_PARAMETER && !found);
        }
        HANDLE handle;
        CHECK(ObInsertObject(body, NULL, READ_CONTROL, 0, NULL, &handle) == STATUS_INVALID_PARAMETER &&
              !handle && !type->TotalNumberOfObjects && !type->TotalNumberOfHandles);
    }
    XpTestReport("PASS: XP Phase 1 WindowStation and Desktop types with pre-session dispatch guards\n");
}

static VOID check_console_desktops(VOID)
{
    NTSTATUS init_status = ExpArm64InitializeWin32Console();
    if (!NT_SUCCESS(init_status)) {
        XpTestReport("Console initialization status="); XpTestHex((ULONG)init_status); XpTestReport("\n");
    }
    CHECK(NT_SUCCESS(init_status));
    UNICODE_STRING console, secondary, name, path;
    RtlInitUnicodeString(&console, L"WinSta0");
    RtlInitUnicodeString(&secondary, L"ServiceStation");
    RtlInitUnicodeString(&name, L"Default");
    HANDLE station, other_station, desktop, other_desktop, rejected;
    CHECK(ExpArm64CreateWindowStation(&console, &station) == STATUS_SUCCESS &&
          ExpArm64CreateWindowStation(&secondary, &other_station) == STATUS_SUCCESS);
    CHECK(ExpArm64CreateWindowStation(&console, &rejected) == STATUS_OBJECT_NAME_COLLISION && !rejected);
    CHECK(ExpArm64CreateDesktop(station, &name, &desktop) == STATUS_SUCCESS &&
          ExpArm64CreateDesktop(other_station, &name, &other_desktop) == STATUS_SUCCESS);
    CHECK(ExpArm64CreateDesktop(station, &name, &rejected) == STATUS_OBJECT_NAME_COLLISION && !rejected);

    HANDLE read_only;
    RtlInitUnicodeString(&path, L"\\Windows\\WindowStations\\WinSta0");
    CHECK(ObArm64OpenBootObjectByName(NULL, &path, 0, ExWindowStationObjectType, READ_CONTROL, &read_only) ==
          STATUS_SUCCESS);
    CHECK(ExpArm64CreateDesktop(read_only, &name, &rejected) == STATUS_ACCESS_DENIED && !rejected);
    CHECK(ObArm64CloseBootObjectHandle(read_only) == STATUS_SUCCESS);

    PVOID first, second, resolved;
    CHECK(ObReferenceObjectByHandle(desktop, READ_CONTROL, ExDesktopObjectType, KernelMode, &first, NULL) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(other_desktop, READ_CONTROL, ExDesktopObjectType, KernelMode, &second, NULL) == STATUS_SUCCESS &&
          first != second);
    RtlInitUnicodeString(&path, L"\\Windows\\WindowStations\\winsta0\\default");
    CHECK(ObArm64LookupBootObjectName(NULL, &path, OBJ_CASE_INSENSITIVE, ExDesktopObjectType, &resolved) == STATUS_SUCCESS &&
          resolved == first);
    ObDereferenceObject(resolved);
    HANDLE reopened;
    CHECK(ObArm64OpenBootObjectByName(station, &name, 0, ExDesktopObjectType, READ_CONTROL, &reopened) == STATUS_SUCCESS);
    CHECK(ObReferenceObjectByHandle(reopened, READ_CONTROL, ExDesktopObjectType, KernelMode, &resolved, NULL) == STATUS_SUCCESS &&
          resolved == first);
    ObDereferenceObject(resolved);
    RtlInitUnicodeString(&path, L"\\Windows\\WindowStations\\ServiceStation\\Default");
    CHECK(ObArm64LookupBootObjectName(NULL, &path, 0, ExDesktopObjectType, &resolved) == STATUS_SUCCESS &&
          resolved == second);
    ObDereferenceObject(resolved);
    RtlInitUnicodeString(&path, L"Default\\nested");
    CHECK(ObArm64LookupBootObjectName(station, &path, 0, ExDesktopObjectType, &resolved) == STATUS_OBJECT_PATH_INVALID);
    CHECK(ExpArm64CreateDesktop(reopened, &name, &rejected) == STATUS_OBJECT_TYPE_MISMATCH && !rejected);

    ULONG stations = ExWindowStationObjectType->TotalNumberOfObjects;
    ULONG desktops = ExDesktopObjectType->TotalNumberOfObjects;
    CHECK(stations == 2 && desktops == 2);
    ObDereferenceObject(first);
    ObDereferenceObject(second);
    CHECK(ObArm64CloseBootObjectHandle(desktop) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(reopened) == STATUS_SUCCESS);
    CHECK(ObArm64LookupBootObjectName(station, &name, 0, ExDesktopObjectType, &resolved) == STATUS_OBJECT_NAME_NOT_FOUND &&
          !resolved);
    CHECK(ObArm64CloseBootObjectHandle(station) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(other_desktop) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(other_station) == STATUS_SUCCESS &&
          !ExDesktopObjectType->TotalNumberOfObjects && !ExDesktopObjectType->TotalNumberOfHandles &&
          !ExWindowStationObjectType->TotalNumberOfObjects && !ExWindowStationObjectType->TotalNumberOfHandles);
    XpTestReport("Win32 console evidence stations="); XpTestHex(stations);
    XpTestReport(" desktops="); XpTestHex(desktops);
    XpTestReport(" live_handles="); XpTestHex(ExDesktopObjectType->TotalNumberOfHandles + ExWindowStationObjectType->TotalNumberOfHandles);
    XpTestReport(" live_objects="); XpTestHex(ExDesktopObjectType->TotalNumberOfObjects + ExWindowStationObjectType->TotalNumberOfObjects);
    XpTestReport("\n");
    XpTestReport("PASS: resident Win32 window stations, desktop object paths and last-close retirement\n");
}

static VOID wait_for_input_probe(MM_ARM64_USER_SPACE *space, PETHREAD thread, ULONG_PTR state, ULONG value)
{
    ULONGLONG deadline = KeQueryInterruptTime() + 50000000;
    LARGE_INTEGER poll = {.QuadPart = -10000};
    for (;;) {
        ULONG stage;
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        CHECK(MmArm64CopyUserBuffer(space, state, &stage, sizeof(stage), FALSE) == STATUS_SUCCESS);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        if (stage == value && thread->Tcb.State == Waiting && thread->Tcb.WaitReason == WrUserRequest) break;
        if (thread->Tcb.State == Terminated || KeQueryInterruptTime() >= deadline) {
            ULONG step[2];
            __asm__ volatile("msr daifset, #15" ::: "memory");
            CHECK(MmArm64CopyUserBuffer(space, state - 80, step, sizeof(step), FALSE) == STATUS_SUCCESS);
            __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
            XpTestReport("VirtIO probe stopped line="); XpTestHex(step[0]);
            XpTestReport(" service="); XpTestHex(step[1]); XpTestReport(" stage="); XpTestHex(stage);
            XpTestReport("\n");
            CHECK(FALSE);
        }
        CHECK(KeDelayExecutionThread(KernelMode, FALSE, &poll) == STATUS_SUCCESS);
    }
}

static VOID check_user_console_services(VOID)
{
    _Static_assert(__builtin_offsetof(TEB, ClientId.UniqueThread) == 0x48, "TEB client ID offset");
    _Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48 && __builtin_offsetof(OBJECT_ATTRIBUTES, ObjectName) == 16,
                   "object attributes ABI");
    _Static_assert(sizeof(UNICODE_STRING) == 16 && __builtin_offsetof(UNICODE_STRING, Buffer) == 8,
                   "unicode string ABI");
    UNICODE_STRING first, second, name;
    RtlInitUnicodeString(&first, L"WinSta0");
    RtlInitUnicodeString(&second, L"ServiceStation");
    RtlInitUnicodeString(&name, L"Default");
    HANDLE stations[2], desktops[2];
    CHECK(ExpArm64CreateWindowStation(&first, &stations[0]) == STATUS_SUCCESS &&
          ExpArm64CreateWindowStation(&second, &stations[1]) == STATUS_SUCCESS);
    for (ULONG i = 0; i < 2; ++i)
        CHECK(ExpArm64CreateDesktop(stations[i], &name, &desktops[i]) == STATUS_SUCCESS);

    HANDLE process_handle, thread_handle, sender_handle;
    PEPROCESS process;
    PETHREAD thread, sender;
    CHECK(PsArm64CreateUserProcess(PspInitialSystemProcessHandle, &process_handle) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(process_handle, SYNCHRONIZE, PsProcessType, KernelMode,
                                    (PVOID *)&process, NULL) == STATUS_SUCCESS);
    HAL_ARM64_INPUT_STATS input;
    HalArm64GetInputStats(&input);
    XpTestReport("VirtIO discovery status="); XpTestHex((ULONG)input.DiscoveryStatus);
    XpTestReport(" start="); XpTestHex((ULONG)input.StartStatus);
    XpTestReport(" nodes="); XpTestHex(input.Transports);
    XpTestReport(" keyboard="); XpTestHex(input.KeyboardIrq);
    XpTestReport(" mouse="); XpTestHex(input.MouseIrq); XpTestReport("\n");
    ULONG test_input = input.KeyboardIrq && input.MouseIrq;
    ULONG_PTR entry;
    CHECK(PsArm64InitializeProcessImage(process, XpUserImage, XpUserImageEnd - XpUserImage, 0x90000, &entry) ==
          STATUS_SUCCESS && PsArm64EnableWin32Services(process) == STATUS_SUCCESS &&
          PsArm64SetUserCallbackDispatcher(process, entry + 24) == STATUS_SUCCESS);
    XP_ARM64_USER_START start = {entry + 20, 0x51000, 0x50000, 0x70000, 0x10000};
    XP_ARM64_USER_START post = {entry + 28, 0x55000, 0x54000, 0x74000, start.Argument};
    MM_ARM64_USER_SPACE *space = PsArm64UserProcessSpace(process);
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    CHECK(MmArm64MapUserPages(space, start.StackLimit, PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS &&
          MmArm64MapUserPages(space, post.StackLimit, PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS &&
          MmArm64MapUserPages(space, start.Argument, PAGE_SIZE, PAGE_READWRITE) == STATUS_SUCCESS &&
          MmArm64CopyUserBuffer(space, start.Argument + 208, &test_input, sizeof(test_input), TRUE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(PsArm64CreateUserThread(process_handle, &start, &thread_handle, NULL) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(thread_handle, SYNCHRONIZE, PsThreadType, KernelMode,
                                    (PVOID *)&thread, NULL) == STATUS_SUCCESS);
    LARGE_INTEGER timeout = {.QuadPart = -20000000};
    LARGE_INTEGER poll = {.QuadPart = -10000};
    const XP_ARM64_BOOT_DISPLAY *display = &CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt)->Display;
    ULONGLONG deadline = KeQueryInterruptTime() + 20000000;
    ULONG64 result[31] = {0};
    ULONG pixels[12][16];
    ULONG stage;
    do {
        __asm__ volatile("msr daifset, #15" ::: "memory");
        CHECK(MmArm64CopyUserBuffer(space, start.Argument + 128, &stage, sizeof(stage), FALSE) == STATUS_SUCCESS);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        if (stage == 1) break;
        if (thread->Tcb.State == Terminated || KeQueryInterruptTime() >= deadline) {
            ULONG step[2];
            __asm__ volatile("msr daifset, #15" ::: "memory");
            CHECK(MmArm64CopyUserBuffer(space, start.Argument + 132, step, sizeof(step), FALSE) == STATUS_SUCCESS);
            __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
            XpTestReport("HWND probe stopped: native probe line="); XpTestHex(step[0]);
            XpTestReport(" service result="); XpTestHex(step[1]);
            XpTestReport(" thread exit="); XpTestHex(thread->ExitStatus); XpTestReport("\n");
            CHECK(FALSE);
        }
        CHECK(KeDelayExecutionThread(KernelMode, FALSE, &poll) == STATUS_SUCCESS);
    } while (TRUE);
    /* Two live windows, the application class, and the process EDIT class. */
    CHECK(ExpArm64CountUserObjects(process) == 4);
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 256, pixels, sizeof(pixels), FALSE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (display->VirtualBase) {
        CHECK(display->Width > 270 && display->Height > 76);
        const volatile ULONG *screen = (const volatile ULONG *)display->VirtualBase;
        for (ULONG y = 63; y <= 76; ++y) {
            for (ULONG x = 255; x <= 270; ++x) {
                ULONG expected = 0;
                if (x >= 256 && x < 268 && y >= 64 && y < 74)
                    expected = pixels[y - 63][x - 254];
                if (x >= 262 && x < 270 && y >= 68 && y < 76)
                    expected = x == 263 && y == 69 ? 0x00cef84a : 0;
                if (display->Format == XP_DISPLAY_RGBX)
                    expected = ((expected & 0xff) << 16) | (expected & 0xff00) | (expected >> 16);
                CHECK((screen[(SIZE_T)y * display->Stride + x] & 0xffffff) == expected);
            }
        }
        XpTestReport("PASS: live HWND overlap and client clipping match all 224 framebuffer pixels\n");
    }
    stage = 2;
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 128, &stage, sizeof(stage), TRUE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    /* The peer doesn't exist yet. The receiver must park in the dispatcher
     * even though its own queue already contains an unmatched message. */
    deadline = KeQueryInterruptTime() + 20000000;
    while (thread->Tcb.State != Waiting || thread->Tcb.WaitReason != WrUserRequest) {
        CHECK(thread->Tcb.State != Terminated && KeQueryInterruptTime() < deadline &&
              KeDelayExecutionThread(KernelMode, FALSE, &poll) == STATUS_SUCCESS);
    }
    CHECK(thread->Tcb.WaitMode == UserMode);
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 176, &stage, sizeof(stage), FALSE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(stage == 1);
    CHECK(PsArm64CreateUserThread(process_handle, &post, &sender_handle, NULL) == STATUS_SUCCESS &&
          ObReferenceObjectByHandle(sender_handle, SYNCHRONIZE, PsThreadType, KernelMode,
                                    (PVOID *)&sender, NULL) == STATUS_SUCCESS);
    CHECK(KeWaitForSingleObject(&sender->Tcb, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS &&
          sender->ExitStatus == 42 && !sender->Tcb.Win32Thread);
    if (test_input) {
        wait_for_input_probe(space, thread, start.Argument + 212, 1);
        XpTestReport("READY: native USER waiting for VirtIO mouse\n");
        wait_for_input_probe(space, thread, start.Argument + 212, 2);
        XpTestReport("READY: native USER waiting for VirtIO keyboard\n");
        wait_for_input_probe(space, thread, start.Argument + 212, 4);
        XpTestReport("READY: native edit window waiting for focus click\n");
        wait_for_input_probe(space, thread, start.Argument + 212, 5);
        XpTestReport("READY: native edit window waiting for initial text\n");
        wait_for_input_probe(space, thread, start.Argument + 212, 6);
        XpTestReport("READY: native edit window waiting for editing keys\n");
        wait_for_input_probe(space, thread, start.Argument + 212, 7);
        XpTestReport("READY: native edit window painted and waiting for Return\n");
        timeout.QuadPart = -50000000;
    }
    CHECK(KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS);
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument, result, sizeof(result), FALSE) == STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (thread->ExitStatus != 42) {
        XpTestReport("USER probe stopped: native probe line="); XpTestHex(result[16] >> 32);
        XpTestReport(" service result="); XpTestHex((ULONG)result[17]);
        XpTestReport(" thread exit="); XpTestHex(thread->ExitStatus); XpTestReport("\n");
    }
    XpTestReport("NtUser console evidence exit="); XpTestHex(thread->ExitStatus);
    XpTestReport(" station="); XpTestHex(result[0]); XpTestReport(" desktop="); XpTestHex(result[2]);
    XpTestReport(" replacement="); XpTestHex(result[4]); XpTestReport(" current="); XpTestHex(result[5]);
    XpTestReport(" protected_close="); XpTestHex(result[6]); XpTestReport("\n");
    CHECK(thread->ExitStatus == 42 && process->ExitStatus == 42 && !thread->Tcb.Win32Thread &&
          !process->Win32Process && !process->ObjectTable &&
          result[0] && result[1] != result[0] && result[2] && result[3] != result[2] &&
          result[4] && result[4] != result[2] && result[4] == result[5] &&
          (ULONG)result[6] == (ULONG)STATUS_HANDLE_NOT_CLOSABLE);
    CHECK(result[7] && result[8] && result[7] != result[8] &&
          (ULONG)result[9] == 0x302010 && (result[9] >> 32) == sizeof(pixels) &&
          !ExpArm64CountGdiObjects(process));
    CHECK(pixels[0][0] == 0x0020a0e0 && pixels[3][3] == 0x00f05030 &&
          pixels[5][5] == pixels[3][3] && pixels[11][15] == 0);
    XpTestReport("GDI software evidence bytes="); XpTestHex(result[9] >> 32);
    XpTestReport(" bitmap0="); XpTestHex(result[7]); XpTestReport(" bitmap1="); XpTestHex(result[8]);
    XpTestReport(" objects_after="); XpTestHex(ExpArm64CountGdiObjects(process)); XpTestReport("\n");
    for (ULONG y = 0; y < 12; ++y) {
        CHAR row[16 * 6 + 1];
        for (ULONG x = 0; x < 16; ++x)
            for (ULONG digit = 0; digit < 6; ++digit)
                row[x * 6 + digit] = "0123456789abcdef"[(pixels[y][x] >> (20 - digit * 4)) & 15];
        row[sizeof(row) - 1] = 0;
        XpTestReport("GDI row "); XpTestReport(row); XpTestReport("\n");
    }
    XpTestReport("PASS: native GDI memory DC, 32 bpp pixels, clipped overlap-safe BitBlt and rundown\n");
    if (display->VirtualBase) {
        CHECK(MmArm64QueryPhysicalAddress(display->PhysicalBase) == ~0ULL &&
              MmArm64QueryPhysicalAddress(display->VirtualBase) == display->PhysicalBase &&
              MmArm64QueryPhysicalAddress(display->VirtualBase + display->Bytes - 1) ==
                  display->PhysicalBase + display->Bytes - 1 &&
              result[10] && result[11] == 0x00e0a020);
        XpTestReport("Scanout evidence width="); XpTestHex(display->Width);
        XpTestReport(" height="); XpTestHex(display->Height); XpTestReport(" stride="); XpTestHex(display->Stride);
        XpTestReport(" format="); XpTestHex(display->Format);
        XpTestReport(" hdc="); XpTestHex(result[10]); XpTestReport("\n");
    } else CHECK(!result[10] && !result[11]);
    CHECK(result[12] >= 0xc000 && result[13] && result[14] && result[13] != result[14] &&
          (result[15] >> 32) && (result[15] >> 32) != result[14] &&
          (ULONG)result[15] == (display->VirtualBase ? 0x0022a5f3 : 0) &&
          !ExpArm64CountUserObjects(process));
    XpTestReport("HWND evidence atom="); XpTestHex(result[12]);
    XpTestReport(" lower="); XpTestHex(result[13]); XpTestReport(" upper="); XpTestHex(result[14]);
    XpTestReport(" reused="); XpTestHex(result[15] >> 32); XpTestReport(" revealed="); XpTestHex((ULONG)result[15]);
    XpTestReport("\nPASS: native HWND create, clipped DCs, visibility, handle revocation and exit rundown\n");
    ULONG64 lifecycle[6];
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 0x500, lifecycle, sizeof(lifecycle), FALSE) ==
          STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(lifecycle[0] == 0x1234 && lifecycle[1] == 0x14 && lifecycle[2] == 0x1234 &&
          lifecycle[3] == 0x1234 && lifecycle[4] != lifecycle[5] &&
          (USHORT)lifecycle[4] == (USHORT)lifecycle[5]);
    XpTestReport("USER lifecycle evidence accepted="); XpTestHex(lifecycle[0]);
    XpTestReport(" reject_nc="); XpTestHex(lifecycle[1]);
    XpTestReport(" reject_create="); XpTestHex(lifecycle[2]);
    XpTestReport(" reentrant="); XpTestHex(lifecycle[3]);
    XpTestReport(" retired="); XpTestHex(lifecycle[4]);
    XpTestReport(" replacement="); XpTestHex(lifecycle[5]);
    XpTestReport("\nPASS: native CREATESTRUCT callbacks, rejection, nested replacement and handle rundown\n");
    ULONG64 focus[4];
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 0x540, focus, sizeof(focus), FALSE) ==
          STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(focus[0] == 0x2ab0 && focus[1] == 0x2a7e && focus[2] == 0x150 &&
          focus[3] == (test_input ? 0x101 : 0));
    XpTestReport("USER focus evidence direct="); XpTestHex(focus[0]);
    XpTestReport(" redirect="); XpTestHex(focus[1]);
    XpTestReport(" destroyed="); XpTestHex(focus[2]);
    XpTestReport(" hardware="); XpTestHex(focus[3]);
    XpTestReport("\nPASS: native per-queue focus, ordered callbacks and reentrant HWND changes\n");
    ULONG64 children[4];
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 0x560, children, sizeof(children), FALSE) ==
          STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(children[0] == 4 && children[1] == (display->VirtualBase ? 315 : 0) &&
          children[2] == 0x123ba9 && children[3]);
    XpTestReport("USER child evidence create_callbacks="); XpTestHex(children[0]);
    XpTestReport(" clip_pixels="); XpTestHex(children[1]);
    XpTestReport(" teardown="); XpTestHex(children[2]);
    XpTestReport(" retired="); XpTestHex(children[3]);
    XpTestReport("\nPASS: native child HWND ancestry, clipped Z order, descendant messages and cascade rundown\n");
    ULONG64 text[4];
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 0x580, text, sizeof(text), FALSE) ==
          STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(text[0] == 0x03a90070 && text[1] == 3 && text[2] == 0x71595870 && text[3] == 8);
    XpTestReport("USER text send evidence utf16="); XpTestHex(text[0]);
    XpTestReport(" nested_length="); XpTestHex(text[1]);
    XpTestReport(" replacement="); XpTestHex(text[2]);
    XpTestReport(" callbacks="); XpTestHex(text[3]);
    XpTestReport("\nPASS: native synchronous Unicode window text, selection replacement and bounded callback output\n");
    ULONG64 edit_class[4];
    __asm__ volatile("msr daifset, #15" ::: "memory");
    CHECK(MmArm64CopyUserBuffer(space, start.Argument + 0x5a0, edit_class, sizeof(edit_class), FALSE) ==
          STATUS_SUCCESS);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(edit_class[0] >= 0xc000 && edit_class[1] == 0x420a0d41 && edit_class[2] &&
          edit_class[3] && edit_class[2] != edit_class[3]);
    XpTestReport("USER32 EDIT evidence atom="); XpTestHex(edit_class[0]);
    XpTestReport(" multiline="); XpTestHex(edit_class[1]);
    XpTestReport(" single="); XpTestHex(edit_class[2]);
    XpTestReport(" multi="); XpTestHex(edit_class[3]);
    XpTestReport("\nPASS: native USER32 registers EDIT and keeps distinct single and multiline control state\n");





    CHECK((result[18] >> 32) == 2 && (ULONG)result[19] == 1 &&
          (result[19] >> 32) == 0x4b2e1f && (ULONG)result[20] == 0x674455 &&
          (result[20] >> 32) == 0x3050f0 && result[21] == 0x1122334455667788ULL);
    XpTestReport("Paint evidence callbacks="); XpTestHex(result[18] >> 32);
    XpTestReport(" erases="); XpTestHex((ULONG)result[19]);
    XpTestReport(" hole="); XpTestHex(result[19] >> 32);
    XpTestReport(" deferred="); XpTestHex((ULONG)result[20]);
    XpTestReport(" callback_result="); XpTestHex(result[21]);
    XpTestReport("\nPASS: native WM_PAINT, disjoint update clipping, nested erase and EndPaint rundown\n");
    CHECK((ULONG)result[22] == 1 && (result[22] >> 32) == 1 &&
          (ULONG)result[23] == 0x4422 && (result[23] >> 32) == (ULONG_PTR)thread->Cid.UniqueThread &&
          result[24] == 0xfeedface11223344ULL && (ULONG)result[25] == 27);
    XpTestReport("USER queue evidence receiver="); XpTestHex((ULONG_PTR)thread->Cid.UniqueThread);
    XpTestReport(" sender="); XpTestHex((ULONG_PTR)sender->Cid.UniqueThread);
    XpTestReport(" wait_reason="); XpTestHex(WrUserRequest);
    XpTestReport(" dispatch="); XpTestHex(result[24]); XpTestReport(" quit="); XpTestHex((ULONG)result[25]);
    XpTestReport("\nPASS: native GetMessage blocks, cross-thread posts wake and dispatch, WM_QUIT drains\n");
    if (test_input) {
        HalArm64GetInputStats(&input);
        CHECK(NT_SUCCESS(input.DiscoveryStatus) && NT_SUCCESS(input.StartStatus) &&
              input.KeyboardIrq != input.MouseIrq && input.KeyboardInterrupts && input.MouseInterrupts &&
              input.KeyboardEvents >= 2 && input.MouseEvents >= 4 && !input.Faults &&
              (result[26] >> 32) == 8 && (ULONG)result[27] == 5 &&
              (result[27] >> 32) == 0x7cab5e && (ULONG)result[28] == 0x4392b1);
        XpTestReport("VirtIO input evidence key_irq="); XpTestHex(input.KeyboardIrq);
        XpTestReport(" mouse_irq="); XpTestHex(input.MouseIrq);
        XpTestReport(" key_interrupts="); XpTestHex(input.KeyboardInterrupts);
        XpTestReport(" mouse_interrupts="); XpTestHex(input.MouseInterrupts);
        XpTestReport(" key_records="); XpTestHex(input.KeyboardEvents);
        XpTestReport(" mouse_records="); XpTestHex(input.MouseEvents);
        XpTestReport("\nPASS: VirtIO keyboard and mouse IRQs wake native USER, dispatch and paint\n");
        CHECK((result[28] >> 32) == 0x00420063 && (ULONG)result[29] == 1 &&
              (result[29] >> 32) == 16 && (ULONG)result[30] == 8);
        XpTestReport("PASS: native TranslateMessage queues WM_CHAR, USER32 EDIT reads cB and text extent is 16x8\n");
    } else CHECK(!result[26] && !result[27] && !result[28] && !result[29] && !result[30]);
    /* The thread signals before the executive reaper drops its reference.
     * Leave IRQ dispatch active until the kernel stack has been released. */
    deadline = KeQueryInterruptTime() + 10000000;
    extern BOOLEAN PsReaperActive;
    while (thread->Tcb.InitialStack || sender->Tcb.InitialStack || PsReaperActive) {
        CHECK(KeQueryInterruptTime() < deadline &&
              KeDelayExecutionThread(KernelMode, FALSE, &poll) == STATUS_SUCCESS);
    }
    ObDereferenceObject(thread);
    ObDereferenceObject(sender);
    ObDereferenceObject(process);
    CHECK(ObArm64CloseBootObjectHandle(thread_handle) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(sender_handle) == STATUS_SUCCESS &&
          ObArm64CloseBootObjectHandle(process_handle) == STATUS_SUCCESS);
    for (ULONG i = 0; i < 2; ++i) {
        CHECK(ObArm64CloseBootObjectHandle(desktops[i]) == STATUS_SUCCESS &&
              ObArm64CloseBootObjectHandle(stations[i]) == STATUS_SUCCESS);
    }
    CHECK(!ExWindowStationObjectType->TotalNumberOfHandles && !ExDesktopObjectType->TotalNumberOfHandles &&
          !ExWindowStationObjectType->TotalNumberOfObjects && !ExDesktopObjectType->TotalNumberOfObjects);
    XpTestReport("PASS: native NtUser window station and desktop binding, protected close and process rundown\n");
}

VOID XpCheckPhase1PoolLookasides(VOID);

typedef struct {
    WORK_QUEUE_ITEM Work;
    PKEVENT Release;
    KEVENT Entered;
    KEVENT Ended;
} XP_QUEUE_BLOCKER;

static VOID block_worker(PVOID parameter)
{
    XP_QUEUE_BLOCKER *work = parameter;
    KeSetEvent(&work->Entered, 0, FALSE);
    CHECK(KeWaitForSingleObject(work->Release, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    KeSetEvent(&work->Ended, 0, FALSE);
}

typedef struct {
    WORK_QUEUE_ITEM Work;
    KEVENT Done;
    PETHREAD Thread;
} XP_QUEUE_PROBE;

static VOID record_new_worker(PVOID parameter)
{
    XP_QUEUE_PROBE *probe = parameter;
    probe->Thread = PsGetCurrentThread();
    ObReferenceObject(probe->Thread);
    KeSetEvent(&probe->Done, 0, FALSE);
}

static ULONGLONG check_worker_growth(WORK_QUEUE_TYPE type, ULONG count)
{
    PEX_WORK_QUEUE queue = &ExWorkerQueue[type];
    CHECK(queue->Info.WorkerCount == count && !queue->DynamicThreadCount &&
          !KeReadStateQueue(&queue->WorkerQueue));
    KEVENT release, park;
    KeInitializeEvent(&release, NotificationEvent, FALSE);
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    LARGE_INTEGER short_wait = {.QuadPart = -100000};
    LARGE_INTEGER wait = {.QuadPart = -10000000};
    XP_QUEUE_BLOCKER blockers[7];
    CHECK(count <= sizeof(blockers) / sizeof(*blockers));
    for (ULONG i = 0; i < count; ++i) {
        XP_QUEUE_BLOCKER *work = &blockers[i];
        work->Release = &release;
        KeInitializeEvent(&work->Entered, SynchronizationEvent, FALSE);
        KeInitializeEvent(&work->Ended, SynchronizationEvent, FALSE);
        ExInitializeWorkItem(&work->Work, block_worker, work);
        ExQueueWorkItem(&work->Work, type);
        CHECK(KeWaitForSingleObject(&work->Entered, Executive, KernelMode, FALSE, &wait) == STATUS_SUCCESS);
    }

    /* Every resident worker has entered its callback and left the queue
     * to wait on a real dispatcher event. There is nobody to pop new work. */
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &short_wait) == STATUS_TIMEOUT &&
          !queue->WorkerQueue.CurrentCount && IsListEmpty(&queue->WorkerQueue.Header.WaitListHead) &&
          !queue->DynamicThreadCount);
    XP_QUEUE_PROBE probe = {0};
    KeInitializeEvent(&probe.Done, SynchronizationEvent, FALSE);
    ExInitializeWorkItem(&probe.Work, record_new_worker, &probe);
    ULONGLONG start = KeQueryInterruptTime();
    ExQueueWorkItem(&probe.Work, type);
    CHECK(KeReadStateQueue(&queue->WorkerQueue) == 1);
    LARGE_INTEGER deadline = {.QuadPart = type == CriticalWorkQueue ? -9000000 : -35000000};
    CHECK(KeWaitForSingleObject(&probe.Done, Executive, KernelMode, FALSE, &deadline) == STATUS_SUCCESS);
    ULONGLONG elapsed = KeQueryInterruptTime() - start;
    CHECK(queue->DynamicThreadCount == 1 && queue->Info.WorkerCount == count + 1 &&
          probe.Thread && probe.Thread->ThreadsProcess == PsInitialSystemProcess &&
          probe.Thread->ActiveExWorker && probe.Thread->Tcb.Queue == &queue->WorkerQueue);
    if (type == DelayedWorkQueue) CHECK(elapsed >= 9000000 && queue->QueueDepthLastPass == 1);

    KeSetEvent(&release, 0, FALSE);
    for (ULONG i = 0; i < count; ++i)
        CHECK(KeWaitForSingleObject(&blockers[i].Ended, Executive, KernelMode, FALSE, &wait) == STATUS_SUCCESS);
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &short_wait) == STATUS_TIMEOUT);
    PKTHREAD worker = &probe.Thread->Tcb;
    CHECK(worker->State == Waiting && worker->WaitReason == WrQueue &&
          worker->Timer.Header.Inserted && !worker->EnableStackSwap &&
          !KeReadStateQueue(&queue->WorkerQueue) && !queue->WorkerQueue.CurrentCount);
    ULONGLONG remaining = worker->Timer.DueTime.QuadPart - KeQueryInterruptTime();
    CHECK(remaining >= 5980000000ULL && remaining <= 6000000000ULL);
    ObDereferenceObject(probe.Thread);
    return elapsed;
}

static ULONG check_worker_balance(VOID)
{
    PETHREAD manager = ExpWorkerThreadBalanceManagerPtr;
    CHECK(manager && manager->ThreadsProcess == PsInitialSystemProcess &&
          manager->Tcb.State == Waiting && manager->Tcb.WaitBlockList);
    PKWAIT_BLOCK timer = manager->Tcb.WaitBlockList;
    PKWAIT_BLOCK demand = timer->NextWaitBlock;
    CHECK(demand && demand->NextWaitBlock == timer && timer->WaitKey == 0 && demand->WaitKey == 1);
    ULONGLONG demand_time = check_worker_growth(CriticalWorkQueue, 5);
    ULONGLONG timer_time = check_worker_growth(DelayedWorkQueue, 7);
    CHECK(manager->Tcb.State == Waiting && manager->Tcb.ContextSwitches >= 4);
    check_executive_event_wait();
    XpTestReport("Balance Phase 1 evidence demand="); XpTestHex(demand_time);
    XpTestReport(" timer="); XpTestHex(timer_time);
    XpTestReport(" manager_runs="); XpTestHex(manager->Tcb.ContextSwitches);
    XpTestReport(" dynamic="); XpTestHex(ExWorkerQueue[CriticalWorkQueue].DynamicThreadCount +
                                        ExWorkerQueue[DelayedWorkQueue].DynamicThreadCount);
    XpTestReport("\n");
    XpTestReport("PASS: XP Phase 1 worker balance, dynamic queues and executive Event waits\n");
    return 1 + ExWorkerQueue[CriticalWorkQueue].DynamicThreadCount + ExWorkerQueue[DelayedWorkQueue].DynamicThreadCount;
}

extern LARGE_INTEGER KdPerformanceCounterRate;
VOID XpCheckBootSecurityPhase1(VOID);
VOID XpCheckBootSectionPhase1(VOID);

static VOID check_device_path_bounds(VOID)
{
    UCHAR path[] = {1, 1, 5, 0, 0x32, 3, 5, 4, 0, 0x7f, 0xff, 4, 0};
    CHAR name[XP_ARM64_ARC_NAME_CAPACITY];
    CHECK(BlArm64EncodeArcDevicePath(path, sizeof(path), name) == sizeof(path) &&
          !memcmp(name, "uefi(0101050032030504007fff0400)", sizeof("uefi(0101050032030504007fff0400)")));
    CHECK(!BlArm64EncodeArcDevicePath(path, sizeof(path) - 1, name) && !name[0]);
    path[10] = 1; /* End Instance cannot identify a single volume. */
    CHECK(!BlArm64EncodeArcDevicePath(path, sizeof(path), name) && !name[0]);
    path[10] = 0xff;
    path[2] = 3;
    CHECK(!BlArm64EncodeArcDevicePath(path, sizeof(path), name) && !name[0]);
}

static VOID check_system_root(PXP_ARM64_BOOT_BLOCK block)
{
    check_device_path_bounds();
    const XP_ARM64_BOOT_VOLUME *volume = &block->Volume;
    static const CHAR digits[] = "0123456789abcdef";
    ULONG bytes = volume->DevicePathBytes;
    CHECK(bytes >= 8 && bytes <= sizeof(volume->DevicePath) &&
          !memcmp(volume->DevicePath + bytes - 4, "\x7f\xff\x04\x00", 4) &&
          block->Nt.ArcBootDeviceName == volume->ArcName && block->Nt.NtBootPathName == volume->NtDirectory &&
          !memcmp(volume->ArcName, "uefi(", 5));
    for (ULONG i = 0; i < bytes; ++i) {
        CHECK(volume->ArcName[5 + 2 * i] == digits[volume->DevicePath[i] >> 4] &&
              volume->ArcName[6 + 2 * i] == digits[volume->DevicePath[i] & 15]);
    }
    CHECK(volume->ArcName[5 + 2 * bytes] == ')' && !volume->ArcName[6 + 2 * bytes]);
    const UNICODE_STRING *file = &block->KernelModule.FullDllName;
    ULONG prefix = file->Length / sizeof(WCHAR);
    while (prefix && file->Buffer[prefix - 1] != L'\\') --prefix;
    CHECK(prefix && prefix < sizeof(volume->NtDirectory) && !volume->NtDirectory[prefix]);
    for (ULONG i = 0; i < prefix; ++i) CHECK(file->Buffer[i] == (WCHAR)volume->NtDirectory[i]);

    const PCWSTR directories[] = {L"\\ArcName", L"\\Device"};
    UNICODE_STRING name;
    for (ULONG i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        RtlInitUnicodeString(&name, directories[i]);
        PVOID directory;
        CHECK(ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE, ObpDirectoryObjectType,
              &directory) == STATUS_SUCCESS);
        POBJECT_HEADER header = OBJECT_TO_OBJECT_HEADER(directory);
        CHECK((header->Flags & OB_FLAG_PERMANENT_OBJECT) && header->SecurityDescriptor && !header->HandleCount);
        ObDereferenceObject(directory);
    }

    RtlInitUnicodeString(&name, L"\\SystemRoot");
    HANDLE handle;
    CHECK(ObArm64OpenBootObjectByName(NULL, &name, OBJ_CASE_INSENSITIVE, ObpSymbolicLinkObjectType,
          SYMBOLIC_LINK_QUERY, &handle) == STATUS_SUCCESS);
    PVOID link;
    CHECK(ObReferenceObjectByHandle(handle, SYMBOLIC_LINK_QUERY, ObpSymbolicLinkObjectType, KernelMode,
          &link, NULL) == STATUS_SUCCESS);
    CHECK((OBJECT_TO_OBJECT_HEADER(link)->Flags & OB_FLAG_PERMANENT_OBJECT) &&
          OBJECT_TO_OBJECT_HEADER(link)->SecurityDescriptor);
    ObDereferenceObject(link);
    WCHAR target[8 + XP_ARM64_ARC_NAME_CAPACITY + XP_ARM64_BOOT_DIRECTORY_CAPACITY];
    UNICODE_STRING result = {0, 2, target};
    ULONG required = 0;
    CHECK(NtQuerySymbolicLinkObject(handle, &result, &required) == STATUS_BUFFER_TOO_SMALL &&
          !result.Length && required > result.MaximumLength);
    result.MaximumLength = sizeof(target);
    CHECK(NtQuerySymbolicLinkObject(handle, &result, &required) == STATUS_SUCCESS &&
          required == result.Length + sizeof(WCHAR) && !target[result.Length / sizeof(WCHAR)]);
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS);

    CHAR observed[sizeof(target) / sizeof(*target)];
    ULONG units = result.Length / sizeof(WCHAR);
    for (ULONG i = 0; i <= units; ++i) {
        CHECK(target[i] < 128);
        observed[i] = (CHAR)target[i];
    }
    const ULONG arc_length = 2 * bytes + 6;
    CHECK(units == 9 + arc_length + prefix - 1 && !memcmp(observed, "\\ArcName\\", 9) &&
          !memcmp(observed + 9, volume->ArcName, arc_length) &&
          !memcmp(observed + 9 + arc_length, volume->NtDirectory, prefix - 1));

    /* Real object-name reparsing reaches the ARC directory. The NT storage
     * path will populate the device alias after kernel startup. */
    PVOID resolved;
    CHECK(ObArm64LookupBootObjectName(NULL, &name, OBJ_CASE_INSENSITIVE, NULL, &resolved) ==
          STATUS_OBJECT_NAME_NOT_FOUND && !resolved);
    XpTestReport("SystemRoot evidence device_bytes="); XpTestHex(bytes);
    XpTestReport(" directory_bytes="); XpTestHex(prefix);
    XpTestReport(" target="); XpTestReport(observed); XpTestReport("\n");
    XpTestReport("PASS: firmware volume handoff and original XP SystemRoot namespace publication\n");
}

static VOID check_machine_phase1(ULONG64 old_selector, ULONG64 old_mair)
{
    ULONG64 selector, mair, ctr, midr, hertz;
    __asm__ volatile("mrs %0, csselr_el1\nmrs %1, mair_el1\nmrs %2, ctr_el0\n"
                     "mrs %3, midr_el1\nmrs %4, cntfrq_el0"
                     : "=r"(selector), "=r"(mair), "=r"(ctr), "=r"(midr), "=r"(hertz));
    ULONG alignment = KeGetRecommendedSharedDataAlignment();
    CHECK(selector == old_selector && mair == old_mair && midr &&
          KeProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 &&
          SharedUserData->ImageNumberLow == IMAGE_FILE_MACHINE_ARM64 &&
          SharedUserData->ImageNumberHigh == IMAGE_FILE_MACHINE_ARM64 &&
          alignment == KeLargestCacheLine && !(alignment & (alignment - 1)) &&
          alignment >= (4u << ((ctr >> 16) & 15)) && alignment >= (4u << (ctr & 15)));

    HAL_ARM64_IRQ_STATS irq;
    HalArm64GetIrqStats(&irq);
    LARGE_INTEGER first, later, frequency;
    CHECK(NtQueryPerformanceCounter(NULL, NULL) == STATUS_INVALID_PARAMETER);
    ULONG64 counter_before = HalArm64ReadCounter();
    CHECK(NtQueryPerformanceCounter(&first, &frequency) == STATUS_SUCCESS);
    CHECK((ULONG64)first.QuadPart >= counter_before && (ULONG64)first.QuadPart <= HalArm64ReadCounter() &&
          (ULONG64)frequency.QuadPart == (ULONG)hertz && (ULONG64)frequency.QuadPart == irq.Frequency &&
          KdPerformanceCounterRate.QuadPart == frequency.QuadPart);
    ULONGLONG interrupt_time = KeQueryInterruptTime();
    LARGE_INTEGER delay = {.QuadPart = -10000};
    CHECK(KeDelayExecutionThread(KernelMode, FALSE, &delay) == STATUS_SUCCESS &&
          NtQueryPerformanceCounter(&later, NULL) == STATUS_SUCCESS &&
          later.QuadPart > first.QuadPart && KeQueryInterruptTime() > interrupt_time);

    XpTestReport("Machine Phase 1 evidence midr="); XpTestHex(midr);
    XpTestReport(" cache_line="); XpTestHex(alignment);
    XpTestReport(" frequency="); XpTestHex(frequency.QuadPart);
    XpTestReport(" counter_delta="); XpTestHex(later.QuadPart - first.QuadPart);
    XpTestReport(" image_machine="); XpTestHex(SharedUserData->ImageNumberLow);
    XpTestReport(" kd_frequency="); XpTestHex(KdPerformanceCounterRate.QuadPart);
    XpTestReport("\n");
    XpTestReport("PASS: original XP Ke and KD Phase 1, live ARM64 cache and performance counter\n");
}

VOID XpCheckBootPhase1(PXP_ARM64_BOOT_BLOCK block)
{
    extern ULONG InitializationPhase;
    PVOID anchor = (PVOID)MmGetPhysicalMemoryRanges;
    PKLDR_DATA_TABLE_ENTRY module = MiLookupDataTableEntry(anchor, FALSE);
    CHECK(module && module != &block->KernelModule && module->DllBase == PsNtosImageBase &&
          module->DllBase == (PVOID)(ULONG_PTR)block->KernelImage.VirtualBase &&
          module->SizeOfImage == block->KernelImage.Size &&
          module->BaseDllName.Buffer != block->KernelModule.BaseDllName.Buffer &&
          module->FullDllName.Buffer != block->KernelModule.FullDllName.Buffer &&
          RtlEqualUnicodeString(&module->BaseDllName, &block->KernelModule.BaseDllName, FALSE) &&
          RtlEqualUnicodeString(&module->FullDllName, &block->KernelModule.FullDllName, FALSE) &&
          IsListEmpty(&block->Nt.BootDriverListHead));
    CHECK(!MiLookupDataTableEntry((PVOID)KI_USER_SHARED_DATA, FALSE));

    XpCheckBootObPhase1Rollback();
    HAL_ARM64_IRQ_STATS before, after;
    HalArm64GetIrqStats(&before);
    ULONG64 cache_selector, mair;
    __asm__ volatile("mrs %0, csselr_el1\nmrs %1, mair_el1" : "=r"(cache_selector), "=r"(mair));
    CHECK(!before.TimerActive && !InitializationPhase && !ExPageLockHandle &&
          KeProcessorArchitecture == PROCESSOR_ARCHITECTURE_UNKNOWN && !KdPerformanceCounterRate.QuadPart);
    HANDLE handle;
    CHECK(KiArm64StartPhase1(NULL, &handle) == STATUS_INVALID_DEVICE_STATE && !handle);
    CHECK(KiArm64StartPhase1(block, &handle) == STATUS_SUCCESS && (LONG_PTR)handle < 0);
    PETHREAD thread;
    CHECK(ObReferenceObjectByHandle(handle, SYNCHRONIZE, PsThreadType, KernelMode,
                                    (PVOID *)&thread, NULL) == STATUS_SUCCESS &&
          thread->ThreadsProcess == PsInitialSystemProcess);
    CHECK(KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(thread->ExitStatus == STATUS_SUCCESS && thread->Tcb.State == Terminated &&
          thread->Tcb.BasePriority == NORMAL_BASE_PRIORITY && InitializationPhase == 1 && ExPageLockHandle);
    check_active_process(PsIdleProcess, PsInitialSystemProcess, block);
    check_machine_phase1(cache_selector, mair);
    XpCheckBootSecurityPhase1();
    check_process_handle_tables();
    check_child_process();
    check_system_root(block);
    XpCheckBootSectionPhase1();

    const IMAGE_SECTION_HEADER *header = ExPageLockHandle;
    CHECK(!memcmp(header->Name, "PAGELK\0", sizeof(header->Name)) &&
          MmArm64QueryTranslation((ULONG_PTR)header, TRUE) & 1);
    ULONG_PTR base = (ULONG_PTR)module->DllBase + header->VirtualAddress;
    PFN_NUMBER first = (block->KernelImage.Base + header->VirtualAddress) >> PAGE_SHIFT;
    ULONG pages = BYTES_TO_PAGES(header->Misc.VirtualSize);
    CHECK(pages && (ULONG_PTR)anchor >= base &&
          (ULONG_PTR)anchor - base < header->Misc.VirtualSize &&
          MmArm64QueryPhysicalAddress(base) == ((ULONG64)first << PAGE_SHIFT));
    check_pagelk_refs(first, pages, 1);
    CHECK(MmLockPagableDataSection(anchor) == ExPageLockHandle);
    MmLockPagableSectionByHandle(ExPageLockHandle);
    check_pagelk_refs(first, pages, 2);
    MmUnlockPagableImageSection(ExPageLockHandle);
    check_pagelk_refs(first, pages, 2);

    /* Call the original code in this section while another holder pins it. */
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    CHECK(ranges && MmPhysicalMemoryBlock->NumberOfRuns);
    ULONG i;
    for (i = 0; i < MmPhysicalMemoryBlock->NumberOfRuns; ++i) {
        CHECK(ranges[i].BaseAddress.QuadPart == (LONGLONG)MmPhysicalMemoryBlock->Run[i].BasePage * PAGE_SIZE &&
              ranges[i].NumberOfBytes.QuadPart == (LONGLONG)MmPhysicalMemoryBlock->Run[i].PageCount * PAGE_SIZE);
    }
    CHECK(!ranges[i].BaseAddress.QuadPart && !ranges[i].NumberOfBytes.QuadPart);
    ExFreePool(ranges);
    check_pagelk_refs(first, pages, 2);
    MmUnlockPagableImageSection(ExPageLockHandle);
    check_pagelk_refs(first, pages, 1);

    KEVENT event;
    KeInitializeEvent(&event, SynchronizationEvent, FALSE);
    LARGE_INTEGER delay = {.QuadPart = -100000};
    CHECK(KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &delay) == STATUS_TIMEOUT);
    HalArm64GetIrqStats(&after);
    CHECK(after.TimerActive && after.Interrupts > before.Interrupts &&
          after.GicDistributorVa == before.GicDistributorVa &&
          after.GicCpuInterfaceVa == before.GicCpuInterfaceVa && !after.LastFailure &&
          !thread->Tcb.InitialStack);
    ObDereferenceObject(thread);
    ULONG workers = check_static_workers(block);
    check_executive_atoms();
    XpTestReport("PASS: XP Phase 1 atoms and resident executive work queues\n");
    workers += check_worker_balance();
    check_executive_dispatcher_types();
    check_executive_callbacks();
    check_executive_semaphore();
    check_executive_timers();
    check_executive_profile();
    check_executive_uuids();
    check_executive_keyed_events();
    check_win32_object_types();
    XpCheckPhase1PoolLookasides();
    CHECK(ObArm64CloseBootObjectHandle(handle) == STATUS_SUCCESS &&
          PspCidTable->HandleCount == 2 + workers && PsInitialSystemProcess->ActiveThreads == 1 + workers);
    HANDLE duplicate;
    CHECK(KiArm64StartPhase1(block, &duplicate) == STATUS_INVALID_DEVICE_STATE && !duplicate);

    XpCheckBootObPhase1();
    check_console_desktops();
    check_user_console_services();
    KiArm64DisableTimerDispatch();
    XpTestReport("Phase 1 entry evidence phase="); XpTestHex(InitializationPhase);
    XpTestReport(" section="); XpTestHex(base);
    XpTestReport(" pfn="); XpTestHex(first);
    XpTestReport(" pages="); XpTestHex(pages);
    XpTestReport(" ranges="); XpTestHex(i);
    XpTestReport(" clock_irqs="); XpTestHex(after.Interrupts - before.Interrupts);
    XpTestReport(" live_cids="); XpTestHex(PspCidTable->HandleCount); XpTestReport("\n");
    XpTestReport("PASS: XP Phase 1 entry, loaded-module PAGELK pinning and HAL runtime clock\n");
}
