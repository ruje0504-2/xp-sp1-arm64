/* Real EL1 trap state -> bounded PE unwind. This does not yet dispatch
 * C language handlers or resume an exception handler via RtlUnwindEx. */
#include "../base/boot/arm64/boot.h"
#include "../base/ntos/rtl/arm64/unwind.h"
#include "uefi_test.h"
extern IMAGE_DOS_HEADER __ImageBase;
extern VOID XpUnwindFullFixture(PCONTEXT Expected);
extern VOID XpUnwindPackedFixture(PCONTEXT Expected);
extern VOID XpUnwindPackedBody(VOID);
static RTL_ARM64_UNWIND_IMAGE image;
static CONTEXT expected, input, output, unchanged;
static RTL_ARM64_UNWIND_RESULT result;
static ULONG_PTR stack_low, stack_high;
static ULONG seen;
static BOOLEAN enabled;
static DECLSPEC_ALIGN(16) UCHAR malformed_image[4096];
static BOOLEAN equal_bytes(const VOID *a, const VOID *b, ULONG size)
{
    const UCHAR *x = a, *y = b;
    for (ULONG i = 0; i < size; ++i) if (x[i] != y[i]) return FALSE;
    return TRUE;
}
/* Rejection-only parser controls, never presented as hardware execution. */
static VOID malformed_cases(VOID)
{
    RtlZeroMemory(malformed_image, sizeof(malformed_image));
    IMAGE_DOS_HEADER *dos = (PVOID)malformed_image;
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 64;
    IMAGE_NT_HEADERS64 *nt = (PVOID)(malformed_image + 64);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_ARM64;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = sizeof(malformed_image);
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 0x300;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 8;
    RUNTIME_FUNCTION *entry = (PVOID)(malformed_image + 0x300);
    entry->BeginAddress = 0x400; entry->UnwindData = 0x800;
    ULONG *header = (PVOID)(malformed_image + 0x800);
    *header = (1u << 27) | 8; /* one code word, 32-byte function body */
    RTL_ARM64_UNWIND_IMAGE bad;
    CHECK(RtlArm64InitializeUnwindImage(malformed_image, sizeof(malformed_image), &bad) == STATUS_SUCCESS);
    const UCHAR codes[][4] = {
        {0xe8, 0xe4, 0xe3, 0xe3}, /* forbidden modern private trap frame */
        {0xcb, 0xff, 0xe4, 0xe3}, /* invalid GPR range, must not write outside X[] */
        {0xe3, 0xe3, 0xe3, 0xe3}, /* missing END must not look like success */
        {0xe3, 0xe3, 0xe3, 0xe0}, /* truncated multi-byte operation */
    };
    for (ULONG test = 0; test < sizeof(codes) / sizeof(codes[0]); ++test) {
        RtlCopyMemory(malformed_image + 0x804, codes[test], 4);
        output = input;
        output.Pc = (ULONG_PTR)malformed_image + 0x410;
        unchanged = output;
        RtlFillMemoryUlong(&result, sizeof(result), 0xa5a5a5a5);
        NTSTATUS status = RtlArm64UnwindOne(&bad, &output, stack_low, stack_high, &result);
        CHECK(!NT_SUCCESS(status) && equal_bytes(&output, &unchanged, sizeof(output)));
        const UCHAR *bytes = (PVOID)&result;
        for (ULONG i = 0; i < sizeof(result); ++i) CHECK(bytes[i] == 0xa5);
    }
    CHECK(RtlArm64InitializeUnwindImage(malformed_image, 80, &bad) == STATUS_INVALID_IMAGE_FORMAT);
}
BOOLEAN XpHandleUnwindCheckpoint(PKTRAP_FRAME frame)
{
    ULONG code = (ULONG)(frame->Esr & 65535);
    if (!enabled || ((frame->Esr >> 26) & 63) != 0x3c || code < 0x181 || code > 0x187)
        return FALSE;
    CHECK(code == 0x181 + seen++);
    RtlZeroMemory(&input, sizeof(input));
    input.ContextFlags = CONTEXT_FULL;
    for (ULONG i = 0; i < 31; ++i) input.X[i] = frame->X[i];
    input.Sp = frame->Sp; input.Pc = frame->Pc; input.Cpsr = (ULONG)frame->Cpsr;
    for (ULONG i = 0; i < 32; ++i) input.V[i] = frame->V[i];
    input.Fpcr = frame->Fpcr; input.Fpsr = frame->Fpsr;
    output = input;
    NTSTATUS status = RtlArm64UnwindOne(&image, &output, stack_low, stack_high, &result);
    CHECK(status == STATUS_SUCCESS);
    CHECK(output.Sp == expected.Sp && output.Pc == expected.Pc);
    CHECK(output.X18 == expected.X18);
    for (ULONG i = 19; i < 31; ++i) CHECK(output.X[i] == expected.X[i]);
    for (ULONG i = 8; i < 16; ++i) CHECK(output.V[i].Low == expected.V[i].Low);
    CHECK(result.EstablisherFrame == expected.Sp && !result.Handler && !result.HandlerData);
    CHECK(output.ContextFlags & 0x20000000u);
    if (code == 0x184) {
        /* Real saved frame, deliberately truncated permitted stack range.
         * Failure must not commit even partially restored register values. */
        unchanged = input; output = input;
        RtlFillMemoryUlong(&result, sizeof(result), 0xa5a5a5a5);
        status = RtlArm64UnwindOne(&image, &output, input.Sp, input.Sp + 16, &result);
        CHECK(!NT_SUCCESS(status) && equal_bytes(&output, &unchanged, sizeof(output)));
        const UCHAR *bytes = (PVOID)&result;
        for (ULONG i = 0; i < sizeof(result); ++i) CHECK(bytes[i] == 0xa5);
        malformed_cases();
    }
    frame->Pc += 4; /* Resume the actual assembly at the next checkpoint. */
    return TRUE;
}
VOID XpCheckUnwind(PXP_ARM64_BOOT_BLOCK block)
{
    UCHAR *base = (PVOID)&__ImageBase;
    IMAGE_NT_HEADERS64 *nt = (PVOID)(base + __ImageBase.e_lfanew);
    CHECK(RtlArm64InitializeUnwindImage(base, nt->OptionalHeader.SizeOfImage, &image) == STATUS_SUCCESS);
    BOOLEAN full = FALSE, packed = FALSE;
    for (ULONG i = 0; i < image.Count; ++i) {
        ULONG_PTR pc = image.Base + image.Functions[i].BeginAddress;
        if (pc == (ULONG_PTR)XpUnwindFullFixture) full = !(image.Functions[i].UnwindData & 3);
        if (pc == (ULONG_PTR)XpUnwindPackedBody) packed = (image.Functions[i].UnwindData & 3) == 1;
    }
    CHECK(full && packed);
    stack_low = block->StackBase; stack_high = block->Nt.KernelStack;
    ULONG64 old_cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(old_cpacr));
    ULONG64 cpacr = old_cpacr | (3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(cpacr) : "memory");
    enabled = TRUE;
    XpUnwindFullFixture(&expected);
    XpUnwindPackedFixture(&expected);
    enabled = FALSE;
    __asm__ volatile("msr cpacr_el1, %0\nisb" :: "r"(old_cpacr) : "memory");
    CHECK(seen == 7);
    XpTestReport("PASS: real EL1 prolog/body/epilog and packed PE unwind, GPR/FP restoration and bounded failure\n");
}
