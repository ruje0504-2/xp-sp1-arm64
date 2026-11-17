#include <ntos.h>
#include "bootmap.h"

ULONG64 MmArm64QueryTranslation(ULONG_PTR address, BOOLEAN write)
{
    ULONG64 result, saved;
    __asm__ volatile("mrs %0, par_el1" : "=r"(saved));
    if (write) {
        __asm__ volatile("at s1e1w, %0" :: "r"(address) : "memory");
    } else {
        __asm__ volatile("at s1e1r, %0" :: "r"(address) : "memory");
    }
    __asm__ volatile("isb\nmrs %0, par_el1" : "=r"(result) :: "memory");
    __asm__ volatile("msr par_el1, %0" :: "r"(saved));
    return result;
}
ULONG64 MmArm64QueryPhysicalAddress(ULONG_PTR address)
{
    ULONG64 result=MmArm64QueryTranslation(address,FALSE);
    return (result & 1) ? ~0ULL : (result & 0x0000fffffffff000ULL) | (address & 4095);
}
