#include "seh.h"
#include "../../ke/arm64/trapcontext.h"
#define MAX_SEH_FRAMES 16
extern const UCHAR RtlArm64RaiseStatusTrap[];
typedef struct {
    CONTEXT Before;
    RTL_ARM64_UNWIND_RESULT Unwind;
} SEH_FRAME;
typedef struct {
    const RTL_ARM64_UNWIND_IMAGE *Image;
    ULONG_PTR Low, High;
    ULONG64 EntrySp;
    ULONG EntryCpsr;
    BOOLEAN Active, Unwinding;
    ULONG Count, Current;
    NTSTATUS LastStatus;
    EXCEPTION_RECORD Record;
    CONTEXT Fault, Transfer;
    SEH_FRAME Frames[MAX_SEH_FRAMES];
    DISPATCHER_CONTEXT Dispatch;
} SEH_STATE;
/* Initial NT_UP contract. Nested/collided dispatch and SMP are not implemented. */
static SEH_STATE state;

NTSTATUS RtlArm64SehLastStatus(VOID) { return state.LastStatus; }
__declspec(noreturn) static VOID fail(NTSTATUS status)
{
    state.LastStatus = status;
    __asm__ volatile("brk #0x7fe" ::: "memory");
    for (;;) __asm__ volatile("wfe");
}
static BOOLEAN span(ULONG_PTR address, ULONG_PTR bytes)
{
    ULONG_PTR base = state.Image->Base, end = base + state.Image->Size;
    return address >= base && address <= end && bytes <= end - address;
}
static BOOLEAN scope_valid(const SEH_FRAME *frame)
{
    const RTL_ARM64_UNWIND_RESULT *u = &frame->Unwind;
    if (u->FrameSp != frame->Before.Sp) return FALSE; /* Dynamic SP not yet supported. */
    PSCOPE_TABLE scope = u->HandlerData;
    if (!span((ULONG_PTR)scope, sizeof(ULONG)) || scope->Count > 1024 ||
        !span((ULONG_PTR)scope, 4 + (ULONG_PTR)scope->Count * 16)) return FALSE;
    for (ULONG i = 0; i < scope->Count; ++i) {
        ULONG64 begin = state.Image->Base + scope->ScopeRecord[i].BeginAddress;
        ULONG64 end = state.Image->Base + scope->ScopeRecord[i].EndAddress;
        ULONG handler = scope->ScopeRecord[i].HandlerAddress, jump = scope->ScopeRecord[i].JumpTarget;
        if (begin < u->FunctionBegin || end > u->FunctionEnd || begin >= end ||
            ((begin | end | jump) & 3) || !handler ||
            (handler != 1 && (!span(state.Image->Base + handler, 4) || (handler & 3))))
            return FALSE;
        if (jump && (state.Image->Base + jump < u->FunctionBegin ||
                     state.Image->Base + jump >= u->FunctionEnd)) return FALSE;
        if (!jump && handler == 1) return FALSE;
    }
    return TRUE;
}
static VOID dispatch_for(ULONG index, ULONG64 target, PDISPATCHER_CONTEXT out)
{
    SEH_FRAME *frame = &state.Frames[index];
    RtlZeroMemory(out, sizeof(*out));
    out->ControlPc = frame->Unwind.ControlPc;
    out->ImageBase = state.Image->Base;
    out->EstablisherFrame = frame->Unwind.EstablisherFrame;
    out->TargetIp = target;
    out->ContextRecord = &frame->Before;
    out->NonVolatileContext = &frame->Before;
    out->HandlerData = frame->Unwind.HandlerData;
}
static BOOLEAN return_valid(const CONTEXT *c)
{
    ULONG64 cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    return (c->Cpsr & 31) == 5 && (c->Cpsr & ~0xf0000000u) == (state.EntryCpsr & ~0xf0000000u) &&
        !(c->Pc & 3) && span(c->Pc, 4) && !(c->Sp & 15) &&
        c->Sp >= state.EntrySp && c->Sp <= state.High && c->X18 == (ULONG_PTR)KeGetPcr() &&
        (!(c->ContextFlags & 4) || (cpacr & (1ULL << 20)));
}
__declspec(noreturn) VOID RtlUnwindEx(PVOID target_frame, PVOID target_ip,
    PEXCEPTION_RECORD record, PVOID value, PCONTEXT context, PVOID history)
{
    if (!state.Active || state.Unwinding || state.Current >= state.Count || history ||
        record != &state.Record || context != &state.Frames[state.Current].Before ||
        (ULONG_PTR)target_frame != state.Frames[state.Current].Unwind.EstablisherFrame)
        fail(STATUS_INVALID_DISPOSITION);
    ULONG target = state.Current;
    SEH_FRAME *frame = &state.Frames[target];
    PSCOPE_TABLE scope = frame->Unwind.HandlerData;
    BOOLEAN found = FALSE;
    for (ULONG i = 0; i < scope->Count; ++i)
        if (scope->ScopeRecord[i].JumpTarget &&
            state.Image->Base + scope->ScopeRecord[i].JumpTarget == (ULONG_PTR)target_ip &&
            frame->Unwind.ControlPc >= state.Image->Base + scope->ScopeRecord[i].BeginAddress &&
            frame->Unwind.ControlPc < state.Image->Base + scope->ScopeRecord[i].EndAddress)
            found = TRUE;
    if (!found) fail(STATUS_INVALID_UNWIND_TARGET);
    state.Unwinding = TRUE;
    ULONG original_flags = state.Record.ExceptionFlags;
    for (ULONG i = 0; i <= target; ++i) {
        SEH_FRAME *f = &state.Frames[i];
        if (!f->Unwind.Handler) continue;
        DISPATCHER_CONTEXT dispatch;
        dispatch_for(i, (ULONG_PTR)target_ip, &dispatch);
        state.Record.ExceptionFlags = (original_flags & EXCEPTION_NONCONTINUABLE) |
            EXCEPTION_UNWINDING | (i == target ? EXCEPTION_TARGET_UNWIND : 0);
        EXCEPTION_DISPOSITION disposition = __C_specific_handler(&state.Record,
            (PVOID)dispatch.EstablisherFrame, &state.Fault, &dispatch);
        if (disposition != ExceptionContinueSearch) fail(STATUS_INVALID_DISPOSITION);
    }
    state.Transfer = frame->Before;
    state.Transfer.Pc = (ULONG_PTR)target_ip;
    state.Transfer.X0 = (ULONG_PTR)value;
    state.Transfer.Cpsr = state.Fault.Cpsr;
    if (!return_valid(&state.Transfer)) fail(STATUS_INVALID_UNWIND_TARGET);
    state.Active = state.Unwinding = FALSE;
    state.LastStatus = STATUS_SUCCESS;
    RtlArm64ResumeException(&state.Transfer);
}
BOOLEAN RtlArm64DispatchScopeException(const RTL_ARM64_UNWIND_IMAGE *image,
    PKTRAP_FRAME frame, ULONG_PTR low, ULONG_PTR high)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (state.Active) { state.LastStatus = STATUS_NOT_SUPPORTED; return FALSE; }
    if (KeNumberProcessors != 1 || (daif & 0x3c0) != 0x3c0 ||
        !image || !frame || (frame->Cpsr & 31) != 5 || low >= high ||
        frame->Sp < low || frame->Sp > high) return FALSE;
    ULONG ec = (ULONG)(frame->Esr >> 26) & 63, fsc = (ULONG)frame->Esr & 63;
    BOOLEAN software = ec == 0x3c && (frame->Esr & 65535) == 0x7f0 &&
        frame->Pc == (ULONG_PTR)RtlArm64RaiseStatusTrap;
    if (!software && (ec != 0x25 || (frame->Esr & (1ULL << 10)) ||
        ((fsc & 0x3c) != 4 && (fsc & 0x3c) != 0xc))) return FALSE;
    RtlZeroMemory(&state, sizeof(state));
    state.Image = image; state.Low = low; state.High = high; state.Active = TRUE;
    state.EntrySp = frame->Sp; state.EntryCpsr = (ULONG)frame->Cpsr;
    state.Record.ExceptionCode = software ? (NTSTATUS)frame->X[0] : STATUS_ACCESS_VIOLATION;
    state.Record.ExceptionFlags = software ? EXCEPTION_NONCONTINUABLE : 0;
    state.Record.ExceptionAddress = (PVOID)(ULONG_PTR)frame->Pc;
    if (!software) {
        state.Record.NumberParameters = 2;
        state.Record.ExceptionInformation[0] = (frame->Esr >> 6) & 1;
        state.Record.ExceptionInformation[1] = frame->Far;
    }
    KiArm64CaptureTrapContext(frame, &state.Fault);
    CONTEXT cursor = state.Fault;
    for (ULONG i = 0; i < MAX_SEH_FRAMES; ++i) {
        SEH_FRAME *f = &state.Frames[i];
        f->Before = cursor;
        state.LastStatus = RtlArm64UnwindOne(image, &cursor, low, high, &f->Unwind);
        if (!NT_SUCCESS(state.LastStatus)) break;
        state.Count = i + 1; state.Current = i;
        if (f->Unwind.Handler) {
            if (f->Unwind.Handler != (PEXCEPTION_ROUTINE)__C_specific_handler || !scope_valid(f)) {
                state.LastStatus = STATUS_NOT_SUPPORTED; break;
            }
            dispatch_for(i, 0, &state.Dispatch);
            EXCEPTION_DISPOSITION d = __C_specific_handler(&state.Record,
                (PVOID)state.Dispatch.EstablisherFrame, &state.Fault, &state.Dispatch);
            if (d == ExceptionContinueExecution) {
                if (state.Record.ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                    state.LastStatus = STATUS_NONCONTINUABLE_EXCEPTION; break;
                }
                /* Continue-execution uses the normal vector return, preserving
                 * its exception stack invariant. Nonlocal targets use unwind. */
                if (!return_valid(&state.Fault) || state.Fault.Sp != frame->Sp) {
                    state.LastStatus = STATUS_INVALID_DISPOSITION; break;
                }
                for (ULONG j = 0; j < 31; ++j) frame->X[j] = state.Fault.X[j];
                for (ULONG j = 0; j < 32; ++j) frame->V[j] = state.Fault.V[j];
                frame->Pc = state.Fault.Pc; frame->Cpsr = state.Fault.Cpsr;
                frame->Fpcr = state.Fault.Fpcr; frame->Fpsr = state.Fault.Fpsr;
                state.Active = FALSE; state.LastStatus = STATUS_SUCCESS;
                return TRUE;
            }
            if (d != ExceptionContinueSearch) { state.LastStatus = STATUS_INVALID_DISPOSITION; break; }
        }
        if (!cursor.Pc || (cursor.Sp == f->Before.Sp && cursor.Pc == f->Before.Pc)) break;
    }
    state.Active = FALSE;
    if (NT_SUCCESS(state.LastStatus)) state.LastStatus = STATUS_UNHANDLED_EXCEPTION;
    return FALSE;
}
