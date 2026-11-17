#include <nt.h>
void XpTestReport(const char *message)
{
    register ULONG64 operation __asm__("x0") = 4;
    register const char *argument __asm__("x1") = message;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(argument) : "memory");
}
__declspec(noreturn) void XpTestFinish(ULONG code)
{
    ULONG64 block[2] = {0x20026, code};
    register ULONG64 operation __asm__("x0") = 0x20;
    register ULONG64 *argument __asm__("x1") = block;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(argument) : "memory");
    for (;;) __asm__ volatile("wfe");
}
__declspec(noreturn) void XpTestFailure(ULONG line)
{
    char digits[12];
    unsigned at = sizeof(digits) - 1;
    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + line % 10);
        line /= 10;
    } while (line);
    XpTestReport("FAIL: XP ARM64 control assertion at line ");
    XpTestReport(digits + at);
    XpTestReport("\n");
    XpTestFinish(1);
}

void XpTestHex(ULONG64 value)
{
    char text[19];
    const char *digits="0123456789abcdef";
    text[0]='0';text[1]='x';
    for (unsigned i=0;i<16;++i) text[2+i]=digits[(value>>(60-i*4))&15];
    text[18]=0;XpTestReport(text);
}
