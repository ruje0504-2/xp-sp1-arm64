/* ARM64 user-mode thread access. The loader must initialize x18 to TEB. */
#ifndef XP_NXARM64_H
#define XP_NXARM64_H
#if defined(_M_ARM64) && !defined(__midl)
FORCEINLINE PTEB NtCurrentTeb(VOID)
{
    PTEB teb;
    __asm__ volatile("mov %0, x18" : "=r"(teb));
    return teb;
}
FORCEINLINE PVOID GetCurrentFiber(VOID)
{
    return ((PNT_TIB)NtCurrentTeb())->FiberData;
}
FORCEINLINE PVOID GetFiberData(VOID)
{
    return *(PVOID *)GetCurrentFiber();
}
#endif
#endif
