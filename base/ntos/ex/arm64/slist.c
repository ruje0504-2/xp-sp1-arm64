/* XP sequenced-list API for the single online ARM64 boot processor.
 * The public 16-byte header retains depth in the low 16 bits of Alignment.
 * Region holds the full native pointer, including TTBR1 address bits. */
#include <ntos.h>

#define SLIST_DEPTH_MASK 0xffffULL
#define SLIST_SEQUENCE_INCREMENT 0x10000ULL

C_ASSERT(sizeof(SLIST_HEADER) == 16);
C_ASSERT(__alignof(SLIST_HEADER) == 16);

static ULONG64 lock_slist(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    if (KeNumberProcessors != 1)
        KeBugCheckEx(SPIN_LOCK_ALREADY_OWNED, 0, KeNumberProcessors, 0, 0);
    return daif;
}

static VOID unlock_slist(ULONG64 daif)
{
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}

VOID InitializeSListHead(PSLIST_HEADER head)
{
    if ((ULONG_PTR)head & 15) ExRaiseStatus(STATUS_DATATYPE_MISALIGNMENT);
    head->Region = 0;
    head->Alignment = 0;
}

USHORT ExQueryDepthSList(PSLIST_HEADER head)
{
    return (USHORT)__atomic_load_n(&head->Alignment, __ATOMIC_ACQUIRE);
}

PSLIST_ENTRY FirstEntrySList(const SLIST_HEADER *head)
{
    return (PSLIST_ENTRY)(ULONG_PTR)__atomic_load_n(&head->Region, __ATOMIC_ACQUIRE);
}

static VOID publish_slist(PSLIST_HEADER head, PSLIST_ENTRY first, ULONG64 alignment)
{
    __atomic_store_n(&head->Region, (ULONG_PTR)first, __ATOMIC_RELEASE);
    __atomic_store_n(&head->Alignment, alignment, __ATOMIC_RELEASE);
}

PSLIST_ENTRY ExpInterlockedPopEntrySList(PSLIST_HEADER head)
{
    ULONG64 daif = lock_slist();
    PSLIST_ENTRY first = (PSLIST_ENTRY)(ULONG_PTR)head->Region;
    if (first) publish_slist(head, first->Next, head->Alignment - 1);
    unlock_slist(daif);
    return first;
}

PSLIST_ENTRY FASTCALL InterlockedPushListSList(PSLIST_HEADER head, PSLIST_ENTRY first,
                                     PSLIST_ENTRY last, ULONG count)
{
    ULONG64 daif = lock_slist();
    PSLIST_ENTRY previous = (PSLIST_ENTRY)(ULONG_PTR)head->Region;
    ULONG64 alignment = head->Alignment;
    ULONG64 depth = (alignment & SLIST_DEPTH_MASK) + count;
    if (!first || !last || !count || depth > SLIST_DEPTH_MASK)
        KeBugCheckEx(BAD_POOL_CALLER, 7, (ULONG_PTR)head, count, depth);
    last->Next = previous;
    publish_slist(head, first, ((alignment & ~SLIST_DEPTH_MASK) + SLIST_SEQUENCE_INCREMENT) | depth);
    unlock_slist(daif);
    return previous;
}

PSLIST_ENTRY ExpInterlockedPushEntrySList(PSLIST_HEADER head, PSLIST_ENTRY entry)
{
    return InterlockedPushListSList(head, entry, entry, 1);
}

PSLIST_ENTRY ExpInterlockedFlushSList(PSLIST_HEADER head)
{
    ULONG64 daif = lock_slist();
    PSLIST_ENTRY first = (PSLIST_ENTRY)(ULONG_PTR)head->Region;
    if (first) publish_slist(head, NULL, head->Alignment & ~SLIST_DEPTH_MASK);
    unlock_slist(daif);
    return first;
}
