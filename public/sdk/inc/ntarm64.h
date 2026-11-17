/* Initial ARM64 continuation-context ABI, matching Windows ARM64 CONTEXT.
 * Trap entry, exception dispatch and restore/unwind are separate work. */
#ifndef XP_NTARM64_H
#define XP_NTARM64_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <excpt.h>
#include <ntdef.h>
#endif

#define CONTEXT_ARM64 0x00400000u
#define CONTEXT_CONTROL (CONTEXT_ARM64 | 1u)
#define CONTEXT_INTEGER (CONTEXT_ARM64 | 2u)
#define CONTEXT_FLOATING_POINT (CONTEXT_ARM64 | 4u)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_ARM64 | 8u)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)

/* ARM64 .pdata entries use a start RVA and packed/unpacked unwind word.
 * The low two bits of UnwindData select the encoding. */
typedef struct _RUNTIME_FUNCTION {
    uint32_t BeginAddress;
    uint32_t UnwindData;
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
_Static_assert(sizeof(RUNTIME_FUNCTION) == 8, "ARM64 runtime function");

typedef struct { uint64_t Low; int64_t High; } ARM64_NT_NEON128;
typedef struct __attribute__((aligned(16))) _CONTEXT {
    uint32_t ContextFlags;
    uint32_t Cpsr;
    union {
        struct {
            uint64_t X0;
            uint64_t X1;
            uint64_t X2;
            uint64_t X3;
            uint64_t X4;
            uint64_t X5;
            uint64_t X6;
            uint64_t X7;
            uint64_t X8;
            uint64_t X9;
            uint64_t X10;
            uint64_t X11;
            uint64_t X12;
            uint64_t X13;
            uint64_t X14;
            uint64_t X15;
            uint64_t X16;
            uint64_t X17;
            uint64_t X18;
            uint64_t X19;
            uint64_t X20;
            uint64_t X21;
            uint64_t X22;
            uint64_t X23;
            uint64_t X24;
            uint64_t X25;
            uint64_t X26;
            uint64_t X27;
            uint64_t X28;
            uint64_t Fp;
            uint64_t Lr;
        };
        uint64_t X[31];
    };
    uint64_t Sp;
    uint64_t Pc;
    ARM64_NT_NEON128 V[32];
    uint32_t Fpcr;
    uint32_t Fpsr;
    uint32_t Bcr[8];
    uint64_t Bvr[8];
    uint32_t Wcr[2];
    uint64_t Wvr[2];
} CONTEXT, *PCONTEXT;

_Static_assert(sizeof(CONTEXT) == 0x390, "ARM64 context size");
_Static_assert(_Alignof(CONTEXT) == 16, "ARM64 context alignment");
_Static_assert(offsetof(CONTEXT, X) == 0x008, "Integer registers");
_Static_assert(offsetof(CONTEXT, Sp) == 0x100, "Stack pointer");
_Static_assert(offsetof(CONTEXT, Pc) == 0x108, "Continuation address");
_Static_assert(offsetof(CONTEXT, V) == 0x110, "SIMD registers");
_Static_assert(offsetof(CONTEXT, Fpcr) == 0x310, "FP control");
_Static_assert(offsetof(CONTEXT, Bcr) == 0x318, "Debug registers");

typedef struct _KNONVOLATILE_CONTEXT_POINTERS {
    uint64_t *X19;
    uint64_t *X20;
    uint64_t *X21;
    uint64_t *X22;
    uint64_t *X23;
    uint64_t *X24;
    uint64_t *X25;
    uint64_t *X26;
    uint64_t *X27;
    uint64_t *X28;
    uint64_t *Fp;
    uint64_t *Lr;
    uint64_t *D8;
    uint64_t *D9;
    uint64_t *D10;
    uint64_t *D11;
    uint64_t *D12;
    uint64_t *D13;
    uint64_t *D14;
    uint64_t *D15;
} KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;

/* Capture the calling continuation at EL0. Cpsr contains NZCV, with EL0t
 * control bits. Debug state is zeroed and excluded from ContextFlags.
 * Privileged trap state must come from exception-entry assembly. */
#ifdef _WIN32
NTSYSAPI VOID NTAPI RtlCaptureContext(PCONTEXT Context);
#else
void RtlCaptureContext(PCONTEXT Context);
#endif
#endif
