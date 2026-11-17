#include <ntarm64.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern uintptr_t XpCaptureFixture(PCONTEXT context);
extern void XpCaptureResume(void);

int main(void)
{
    struct {
        unsigned char before[16];
        CONTEXT context;
        unsigned char after[16];
    } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    uint64_t fpcr, fpsr, reserved_x18;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    __asm__ volatile("mov %0, x18" : "=r"(reserved_x18));
    uintptr_t sp = XpCaptureFixture(&guarded.context);
    CONTEXT *c = &guarded.context;
    assert(c->ContextFlags == CONTEXT_FULL);
    assert(c->X0 == (uintptr_t)c);
    for (unsigned i = 1; i < 30; ++i) {
        if (i == 18) assert(c->X[i] == reserved_x18);
        else assert(c->X[i] == 0x1000 + i);
    }
    assert(c->Pc == (uintptr_t)&XpCaptureResume && c->Lr == c->Pc);
    assert(c->Sp == sp && !(sp & 15));
    assert(c->Cpsr == 0x60000000);
    assert(c->Fpcr == fpcr && c->Fpsr == fpsr);
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned char *bytes = (const unsigned char *)&c->V[i];
        for (unsigned j = 0; j < 16; ++j) assert(bytes[j] == i);
    }
    const unsigned char *debug = (const unsigned char *)c + offsetof(CONTEXT, Bcr);
    for (size_t i = 0; i < sizeof(*c) - offsetof(CONTEXT, Bcr); ++i) assert(debug[i] == 0);
    for (unsigned i = 0; i < 16; ++i) {
        assert(guarded.before[i] == 0xa5);
        assert(guarded.after[i] == 0xa5);
    }
    puts("PASS: real ARM64 context capture, integer/SIMD registers, NZCV, FP status, SP/PC and bounds");
    return 0;
}
