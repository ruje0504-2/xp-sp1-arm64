#include <ntos.h>
static LONG XpFilter(PEXCEPTION_POINTERS information, ULONG token)
{
    return information->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION &&
           token == 0x4132 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
__declspec(noinline) ULONG XpSehContract(volatile ULONG *address, ULONG token)
{
    volatile ULONG cookie = token + 7;
    try {
        return *address + cookie;
    } except (XpFilter(GetExceptionInformation(), token)) {
        return cookie ^ GetExceptionCode();
    }
}

volatile ULONG XpFinallyObserved;
__declspec(noinline) static ULONG XpSehFinally(volatile ULONG *address, ULONG token)
{
    volatile ULONG cookie = token + 3;
    try {
        return *address + cookie;
    } finally {
        XpFinallyObserved = AbnormalTermination() ? cookie : 0;
    }
}
__declspec(noinline) ULONG XpSehOuter(volatile ULONG *address, ULONG token)
{
    volatile ULONG cookie = token + 11;
    try {
        return XpSehFinally(address, token);
    } except (XpFilter(GetExceptionInformation(), token)) {
        return cookie ^ GetExceptionCode();
    }
}

volatile ULONG XpSearchObserved;
__declspec(noinline) static LONG XpSearchFilter(PEXCEPTION_POINTERS pointers)
{
    XpSearchObserved = pointers->ExceptionRecord->ExceptionCode;
    return EXCEPTION_CONTINUE_SEARCH;
}
__declspec(noinline) static ULONG XpSearchInner(volatile ULONG *address)
{
    try { return *address; }
    except (XpSearchFilter(GetExceptionInformation())) { return 0xdead; }
}
__declspec(noinline) ULONG XpSearchOuter(volatile ULONG *address)
{
    try { return XpSearchInner(address); }
    except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
__declspec(noinline) ULONG XpSameFrameFinally(volatile ULONG *address, ULONG token)
{
    volatile ULONG cookie = token + 13;
    try {
        try { return *address + cookie; }
        finally { XpFinallyObserved = AbnormalTermination() ? cookie + 5 : 0; }
    } except (XpFilter(GetExceptionInformation(), token)) {
        return cookie ^ GetExceptionCode();
    }
}
extern ULONG XpSehResumeRead(volatile ULONG *Address);
extern const UCHAR XpSehResumeFault[], XpSehResumeTarget[];
static LONG XpResumeFilter(PEXCEPTION_POINTERS pointers)
{
    if (pointers->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION ||
        pointers->ExceptionRecord->ExceptionAddress != (PVOID)XpSehResumeFault)
        return EXCEPTION_CONTINUE_SEARCH;
    pointers->ContextRecord->Pc = (ULONG_PTR)XpSehResumeTarget;
    pointers->ContextRecord->X0 = 0x55;
    return EXCEPTION_CONTINUE_EXECUTION;
}
__declspec(noinline) ULONG XpSehContinue(volatile ULONG *address)
{
    try { return XpSehResumeRead(address) + 11; }
    except (XpResumeFilter(GetExceptionInformation())) { return 0xbad; }
}
