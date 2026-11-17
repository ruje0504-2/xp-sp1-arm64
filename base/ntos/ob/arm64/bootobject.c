/* Early state preceding the initial object type creation in XP ObInitSystem. */
#include "bootobject.h"
#include "xp-obcache.h"

PHANDLE_TABLE ObpKernelHandleTable;
GENERAL_LOOKASIDE ObpCreateInfoLookasideList, ObpNameBufferLookasideList;
KEVENT ObpDefaultObject;
EX_PUSH_LOCK ObpLock;
FAST_MUTEX ObpDeviceMapLock;
PVOID ObpRemoveObjectList;
extern PFN_COUNT MmNumberOfPhysicalPages;

/* Storage belongs to psinit.c in XP. The retained initializer is psquota.c. */
EPROCESS_QUOTA_BLOCK PspDefaultQuotaBlock;
KSPIN_LOCK PspQuotaLock;

NTSTATUS ObArm64InitializeBootObjectState(VOID)
{
    static BOOLEAN started;
    if (started || KeNumberProcessors != 1 || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !KeGetCurrentThread() || ObpKernelHandleTable || PsGetCurrentProcess()->ObjectTable)
        return STATUS_INVALID_DEVICE_STATE;
    started = TRUE;

    /* XP Workstation Phase 0 is a large system at 32 MiB and above.
     * Product-suite registry discovery is a later startup dependency. */
    BOOLEAN large = MmNumberOfPhysicalPages >= (32 * 1024 * 1024 / PAGE_SIZE);
    ExInitializeSystemLookasideList(&ObpCreateInfoLookasideList, NonPagedPool,
        sizeof(OBJECT_CREATE_INFORMATION), 'iCbO', large ? 32 : 3, &ExSystemLookasideListHead);
    ExInitializeSystemLookasideList(&ObpNameBufferLookasideList, PagedPool,
        OBJECT_NAME_BUFFER_SIZE, 'mNbO', large ? 16 : 3, &ExSystemLookasideListHead);

    PKPRCB prcb = KeGetCurrentPrcb();
    prcb->PPLookasideList[LookasideCreateInfoList].P = &ObpCreateInfoLookasideList;
    prcb->PPLookasideList[LookasideCreateInfoList].L = &ObpCreateInfoLookasideList;
    prcb->PPLookasideList[LookasideNameBufferList].P = &ObpNameBufferLookasideList;
    prcb->PPLookasideList[LookasideNameBufferList].L = &ObpNameBufferLookasideList;

    ObpRemoveObjectList = NULL;
    NTSTATUS status = ObpInitSecurityDescriptorCache();
    if (!NT_SUCCESS(status)) return status;
    KeInitializeEvent(&ObpDefaultObject, NotificationEvent, TRUE);
    ExInitializePushLock(&ObpLock);
    PsGetCurrentProcess()->GrantedAccess = PROCESS_ALL_ACCESS;
    PsGetCurrentThread()->GrantedAccess = THREAD_ALL_ACCESS;
    ExInitializeFastMutex(&ObpDeviceMapLock);

    PsInitializeQuotaSystem();
    PHANDLE_TABLE table = ExCreateHandleTable(NULL);
    if (!table) return STATUS_INSUFFICIENT_RESOURCES;
    ObpKernelHandleTable = PsGetCurrentProcess()->ObjectTable = table;
    status = ObArm64InitializeBootObjectTypes();
    if (!NT_SUCCESS(status)) return status;
    return ObArm64InitializeBootNamespace();
}
