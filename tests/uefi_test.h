#ifndef XP_UEFI_TEST_H
#define XP_UEFI_TEST_H
#include <nt.h>
void XpTestReport(const char *Message);
void XpTestHex(ULONG64 Value);
__declspec(noreturn) void XpTestFinish(ULONG Code);
__declspec(noreturn) void XpTestFailure(ULONG Line);
void XpCheckKernelTraps(void);
#define CHECK(condition) do { if (!(condition)) XpTestFailure(__LINE__); } while (0)
#endif
