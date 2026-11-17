#include "userentry.h"
#include "../../mm/arm64/bootpool.h"
#include "xp-user-services.h"

VOID KiArm64EnterUserRaw(ULONG64 Root, KARM64_USER_ENTRY *Entry);
C_ASSERT(FIELD_OFFSET(KARM64_USER_ENTRY, KernelSp) == sizeof(KTRAP_FRAME));
C_ASSERT(FIELD_OFFSET(KARM64_USER_ENTRY, VectorKind) == sizeof(KTRAP_FRAME) + 24);

NTSTATUS KiArm64RunUser(MM_ARM64_USER_SPACE *space, KARM64_USER_ENTRY *entry)
{
    ULONG64 daif, current_root;
    __asm__ volatile("mrs %0, daif\nmrs %1, ttbr0_el1" : "=r"(daif), "=r"(current_root));
    if (!space || !space->Root || !space->Boot || KeNumberProcessors != 1 ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || (current_root & 0x0000fffffffff000ULL) != space->Boot->Tables.Root)
        return STATUS_INVALID_DEVICE_STATE;
    if (!entry || ((ULONG_PTR)entry & 15) ||
        !MmArm64IsBootNonPagedPoolAddress(entry) ||
        !MmArm64IsBootNonPagedPoolAddress((PUCHAR)entry + sizeof(*entry) - 1) ||
        entry->Frame.Pc < 0x10000 || entry->Frame.Pc >= 0x80000000 || (entry->Frame.Pc & 3) ||
        entry->Frame.Sp <= 0x10000 || entry->Frame.Sp > 0x80000000 || (entry->Frame.Sp & 15))
        return STATUS_INVALID_PARAMETER;
    __asm__ volatile("msr daifset, #15\nisb" ::: "memory");
    /* EL0 IRQs remain enabled. An IRQ returns to the kernel root before
     * ordinary EL1 IRQ dispatch, so its handler may safely use PCR/stack. */
    entry->Frame.Cpsr = (entry->Frame.Cpsr & 0xf0000000) | 0x340;
    KiArm64EnterUserRaw(space->Root, entry);
    entry->KernelSp = entry->KernelRoot = 0;
    entry->Frame.PreviousMode = UserMode;
    entry->Frame.FpStateSaved = TRUE;
    entry->Frame.ExceptionActive = entry->VectorKind == 0;
    entry->Frame.PreviousIrql = PASSIVE_LEVEL;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return STATUS_SUCCESS;
}

NTSTATUS KiArm64UserService(MM_ARM64_USER_SPACE *space, KARM64_USER_ENTRY *entry)
{
    if (!space || !entry || entry->VectorKind || entry->Frame.PreviousMode != UserMode ||
        (entry->Frame.Cpsr & 0x1f) || (entry->Frame.Esr >> 26) != 0x15 || (entry->Frame.Esr & 0xffff))
        return STATUS_INVALID_PARAMETER;
    KTRAP_FRAME *frame = &entry->Frame;
    NTSTATUS status = STATUS_INVALID_SYSTEM_SERVICE;
    if (frame->X[8] == XP_SERVICE_QUERY_PERFORMANCE_COUNTER) {
        LARGE_INTEGER counter, frequency;
        status = NtQueryPerformanceCounter(&counter, &frequency);
        if (NT_SUCCESS(status)) {
            ULONG64 daif;
            __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
            status = MmArm64CopyUserBuffer(space, frame->X[0], &counter, sizeof(counter), TRUE);
            if (NT_SUCCESS(status) && frame->X[1])
                status = MmArm64CopyUserBuffer(space, frame->X[1], &frequency, sizeof(frequency), TRUE);
            __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
            if (!NT_SUCCESS(status)) status = STATUS_ACCESS_VIOLATION;
        }
    }
    frame->X[0] = (ULONG)status;
    return STATUS_SUCCESS;
}
