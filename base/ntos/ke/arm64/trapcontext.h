#ifndef XP_ARM64_TRAPCONTEXT_H
#define XP_ARM64_TRAPCONTEXT_H
#include <ntos.h>

static inline VOID KiArm64CaptureTrapContext(const KTRAP_FRAME *frame, PCONTEXT context)
{
    RtlZeroMemory(context, sizeof(*context));
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context->Cpsr = (ULONG)frame->Cpsr;
    context->Sp = frame->Sp;
    context->Pc = frame->Pc;
    RtlCopyMemory(context->X, frame->X, sizeof(context->X));
    if (frame->FpStateSaved) {
        context->ContextFlags |= CONTEXT_FLOATING_POINT;
        RtlCopyMemory(context->V, frame->V, sizeof(context->V));
        context->Fpcr = frame->Fpcr;
        context->Fpsr = frame->Fpsr;
    }
}
#endif
