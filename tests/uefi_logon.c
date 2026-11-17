#include "xp-rm.h"
#include <pool.h>
#include "../base/ntos/mm/arm64/bootframes.h"
#include "uefi_test.h"

extern SIZE_T PoolTrackTableSize;

static SIZE_T logon_record_bytes(VOID)
{
    for (SIZE_T i = 0; i < PoolTrackTableSize; ++i) {
        if (PoolTrackTable[i].Key == 'sLeS') return PoolTrackTable[i].PagedBytes;
    }
    CHECK(FALSE);
    return 0;
}

VOID XpCheckBootLogonSessions(VOID)
{
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL && SepLogonSessions &&
          !SepRmState.AuditingEnabled && SepRmState.OperationalMode == LSA_MODE_PASSWORD_PROTECTED &&
          !SepRmState.LsaCommandPortHandle && !SepRmState.LsaCommandPortActive);
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    SIZE_T initial_record_bytes = logon_record_bytes();
    CHECK(initial_record_bytes == 2 * EX_REAL_POOL_USAGE(sizeof(SEP_LOGON_SESSION_REFERENCES)));

    LUID system = SeSystemAuthenticationId, anonymous = SeAnonymousAuthenticationId;
    PSEP_LOGON_SESSION_REFERENCES system_record = SepLogonSessions[SepLogonSessionIndex(&system)];
    PSEP_LOGON_SESSION_REFERENCES anonymous_record = SepLogonSessions[SepLogonSessionIndex(&anonymous)];
    CHECK(system_record && anonymous_record && system_record != anonymous_record &&
          RtlEqualLuid(&system_record->LogonId, &system) &&
          RtlEqualLuid(&anonymous_record->LogonId, &anonymous) &&
          !system_record->Next && !anonymous_record->Next &&
          system_record->ReferenceCount == 1 && !anonymous_record->ReferenceCount &&
          !system_record->pDeviceMap && !anonymous_record->pDeviceMap);
    NTSTATUS duplicate = SepCreateLogonSessionTrack(&system);
    CHECK(duplicate == STATUS_LOGON_SESSION_EXISTS && logon_record_bytes() == initial_record_bytes);

    /* Vary the high half while preserving both the low half and hash bucket. */
    LUID first;
    ExAllocateLocallyUniqueId(&first);
    LUID second = first;
    ++second.HighPart;
    ULONG bucket = SepLogonSessionIndex(&first);
    CHECK(!SepLogonSessions[bucket] && SepLogonSessionIndex(&second) == bucket);
    CHECK(SepCreateLogonSessionTrack(&first) == STATUS_SUCCESS);
    PSEP_LOGON_SESSION_REFERENCES tail = SepLogonSessions[bucket];
    CHECK(SepCreateLogonSessionTrack(&second) == STATUS_SUCCESS);
    PSEP_LOGON_SESSION_REFERENCES head = SepLogonSessions[bucket];
    CHECK(head && tail && head != tail && head->Next == tail && !tail->Next &&
          RtlEqualLuid(&head->LogonId, &second) && RtlEqualLuid(&tail->LogonId, &first));
    CHECK(SepCreateLogonSessionTrack(&first) == STATUS_LOGON_SESSION_EXISTS);
    CHECK(SepReferenceLogonSession(&first) == STATUS_SUCCESS &&
          SepReferenceLogonSession(&first) == STATUS_SUCCESS && tail->ReferenceCount == 2 &&
          !head->ReferenceCount);
    NTSTATUS busy = SepDeleteLogonSessionTrack(&first);
    CHECK(busy == STATUS_BAD_LOGON_SESSION_STATE && head->Next == tail && tail->ReferenceCount == 2);
    SepDeReferenceLogonSession(&first);
    CHECK(head->Next == tail && tail->ReferenceCount == 1);
    SepDeReferenceLogonSession(&first);
    CHECK(SepLogonSessions[bucket] == head && !head->Next &&
          RtlEqualLuid(&head->LogonId, &second) && !head->ReferenceCount);
    NTSTATUS missing = SepReferenceLogonSession(&first);
    CHECK(missing == STATUS_NO_SUCH_LOGON_SESSION && logon_record_bytes() > initial_record_bytes);
    CHECK(SepDeleteLogonSessionTrack(&second) == STATUS_SUCCESS &&
          SepDeleteLogonSessionTrack(&second) == STATUS_NO_SUCH_LOGON_SESSION &&
          !SepLogonSessions[bucket] && logon_record_bytes() == initial_record_bytes);
    CHECK(!SepRmDbLock.ActiveCount && !KeGetCurrentThread()->KernelApcDisable);

    ULONG sessions = 0;
    for (ULONG i = 0; i < SEP_LOGON_TRACK_ARRAY_SIZE; ++i) {
        PSEP_LOGON_SESSION_REFERENCES record = SepLogonSessions[i];
        if (record) {
            ++sessions;
            CHECK((record == system_record || record == anonymous_record) &&
                  !record->Next && record->ReferenceCount == (ULONG)(record == system_record));
        }
    }
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(sessions == 2 && before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Logon session evidence sessions="); XpTestHex(sessions);
    XpTestReport(" buckets="); XpTestHex(SEP_LOGON_TRACK_ARRAY_SIZE);
    XpTestReport(" collision_bucket="); XpTestHex(bucket);
    XpTestReport(" duplicate="); XpTestHex((ULONG)duplicate);
    XpTestReport(" busy="); XpTestHex((ULONG)busy);
    XpTestReport(" missing="); XpTestHex((ULONG)missing);
    XpTestReport(" record_bytes="); XpTestHex(logon_record_bytes());
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original reference monitor boot sessions, LUID collision and last-reference retirement\n");
}
