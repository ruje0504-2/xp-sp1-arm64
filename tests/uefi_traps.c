/* Exercise the real ARM64 vectors/PCR while retaining the surrounding UEFI
 * environment. Only this test installs an early assertion handler. */
#include <ntos.h>
#include "uefi_test.h"

extern VOID XpTrapFixture(PCONTEXT Witness);
extern const UCHAR XpExpectedBrk[];
extern const UCHAR XpNoFpBrk[];

static KPCR processor;
static KSPIN_LOCK shared_locks[16];
static CONTEXT witness;
static BOOLEAN expect_fp;

static BOOLEAN trap_handler(PKTRAP_FRAME frame, ULONG vector_type)
{
    CHECK(vector_type == 0);
    CHECK(KeGetPcr() == &processor);
    CHECK(KeGetCurrentPrcb() == &processor.Prcb);
    CHECK(KeGetCurrentProcessorNumber() == 0);
    CHECK(KeGetCurrentThread() == NULL);
    CHECK(frame->PreviousMode == KernelMode && frame->PreviousIrql == HIGH_LEVEL);
    CHECK(frame->ExceptionActive == 1 && frame->Padding == 0);
    CHECK((frame->Cpsr & 0x1f) == 5);
    CHECK(frame->Sp == (ULONG_PTR)(frame + 1));
    CHECK(((frame->Esr >> 26) & 63) == 0x3c);
    CHECK(frame->FpStateSaved == expect_fp);
    if (expect_fp) {
        CHECK(frame->Pc == (ULONG_PTR)XpExpectedBrk && (frame->Esr & 0xffff) == 0x1234);
        CHECK(frame->X[0] == (ULONG_PTR)&witness);
        for (ULONG i = 1; i < 31; ++i) {
            if (i == 18) CHECK(frame->X[i] == (ULONG_PTR)&processor);
            else CHECK(frame->X[i] == 0x1000 + i);
        }
        CHECK((frame->Cpsr & 0xf0000000) == 0x60000000);
        for (ULONG i = 0; i < 32; ++i) {
            const UCHAR *bytes = (const UCHAR *)&frame->V[i];
            for (ULONG j = 0; j < 16; ++j) CHECK(bytes[j] == i + 16);
        }
        frame->X[7] = 0x7007;
        frame->V[0].Low = 0x0123456789abcdefULL;
    } else {
        CHECK(frame->Pc == (ULONG_PTR)XpNoFpBrk && (frame->Esr & 0xffff) == 0x5678);
        for (ULONG i = 0; i < 32; ++i)
            CHECK(frame->V[i].Low == 0 && frame->V[i].High == 0);
        CHECK(frame->Fpcr == 0 && frame->Fpsr == 0);
    }
    frame->Pc += 4; /* This control explicitly handles only its two BRKs. */
    return TRUE;
}

void XpCheckKernelTraps(void)
{
    ULONG64 level, saved_daif, saved_vbar, saved_tpidr, saved_x18, saved_cpacr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    CHECK(level == 4);
    XpTestReport("XP ARM64 EL1 vector control entered\n");
    CHECK(KiArm64InitializePcr(&processor, 0, shared_locks, trap_handler) == STATUS_SUCCESS);
    CHECK(processor.Self == &processor && processor.Prcb.CurrentThread == NULL);
    CHECK(processor.Prcb.SetMember == 1 && processor.Prcb.NotSetMember == ~(KAFFINITY)1);
    for (ULONG i = 0; i < 16; ++i)
        CHECK(processor.Prcb.LockQueue[i].Lock == &shared_locks[i]);
    CHECK(!((ULONG_PTR)KiArm64VectorTable & 2047));
    __asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(saved_vbar));
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(saved_tpidr));
    __asm__ volatile("mov %0, x18" : "=r"(saved_x18));
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(saved_cpacr));
    __asm__ volatile("msr daifset, #15" ::: "memory");
    __asm__ volatile("msr tpidr_el1, %0\nmov x18, %0\nmsr vbar_el1, %1\nisb"
                     :: "r"(&processor), "r"(KiArm64VectorTable) : "memory");
    ULONG64 cpacr = saved_cpacr | (3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(cpacr) : "memory");
    expect_fp = TRUE;
    XpTrapFixture(&witness);
    CHECK(witness.X0 == (ULONG_PTR)&witness && witness.X7 == 0x7007);
    for (ULONG i = 1; i < 31; ++i) {
        if (i == 7) continue;
        if (i == 18) CHECK(witness.X[i] == (ULONG_PTR)&processor);
        else CHECK(witness.X[i] == 0x1000 + i);
    }
    CHECK(witness.Cpsr == 0x60000000);
    CHECK(witness.V[0].Low == 0x0123456789abcdefULL);
    for (ULONG i = 0; i < 32; ++i) {
        const UCHAR *bytes = (const UCHAR *)&witness.V[i];
        for (ULONG j = i == 0 ? 8 : 0; j < 16; ++j) CHECK(bytes[j] == i + 16);
    }
    expect_fp = FALSE;
    cpacr = saved_cpacr & ~(3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(cpacr) : "memory");
    __asm__ volatile(".globl XpNoFpBrk\nXpNoFpBrk:\nbrk #0x5678" ::: "memory");
    CHECK(processor.TrapCount == 2 && processor.LastTrapPc == (ULONG_PTR)XpNoFpBrk);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(saved_cpacr) : "memory");
    __asm__ volatile("msr vbar_el1, %0\nmsr tpidr_el1, %1\nmov x18, %2\nisb"
                     :: "r"(saved_vbar), "r"(saved_tpidr), "r"(saved_x18) : "memory");
    __asm__ volatile("msr daif, %0\nisb" :: "r"(saved_daif) : "memory");
    ULONG64 observed;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(observed)); CHECK(observed == saved_vbar);
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(observed)); CHECK(observed == saved_tpidr);
    __asm__ volatile("mov %0, x18" : "=r"(observed)); CHECK(observed == saved_x18);
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(observed)); CHECK(observed == saved_cpacr);
    XpTestReport("PASS: XP ARM64 EL1 BRK entry/return, PCR, integer/SIMD frames, FP-disabled path and firmware restore\n");
}
