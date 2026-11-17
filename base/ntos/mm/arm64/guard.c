#include <ntos.h>
#include "bootmap.h"

VOID MmArm64SetBootGuardPresent(PMM_ARM64_BOOT_TABLES state, BOOLEAN present)
{
    /* Only the dedicated non-executing guard leaf changes. Invalidating a
     * live leaf uses break + TLBI; restoring starts from an invalid leaf. */
    ULONG64 desired = present ? state->GuardDescriptor : 0;
    if (*state->GuardPte == desired) return;
    *state->GuardPte = desired;
    __asm__ volatile("dc cvac, %0\ndsb ish\ntlbi vmalle1is\ndsb ish\nisb"
                     :: "r"(state->GuardPte) : "memory");
}
