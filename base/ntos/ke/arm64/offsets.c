#include "context.h"
#include <stddef.h>
#pragma section(".koffs", read)
__declspec(allocate(".koffs")) const ULONG KiArm64Offsets[] = {
    sizeof(KTRAP_FRAME), /* TF_SIZE */
    offsetof(KTRAP_FRAME, X), /* TF_X */
    offsetof(KTRAP_FRAME, Sp), /* TF_SP */
    offsetof(KTRAP_FRAME, Pc), /* TF_PC */
    offsetof(KTRAP_FRAME, Cpsr), /* TF_CPSR */
    offsetof(KTRAP_FRAME, Esr), /* TF_ESR */
    offsetof(KTRAP_FRAME, Far), /* TF_FAR */
    offsetof(KTRAP_FRAME, V), /* TF_V */
    offsetof(KTRAP_FRAME, Fpcr), /* TF_FPCR */
    offsetof(KTRAP_FRAME, Fpsr), /* TF_FPSR */
    offsetof(KTRAP_FRAME, PreviousIrql), /* TF_IRQL */
    offsetof(KTRAP_FRAME, PreviousMode), /* TF_MODE */
    offsetof(KTRAP_FRAME, ExceptionActive), /* TF_ACTIVE */
    offsetof(KTRAP_FRAME, FpStateSaved), /* TF_FP_SAVED */
    offsetof(KTRAP_FRAME, Padding), /* TF_PADDING */
    offsetof(KPCR, Irql), /* PCR_IRQL */
    sizeof(KARM64_SWITCH_FRAME), /* SW_SIZE */
    offsetof(KARM64_SWITCH_FRAME, X19_X28), /* SW_X19 */
    offsetof(KARM64_SWITCH_FRAME, Fp), /* SW_FP */
    offsetof(KARM64_SWITCH_FRAME, D8_D15), /* SW_D8 */
    offsetof(KARM64_SWITCH_FRAME, Fpcr), /* SW_FPCR */
    offsetof(KARM64_SWITCH_FRAME, HasFp), /* SW_HAS_FP */
    offsetof(KARM64_SWITCH_FRAME, Reserved), /* SW_RESERVED */
    offsetof(KTHREAD, KernelStack), /* THREAD_KSTACK */
    offsetof(KPCR, CurrentPrcb), /* PCR_PRCB */
    offsetof(KPRCB, CurrentThread), /* PRCB_THREAD */
    offsetof(CONTEXT, ContextFlags), /* CTX_FLAGS */
    offsetof(CONTEXT, Cpsr), /* CTX_CPSR */
    offsetof(CONTEXT, X), /* CTX_X */
    offsetof(CONTEXT, Sp), /* CTX_SP */
    offsetof(CONTEXT, Pc), /* CTX_PC */
    offsetof(CONTEXT, Fp), /* CTX_FP */
    offsetof(CONTEXT, V), /* CTX_V */
    offsetof(CONTEXT, Fpcr), /* CTX_FPCR */
    offsetof(CONTEXT, Fpsr), /* CTX_FPSR */
};
