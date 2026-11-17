#include "../base/ntos/ke/arm64/scheduler.h"
#include "../base/ntos/ob/arm64/bootobject.h"
#include "../base/ntos/ps/arm64/bootexit.h"
#include "../base/ntos/mm/arm64/bootframes.h"
#include "uefi_test.h"

#define LEAF_ENTRIES (PAGE_SIZE / sizeof(HANDLE_TABLE_ENTRY))
#define HANDLE_COUNT (LEAF_ENTRIES + 12)

extern LIST_ENTRY HandleTableListHead;
static ETHREAD lock_waiters[3], entry_waiter;
static EX_PUSH_LOCK push_lock;
static ULONG writer_handoffs, reader_handoffs, entry_handoffs;
static PHANDLE_TABLE entry_table;
static HANDLE entry_handle;
static HANDLE handles[HANDLE_COUNT];
static struct DECLSPEC_ALIGN(16) {
    KEVENT Event;
} handle_objects[2];

static VOID start_worker(PKSTART_ROUTINE routine, PVOID argument)
{
    routine(argument);
    CHECK(FALSE);
}

static VOID ready_worker(PETHREAD thread, PKPROCESS process, PKSTART_ROUTINE routine)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = KeInitializeThread(&thread->Tcb, NULL, start_worker, routine, thread,
                                         NULL, NULL, process);
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    CHECK(status == STATUS_SUCCESS);
    KeSetPriorityThread(&thread->Tcb, 19);
    KeReadyThread(&thread->Tcb);
    CHECK(thread->Tcb.State == Waiting && thread->Tcb.WaitReason == Executive);
}

static VOID join_worker(PETHREAD thread)
{
    CHECK(KeWaitForSingleObject(&thread->Tcb, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS);
    CHECK(thread->Tcb.State == Terminated && thread->ExitStatus == STATUS_SUCCESS);
}

static VOID push_lock_waiter(PVOID parameter)
{
    PETHREAD thread = parameter;
    KeEnterCriticalRegion();
    if (thread == &lock_waiters[0]) {
        ExAcquirePushLockExclusive(&push_lock);
        CHECK(!writer_handoffs && !reader_handoffs);
        ++writer_handoffs;
        ExReleasePushLockExclusive(&push_lock);
    } else {
        ExAcquirePushLockShared(&push_lock);
        CHECK(writer_handoffs == 1);
        ++reader_handoffs;
        ExReleasePushLockShared(&push_lock);
    }
    KeLeaveCriticalRegion();
    PsArm64ExitBootThread(thread, STATUS_SUCCESS);
}

static VOID test_push_lock(PKPROCESS process)
{
    ExInitializePushLock(&push_lock);
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&push_lock);
    CHECK(push_lock.Shared == 1 && ExTryConvertPushLockSharedToExclusive(&push_lock));
    CHECK(push_lock.Exclusive && !push_lock.Waiting);
    ExReleasePushLockExclusive(&push_lock);
    ExAcquirePushLockShared(&push_lock);
    ExAcquirePushLockShared(&push_lock);
    CHECK(push_lock.Shared == 2);
    for (ULONG i = 0; i < 3; ++i) {
        ready_worker(&lock_waiters[i], process, push_lock_waiter);
        CHECK(push_lock.Waiting);
    }

    const EX_PUSH_LOCK_WAIT_BLOCK *wait = (PVOID)(push_lock.Value & ~(ULONG_PTR)EX_PUSH_LOCK_WAITING);
    CHECK(wait && !wait->Exclusive && wait->Next && !wait->Next->Exclusive);
    CHECK(wait->Next->Next && wait->Next->Next->Exclusive && wait->Next->Next->ShareCount == 2 &&
          !wait->Next->Next->Next);
    ExReleasePushLockShared(&push_lock);
    CHECK(!writer_handoffs && !reader_handoffs && push_lock.Waiting &&
          wait->Next->Next->ShareCount == 1);
    ExReleasePushLockShared(&push_lock);
    KeLeaveCriticalRegion();
    for (ULONG i = 0; i < 3; ++i) join_worker(&lock_waiters[i]);
    CHECK(writer_handoffs == 1 && reader_handoffs == 2 && !push_lock.Value);
}

static VOID handle_entry_waiter(PVOID parameter)
{
    PETHREAD thread = parameter;
    KeEnterCriticalRegion();
    PHANDLE_TABLE_ENTRY entry = ExMapHandleToPointer(entry_table, entry_handle);
    CHECK(entry && entry->Object == &handle_objects[0].Event &&
          entry->GrantedAccess == 0x10203);
    ++entry_handoffs;
    ExUnlockHandleTableEntry(entry_table, entry);
    KeLeaveCriticalRegion();
    PsArm64ExitBootThread(thread, STATUS_SUCCESS);
}

static VOID test_handle_table(PKPROCESS process, ULONG *level)
{
    CHECK(HandleTableListHead.Flink == &ObpKernelHandleTable->HandleTableList &&
          HandleTableListHead.Blink == &ObpKernelHandleTable->HandleTableList);

    PHANDLE_TABLE table = ExCreateHandleTable(NULL);
    CHECK(table && !table->QuotaProcess && !table->HandleCount &&
          ObpKernelHandleTable->HandleTableList.Flink == &table->HandleTableList &&
          HandleTableListHead.Blink == &table->HandleTableList);
    CHECK(!(table->TableCode & 3) && table->FirstFree == 4 &&
          table->NextHandleNeedingPool == LEAF_ENTRIES * 4);
    for (ULONG i = 0; i < 2; ++i) {
        KeInitializeEvent(&handle_objects[i].Event, NotificationEvent, FALSE);
    }

    HANDLE_TABLE_ENTRY initial = {0};
    initial.GrantedAccess = 0x10203;
    for (ULONG i = 0; i < HANDLE_COUNT; ++i) {
        initial.Object = &handle_objects[i % 2].Event;
        handles[i] = ExCreateHandle(table, &initial);
        CHECK(handles[i] && !((ULONG_PTR)handles[i] & 3));
        CHECK(i == 0 || (ULONG_PTR)handles[i] > (ULONG_PTR)handles[i - 1]);
    }
    *level = (ULONG)(table->TableCode & 3);
    CHECK(*level == 1 && table->HandleCount == HANDLE_COUNT &&
          table->NextHandleNeedingPool == 2 * LEAF_ENTRIES * 4);
    entry_table = table;
    entry_handle = handles[0];
    KeEnterCriticalRegion();
    PHANDLE_TABLE_ENTRY locked = ExMapHandleToPointer(table, entry_handle);
    CHECK(locked && locked->Object == &handle_objects[0].Event);
    ready_worker(&entry_waiter, process, handle_entry_waiter);
    const EX_PUSH_LOCK_WAIT_BLOCK *entry_wait = table->HandleContentionEvent.Ptr;
    CHECK(entry_wait && entry_waiter.Tcb.WaitBlockList->Object == &entry_wait->WakeEvent &&
          !entry_handoffs);
    ExUnlockHandleTableEntry(table, locked);
    KeLeaveCriticalRegion();
    join_worker(&entry_waiter);
    CHECK(entry_handoffs == 1 && !table->HandleContentionEvent.Ptr);

    for (ULONG i = 0; i < HANDLE_COUNT; ++i) {
        KeEnterCriticalRegion();
        PHANDLE_TABLE_ENTRY entry = ExMapHandleToPointer(table, handles[i]);
        CHECK(entry && entry->Object == &handle_objects[i % 2].Event &&
              entry->GrantedAccess == 0x10203);
        ExUnlockHandleTableEntry(table, entry);
        KeLeaveCriticalRegion();
        CHECK(ExDestroyHandle(table, handles[i], NULL));
    }
    CHECK(!table->HandleCount);
    KeEnterCriticalRegion();
    CHECK(!ExMapHandleToPointer(table, handles[0]) && !ExMapHandleToPointer(table, NULL) &&
          !ExMapHandleToPointer(table, (HANDLE)(ULONG_PTR)(LEAF_ENTRIES * 4)));
    KeLeaveCriticalRegion();
    CHECK(!ExDestroyHandle(table, handles[0], NULL));

    /* Strict FIFO keeps releases on LastFree until the remaining free
     * entries have been consumed, even when only one table is active. */
    ExSetHandleTableStrictFIFO(table);
    CHECK(table->StrictFIFO);
    initial.Object = &handle_objects[0].Event;
    HANDLE recycled = ExCreateHandle(table, &initial);
    CHECK(recycled && table->HandleCount == 1 && ExDestroyHandle(table, recycled, NULL));
    CHECK(!table->HandleCount);
    ExDestroyHandleTable(table, NULL);
    CHECK(HandleTableListHead.Flink == &ObpKernelHandleTable->HandleTableList &&
          HandleTableListHead.Blink == &ObpKernelHandleTable->HandleTableList);
}

VOID XpCheckExecutiveHandles(PKPROCESS process)
{
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    ULONG stacks = process->StackCount;
    CHECK(KiArm64EnableTimerDispatch() == STATUS_SUCCESS);
    test_push_lock(process);
    KEVENT park;
    LARGE_INTEGER grace;
    KeInitializeEvent(&park, SynchronizationEvent, FALSE);
    grace.QuadPart = -1000000;
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &grace) == STATUS_TIMEOUT);
    CHECK(process->StackCount == stacks && !PsReaperList);

    ULONG level;
    test_handle_table(process, &level);
    CHECK(KeWaitForSingleObject(&park, Executive, KernelMode, FALSE, &grace) == STATUS_TIMEOUT);
    KiArm64DisableTimerDispatch();
    CHECK(process->StackCount == stacks && !PsReaperList && !entry_waiter.Tcb.InitialStack);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(before.Free == after.Free && before.Allocated == after.Allocated);
    XpTestReport("Executive handle evidence handles="); XpTestHex(HANDLE_COUNT);
    XpTestReport(" level="); XpTestHex(level);
    XpTestReport(" writers="); XpTestHex(writer_handoffs);
    XpTestReport(" readers="); XpTestHex(reader_handoffs);
    XpTestReport(" entry_handoffs="); XpTestHex(entry_handoffs);
    XpTestReport(" free_before="); XpTestHex(before.Free);
    XpTestReport(" free_after="); XpTestHex(after.Free); XpTestReport("\n");
    XpTestReport("PASS: original XP push-lock handoffs and growing executive handle table\n");
}
