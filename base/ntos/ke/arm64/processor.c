/* ARM64 processor-local state and the early exception-dispatch boundary. */
#include <ntos.h>

NTSTATUS KiArm64InitializePcr(PKPCR pcr, ULONG number, PKSPIN_LOCK locks,
                              PKARM64_TRAP_HANDLER handler)
{
    if (!pcr || !locks || !handler || number >= 64 ||
        ((ULONG_PTR)pcr & 63) || ((ULONG_PTR)locks & (sizeof(KSPIN_LOCK) - 1)))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(pcr, sizeof(*pcr));
    pcr->Self = pcr;
    pcr->CurrentPrcb = &pcr->Prcb;
    pcr->Number = (UCHAR)number;
    pcr->MajorVersion = 1;
    pcr->Irql = HIGH_LEVEL; /* Initial high IRQL; lowering requires the HAL priority path. */
    pcr->TrapHandler = handler;
    pcr->Prcb.MajorVersion = 1;
    pcr->Prcb.Number = (CCHAR)number;
    pcr->Prcb.SetMember = (KAFFINITY)1 << number;
    pcr->Prcb.NotSetMember = ~pcr->Prcb.SetMember;
    for (ULONG i = 0; i < 16; ++i)
        pcr->Prcb.LockQueue[i].Lock = &locks[i];
    return STATUS_SUCCESS;
}

BOOLEAN KiArm64TrapDispatch(PKTRAP_FRAME frame, ULONG vector_type)
{
    PKPCR pcr = KeGetPcr();
    ++pcr->TrapCount;
    pcr->LastTrapPc = frame->Pc;
    pcr->LastTrapEsr = frame->Esr;
    pcr->LastVectorType = vector_type;
    pcr->LastTrapEsrValid = vector_type == 0;
    frame->ExceptionActive = vector_type == 0 ? 1 : 0;
    if (pcr->Self != pcr || pcr->CurrentPrcb != &pcr->Prcb || !pcr->TrapHandler)
        return FALSE;
    ULONG64 original_mode = frame->Cpsr & 0x1f;
    if (original_mode != 0 && original_mode != 4 && original_mode != 5)
        return FALSE;
    if (!pcr->TrapHandler(frame, vector_type))
        return FALSE;
    /* A normal trap return preserves privilege and the kernel exception
     * stack. Switching kernel stacks belongs to the context-switch path. */
    if ((frame->Cpsr & 0x1f) != original_mode || (frame->Pc & 3) ||
        (frame->Sp & 15) || pcr->Irql != frame->PreviousIrql)
        return FALSE;
    if (original_mode == 5 && frame->Sp != (ULONG_PTR)(frame + 1))
        return FALSE;
    return TRUE;
}
