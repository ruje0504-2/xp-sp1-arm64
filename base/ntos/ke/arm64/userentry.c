#include "ki.h"
#include "userentry.h"
#include "trapcontext.h"
#include "../../ex/arm64/bootwin32.h"
#include "../../mm/arm64/bootpool.h"
#include "xp-user-services.h"

VOID KiArm64EnterUserRaw(ULONG64 Root, KARM64_USER_ENTRY *Entry);
C_ASSERT(FIELD_OFFSET(KARM64_USER_ENTRY, KernelSp) == sizeof(KTRAP_FRAME));
C_ASSERT(FIELD_OFFSET(KARM64_USER_ENTRY, VectorKind) == sizeof(KTRAP_FRAME) + 24);

NTSTATUS KiArm64RunUser(MM_ARM64_USER_SPACE *space, KARM64_USER_ENTRY *entry)
{
    ULONG64 daif, current_root;
    __asm__ volatile("mrs %0, daif\nmrs %1, ttbr0_el1" : "=r"(daif), "=r"(current_root));
    if (!space || !space->Root || !space->Boot || KeNumberProcessors != 1 ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || (current_root & 0x0000fffffffff000ULL) != space->Boot->Tables.Root)
        return STATUS_INVALID_DEVICE_STATE;
    if (!entry || ((ULONG_PTR)entry & 15) ||
        !MmArm64IsBootNonPagedPoolAddress(entry) ||
        !MmArm64IsBootNonPagedPoolAddress((PUCHAR)entry + sizeof(*entry) - 1) ||
        entry->Frame.Pc < 0x10000 || entry->Frame.Pc >= 0x80000000 || (entry->Frame.Pc & 3) ||
        entry->Frame.Sp <= 0x10000 || entry->Frame.Sp > 0x80000000 || (entry->Frame.Sp & 15))
        return STATUS_INVALID_PARAMETER;
    __asm__ volatile("msr daifset, #15\nisb" ::: "memory");
    /* EL0 IRQs remain enabled. An IRQ returns to the kernel root before
     * ordinary EL1 IRQ dispatch, so its handler may safely use PCR/stack. */
    entry->Frame.Cpsr = (entry->Frame.Cpsr & 0xf0000000) | 0x340;
    KiArm64EnterUserRaw(space->Root, entry);
    entry->KernelSp = entry->KernelRoot = 0;
    entry->Frame.PreviousMode = UserMode;
    entry->Frame.FpStateSaved = TRUE;
    entry->Frame.ExceptionActive = entry->VectorKind == 0;
    entry->Frame.PreviousIrql = PASSIVE_LEVEL;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return STATUS_SUCCESS;
}

typedef struct DECLSPEC_ALIGN(16) {
    CONTEXT Context;
    EXCEPTION_RECORD Record;
} KARM64_USER_EXCEPTION_FRAME;

NTSTATUS KiArm64DispatchUserException(MM_ARM64_USER_SPACE *space, KARM64_USER_ENTRY *entry,
                                      ULONG_PTR dispatcher)
{
    PKTRAP_FRAME frame = &entry->Frame;
    if (entry->VectorKind || frame->PreviousMode != UserMode || (frame->Cpsr & 31))
        return STATUS_UNHANDLED_EXCEPTION;
    KARM64_USER_EXCEPTION_FRAME delivery = {0};
    EXCEPTION_RECORD *record = &delivery.Record;
    ULONG ec = (ULONG)(frame->Esr >> 26);
    record->ExceptionAddress = (PVOID)frame->Pc;
    if (ec == 0x24 || ec == 0x20) {
        if ((frame->Esr & 63) == 0x21) record->ExceptionCode = STATUS_DATATYPE_MISALIGNMENT;
        else {
            record->ExceptionCode = STATUS_ACCESS_VIOLATION;
            if (!(frame->Esr & (1u << 10))) {
                record->NumberParameters = 2;
                record->ExceptionInformation[0] = ec == 0x20 ? 8 : ((frame->Esr >> 6) & 1);
                record->ExceptionInformation[1] = frame->Far;
            }
        }
    } else if (ec == 0x3c) record->ExceptionCode = STATUS_BREAKPOINT;
    else if (ec == 0x22 || ec == 0x26) record->ExceptionCode = STATUS_DATATYPE_MISALIGNMENT;
    else record->ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;

    if (!dispatcher) return record->ExceptionCode;
    if (frame->Sp < 0x10000 + sizeof(delivery) || frame->Sp > 0x80000000) return STATUS_STACK_OVERFLOW;
    ULONG_PTR user_sp = (frame->Sp - sizeof(delivery)) & ~(ULONG_PTR)15;
    KiArm64CaptureTrapContext(frame, &delivery.Context);
    delivery.Context.Cpsr &= 0xf0000000u;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64UserCodeAddress(space, dispatcher) ?
        MmArm64CopyUserBuffer(space, user_sp, &delivery, sizeof(delivery), TRUE) : STATUS_ACCESS_VIOLATION;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!NT_SUCCESS(status)) return STATUS_ACCESS_VIOLATION;
    frame->Sp = user_sp;
    frame->Pc = dispatcher;
    frame->X[0] = user_sp + FIELD_OFFSET(KARM64_USER_EXCEPTION_FRAME, Record);
    frame->X[1] = user_sp + FIELD_OFFSET(KARM64_USER_EXCEPTION_FRAME, Context);
    frame->X[30] = 0;
    return STATUS_SUCCESS;
}

/* KiContinuePreviousModeUser captures an entire aligned CONTEXT before
 * modifying the live trap frame. User VAs here belong to a private TTBR0. */
static NTSTATUS continue_user(MM_ARM64_USER_SPACE *space, PKTRAP_FRAME frame)
{
    CONTEXT context;
    ULONG_PTR address = frame->X[0];
    BOOLEAN test_alert = (BOOLEAN)frame->X[1];
    if (address & (__alignof(CONTEXT)-1)) return STATUS_DATATYPE_MISALIGNMENT;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64CopyUserBuffer(space, address, &context, sizeof(context), FALSE);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!NT_SUCCESS(status)) return STATUS_ACCESS_VIOLATION;
    ULONG flags = context.ContextFlags;
    if (!(flags & CONTEXT_ARM64) || !(flags & 7) || (flags & ~CONTEXT_FULL)) return STATUS_INVALID_PARAMETER;
    if (flags & 1) {
        if (context.Pc < 0x10000 || context.Pc >= 0x80000000 || (context.Pc & 3) ||
            context.Sp <= 0x10000 || context.Sp > 0x80000000 || (context.Sp & 15) ||
            (context.Cpsr & ~0xf0000000u)) return STATUS_INVALID_PARAMETER;
        frame->Pc = context.Pc;
        frame->Sp = context.Sp;
        frame->Cpsr = context.Cpsr;
        frame->X[29] = context.Fp;
        frame->X[30] = context.Lr;
    }
    if (flags & 2) RtlCopyMemory(frame->X, context.X, FIELD_OFFSET(CONTEXT, Fp) - FIELD_OFFSET(CONTEXT, X));
    if (flags & 4) {
        RtlCopyMemory(frame->V, context.V, sizeof(context.V));
        frame->Fpcr = context.Fpcr;
        frame->Fpsr = context.Fpsr;
    }
    if (test_alert) KeTestAlertThread(UserMode);
    return STATUS_SUCCESS;
}

/* KiDeliverApc has removed one user KAPC and run its kernel routine.
 * Save the interrupted user state before redirecting to the user thunk. */
VOID KiInitializeUserApc(PKEXCEPTION_FRAME exception_frame, PKTRAP_FRAME frame,
                         PKNORMAL_ROUTINE normal, PVOID context, PVOID argument1, PVOID argument2)
{
    UNREFERENCED_PARAMETER(exception_frame);
    KARM64_USER_ENTRY *entry = CONTAINING_RECORD(frame, KARM64_USER_ENTRY, Frame);
    CONTEXT saved;
    if (frame->Sp < 0x10000 + sizeof(saved) || frame->Sp > 0x80000000) {
        entry->ApcStatus = STATUS_STACK_OVERFLOW;
        return;
    }
    ULONG_PTR new_sp = (frame->Sp - sizeof(saved)) & ~(ULONG_PTR)15;
    KiArm64CaptureTrapContext(frame, &saved);
    saved.Cpsr &= 0xf0000000u;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    entry->ApcStatus = STATUS_ACCESS_VIOLATION;
    if (MmArm64UserCodeAddress(entry->Space, entry->ApcDispatcher) &&
        MmArm64UserCodeAddress(entry->Space, (ULONG_PTR)normal))
        entry->ApcStatus = MmArm64CopyUserBuffer(entry->Space, new_sp, &saved, sizeof(saved), TRUE);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!NT_SUCCESS(entry->ApcStatus)) {
        entry->ApcStatus = STATUS_ACCESS_VIOLATION;
        return;
    }
    frame->Sp = new_sp;
    frame->Pc = entry->ApcDispatcher;
    frame->X[0] = (ULONG_PTR)normal;
    frame->X[1] = (ULONG_PTR)context;
    frame->X[2] = (ULONG_PTR)argument1;
    frame->X[3] = (ULONG_PTR)argument2;
    frame->X[4] = new_sp;
    frame->X[30] = 0;
}

NTSTATUS KiArm64DeliverPendingUserApc(KARM64_USER_ENTRY *entry)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread->ApcState.UserApcPending || thread->KernelApcDisable ||
        thread->ApcState.KernelApcInProgress) return STATUS_SUCCESS;
    KIRQL old_irql;
    KeRaiseIrql(APC_LEVEL, &old_irql);
    ULONG_PTR initial_sp = entry->Frame.Sp;
    entry->ApcStatus = STATUS_SUCCESS;
    while (thread->ApcState.UserApcPending &&
           !IsListEmpty(&thread->ApcState.ApcListHead[UserMode]) &&
           initial_sp == entry->Frame.Sp && NT_SUCCESS(entry->ApcStatus))
        KiDeliverApc(UserMode, NULL, &entry->Frame);
    KeLowerIrql(old_irql);
    return entry->ApcStatus;
}

/* NtDelayExecution probes an aligned LARGE_INTEGER and delegates the
 * actual wait and alertability check to the executive dispatcher. */
static NTSTATUS delay_user(MM_ARM64_USER_SPACE *space, BOOLEAN alertable, ULONG_PTR address)
{
    if (address & (sizeof(ULONG)-1)) return STATUS_DATATYPE_MISALIGNMENT;
    LARGE_INTEGER interval;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64CopyUserBuffer(space, address, &interval, sizeof(interval), FALSE);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!NT_SUCCESS(status)) return STATUS_ACCESS_VIOLATION;
    return KeDelayExecutionThread(UserMode, alertable, &interval);
}

typedef struct {
    KARM64_USER_ENTRY *Entry;
    KTRAP_FRAME SavedFrame;
    ULONG64 SavedThreadPointer;
    ULONG64 SavedVectorKind;
    PVOID Output;
    ULONG Length;
    NTSTATUS Status;
    BOOLEAN Returned;
} KARM64_USER_CALLBACK;

static NTSTATUS run_user_loop(KARM64_USER_ENTRY *entry, KARM64_USER_CALLBACK *callback);

NTSTATUS KeUserModeCallback(ULONG api, PVOID input, ULONG length, PVOID *output, PULONG output_length)
{
    if (!output || !output_length || (length && !input)) return STATUS_INVALID_PARAMETER;
    *output = NULL;
    *output_length = 0;
    PKTHREAD thread = KeGetCurrentThread();
    PKTRAP_FRAME frame = thread->TrapFrame;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || thread->PreviousMode != UserMode || !thread->Teb || !frame ||
        frame->PreviousMode != UserMode || (frame->Esr >> 26) != 0x15)
        return STATUS_INVALID_DEVICE_STATE;
    KARM64_USER_ENTRY *entry = CONTAINING_RECORD(frame, KARM64_USER_ENTRY, Frame);
    if (entry->ExitRequested) return STATUS_THREAD_IS_TERMINATING;
    if (!entry->CallbackDispatcher) return STATUS_NOT_SUPPORTED;
    ULONG_PTR kernel_sp;
    __asm__ volatile("mov %0, sp" : "=r"(kernel_sp));
    /* This port keeps the caller's stack live during EL0 callouts. Bound
     * recursion and retain space above the guard for interrupts and services. */
    if (entry->CallbackDepth >= 4 || kernel_sp < (ULONG_PTR)thread->StackLimit + 3 * PAGE_SIZE)
        return STATUS_STACK_OVERFLOW;
    if (frame->Sp <= 0x10000 || frame->Sp > 0x80000000 || (frame->Sp & 15) ||
        length > frame->Sp - 0x10000)
        return STATUS_STACK_OVERFLOW;
    ULONG_PTR user_sp = (frame->Sp - length) & ~(ULONG_PTR)15;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = STATUS_ACCESS_VIOLATION;
    if (MmArm64UserCodeAddress(entry->Space, entry->CallbackDispatcher))
        status = length ? MmArm64CopyUserBuffer(entry->Space, user_sp, input, length, TRUE) : STATUS_SUCCESS;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    if (!NT_SUCCESS(status)) return STATUS_ACCESS_VIOLATION;

    KARM64_USER_CALLBACK callback = {
        .Entry = entry,
        .SavedFrame = *frame,
        .SavedThreadPointer = entry->UserThreadPointer,
        .SavedVectorKind = entry->VectorKind,
    };
    PVOID previous = thread->CallbackStack;
    thread->CallbackStack = &callback;
    thread->TrapFrame = NULL;
    thread->PreviousMode = KernelMode;
    ++entry->CallbackDepth;
    frame->Sp = user_sp;
    frame->Pc = entry->CallbackDispatcher;
    frame->X[0] = api;
    frame->X[1] = length ? user_sp : 0;
    frame->X[2] = length;
    frame->X[30] = 0;
    status = run_user_loop(entry, &callback);
    --entry->CallbackDepth;
    thread->CallbackStack = previous;
    thread->TrapFrame = frame;
    thread->PreviousMode = UserMode;
    *frame = callback.SavedFrame;
    entry->UserThreadPointer = callback.SavedThreadPointer;
    entry->VectorKind = callback.SavedVectorKind;
    if (!callback.Returned) {
        entry->ExitRequested = TRUE;
        entry->ExitStatus = status;
    } else {
        *output = callback.Output;
        *output_length = callback.Length;
    }
    return status;
}

static NTSTATUS callback_return(KARM64_USER_ENTRY *entry)
{
    KARM64_USER_CALLBACK *callback = KeGetCurrentThread()->CallbackStack;
    if (!callback || callback->Entry != entry) return STATUS_NO_CALLBACK_ACTIVE;
    const KTRAP_FRAME *frame = &entry->Frame;
    ULONG_PTR output = frame->X[0];
    ULONG length = (ULONG)frame->X[1];
    if (length) {
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        NTSTATUS status = MmArm64ProbeUserRead(entry->Space, output, length);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        if (!NT_SUCCESS(status)) return STATUS_ACCESS_VIOLATION;
    }
    callback->Output = (PVOID)output;
    callback->Length = length;
    callback->Status = (NTSTATUS)frame->X[2];
    callback->Returned = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS dispatch_win32_service(MM_ARM64_USER_SPACE *space, const KTRAP_FRAME *frame)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (thread->ServiceTable != &KeServiceDescriptorTableShadow[0] || thread->TrapFrame != frame)
        return STATUS_INVALID_SYSTEM_SERVICE;
    const KSERVICE_TABLE_DESCRIPTOR *table = &KeServiceDescriptorTableShadow[WIN32K_SERVICE_INDEX];
    ULONG index = (ULONG)frame->X[8] & SERVICE_NUMBER_MASK;
    if (!table->Base || index >= table->Limit || !table->Base[index])
        return STATUS_INVALID_SYSTEM_SERVICE;
    ULONG_PTR tail[3] = {0};
    ULONG arguments = table->Number ? table->Number[index] : 8;
    if (arguments > 11) return STATUS_INVALID_SYSTEM_SERVICE;
    if (arguments > 8) {
        ULONG64 daif;
        __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
        NTSTATUS status = MmArm64CopyUserBuffer(space, frame->Sp, tail, (arguments - 8) * sizeof(tail[0]), FALSE);
        __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
        if (!NT_SUCCESS(status)) return STATUS_ACCESS_VIOLATION;
    }
    PKARM64_WIN32_SERVICE service = (PKARM64_WIN32_SERVICE)table->Base[index];
    KPROCESSOR_MODE previous = thread->PreviousMode;
    thread->PreviousMode = UserMode;
    NTSTATUS status = service(frame->X[0], frame->X[1], frame->X[2], frame->X[3],
                              frame->X[4], frame->X[5], frame->X[6], frame->X[7], tail[0], tail[1], tail[2]);
    thread->PreviousMode = previous;
    return status;
}

NTSTATUS KiArm64UserService(MM_ARM64_USER_SPACE *space, KARM64_USER_ENTRY *entry)
{
    if (!space || !entry || entry->VectorKind || entry->Frame.PreviousMode != UserMode ||
        (entry->Frame.Cpsr & 0x1f) || (entry->Frame.Esr >> 26) != 0x15 || (entry->Frame.Esr & 0xffff))
        return STATUS_INVALID_PARAMETER;
    KTRAP_FRAME *frame = &entry->Frame;
    NTSTATUS status = STATUS_INVALID_SYSTEM_SERVICE;
    if (frame->X[8] == XP_SERVICE_CALLBACK_RETURN) {
        status = callback_return(entry);
        if (NT_SUCCESS(status)) return STATUS_SUCCESS;
    } else if (frame->X[8] == XP_SERVICE_TERMINATE_THREAD && entry->Space) {
        HANDLE target = (HANDLE)frame->X[0];
        if (target == NtCurrentThread() || (!target && PsGetCurrentProcess()->ActiveThreads > 1)) {
            entry->ExitRequested = TRUE;
            entry->ExitStatus = (NTSTATUS)frame->X[1];
            return STATUS_SUCCESS;
        }
        status = target ? STATUS_NOT_SUPPORTED : STATUS_CANT_TERMINATE_SELF;
    } else if ((frame->X[8] >> 12) == WIN32K_SERVICE_INDEX) {
        status = dispatch_win32_service(space, frame);
    } else if (frame->X[8] == XP_SERVICE_CLOSE && entry->Space) {
        status = ExpArm64CloseUserHandle((HANDLE)frame->X[0], NULL);
    } else if (frame->X[8] == XP_SERVICE_CONTINUE) {
        status = continue_user(space, frame);
        if (NT_SUCCESS(status)) return STATUS_SUCCESS;
    } else if (frame->X[8] == XP_SERVICE_TEST_ALERT) {
        status = KeTestAlertThread(UserMode) ? STATUS_ALERTED : STATUS_SUCCESS;
    } else if (frame->X[8] == XP_SERVICE_DELAY_EXECUTION) {
        status = delay_user(space, (BOOLEAN)frame->X[0], frame->X[1]);
    } else if (frame->X[8] == XP_SERVICE_QUERY_PERFORMANCE_COUNTER) {
        LARGE_INTEGER counter, frequency;
        status = NtQueryPerformanceCounter(&counter, &frequency);
        if (NT_SUCCESS(status)) {
            ULONG64 daif;
            __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
            status = MmArm64CopyUserBuffer(space, frame->X[0], &counter, sizeof(counter), TRUE);
            if (NT_SUCCESS(status) && frame->X[1])
                status = MmArm64CopyUserBuffer(space, frame->X[1], &frequency, sizeof(frequency), TRUE);
            __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
            if (!NT_SUCCESS(status)) status = STATUS_ACCESS_VIOLATION;
        }
    }
    frame->X[0] = (ULONG)status;
    return STATUS_SUCCESS;
}

/* A callback runs the same user return path as a native thread. On thread
 * exit each pending kernel caller unwinds before the ETHREAD is retired. */
static NTSTATUS run_user_loop(KARM64_USER_ENTRY *entry, KARM64_USER_CALLBACK *callback)
{
    PKTHREAD thread = KeGetCurrentThread();
    KTRAP_FRAME *frame = &entry->Frame;
    for (;;) {
        NTSTATUS status = KiArm64DeliverPendingUserApc(entry);
        if (!NT_SUCCESS(status)) return status;
        status = KiArm64RunUser(entry->Space, entry);
        if (!NT_SUCCESS(status)) return status;
        if (entry->VectorKind == 1) continue;
        if (!entry->VectorKind && (frame->Esr >> 26) == 0x15 && !(frame->Esr & 0xffff)) {
            PKTRAP_FRAME previous = thread->TrapFrame;
            thread->TrapFrame = frame;
            status = KiArm64UserService(entry->Space, entry);
            thread->TrapFrame = previous;
            if (entry->ExitRequested) return entry->ExitStatus;
            if (!NT_SUCCESS(status)) return status;
            if (callback && callback->Returned) return callback->Status;
        } else {
            status = KiArm64DispatchUserException(entry->Space, entry, entry->ExceptionDispatcher);
            if (!NT_SUCCESS(status)) return status;
        }
    }
}

NTSTATUS KiArm64RunUserThread(KARM64_USER_ENTRY *entry)
{
    return run_user_loop(entry, NULL);
}
