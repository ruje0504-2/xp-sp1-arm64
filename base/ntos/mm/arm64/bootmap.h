/* Owned ARMv8.0 EL1 bootstrap translation tables. */
#ifndef XP_ARM64_BOOTMAP_H
#define XP_ARM64_BOOTMAP_H
#include "../../../boot/arm64/bootimage.h"
#define MM_BOOT_TABLE_PAGES 256
#define MM_BOOT_STACK_COUNT 32
#define MM_BOOT_STACK_DATA_PAGES (KERNEL_STACK_SIZE / PAGE_SIZE)
#define MM_BOOT_STACK_STRIDE (MM_BOOT_STACK_DATA_PAGES + 1)
#define MM_BOOT_STACK_ARENA_PAGES (MM_BOOT_STACK_COUNT * MM_BOOT_STACK_STRIDE)
#define MM_BOOT_POOL_BASE 0xfffffad000000000ULL
#define MM_BOOT_POOL_PAGES (PAGE_SIZE / sizeof(ULONG64))
#define MM_BOOT_PAGED_POOL_BASE 0xfffffa8000000000ULL
#define MM_BOOT_PAGED_POOL_PAGES (PAGE_SIZE / sizeof(ULONG64))
#define MM_BOOT_DISPLAY_BASE 0xfffffac000000000ULL
#define MM_BOOT_DISPLAY_LIMIT (32u * 1024u * 1024u)
#define MM_BOOT_IO_BASE 0xfffffaa000000000ULL
#define MM_BOOT_IO_PAGES (PAGE_SIZE / sizeof(ULONG64))
typedef struct _MM_ARM64_BOOT_STACK {
    ULONG Index;
    ULONG64 Generation;
    ULONG_PTR Limit, Top;
} MM_ARM64_BOOT_STACK, *PMM_ARM64_BOOT_STACK;
typedef struct _MM_ARM64_BOOT_STACK_SLOT {
    ULONG64 Generation;
    PVOID Owner;
    volatile ULONG64 *Ptes[MM_BOOT_STACK_STRIDE];
    ULONG64 Descriptors[MM_BOOT_STACK_STRIDE];
    PFN_NUMBER Frames[MM_BOOT_STACK_DATA_PAGES];
} MM_ARM64_BOOT_STACK_SLOT;

typedef struct _MM_ARM64_BOOT_TABLES {
    ULONG64 PoolBase;
    ULONG PoolPages, UsedPages, CopiedPages;
    ULONG StartLevel, RootEntries;
    ULONG64 OriginalTtbr0, OriginalTtbr1, OriginalTcr;
    ULONG64 Root, KernelRoot, ActiveTcr;
    ULONG64 OriginalSctlr, SwitchSctlr;
    ULONG_PTR RootSwitchAddress, RootSwitchSize;
    ULONG64 GuardAddress, GuardDescriptor;
    volatile ULONG64 *GuardPte;
    BOOLEAN Active;
    ULONG64 StackArena;
    RTL_BITMAP StackBitmap;
    ULONG StackBits[(MM_BOOT_STACK_COUNT + 31) / 32];
    BOOLEAN StacksPrepared;
    MM_ARM64_BOOT_STACK_SLOT Stacks[MM_BOOT_STACK_COUNT];
    volatile ULONG64 *NonPagedPoolPtes;
    ULONG64 NonPagedPoolDescriptor;
    volatile ULONG64 *PagedPoolPtes;
    volatile ULONG64 *IoPtes;
    ULONG64 Sources[MM_BOOT_TABLE_PAGES];
    UCHAR Levels[MM_BOOT_TABLE_PAGES];
} MM_ARM64_BOOT_TABLES, *PMM_ARM64_BOOT_TABLES;
ULONG64 MmArm64SwitchBootRoot(ULONG64 Root, ULONG64 Tcr, ULONG64 KernelRoot);
extern const UCHAR MmArm64SwitchBootRootEnd[];
struct _XP_ARM64_BOOT_BLOCK;
NTSTATUS MmArm64TakeBootMappings(struct _XP_ARM64_BOOT_BLOCK *Block);
ULONG64 MmArm64QueryTranslation(ULONG_PTR Address, BOOLEAN Write);
ULONG64 MmArm64QueryPhysicalAddress(ULONG_PTR Address);
VOID MmArm64SetBootGuardPresent(PMM_ARM64_BOOT_TABLES Tables, BOOLEAN Present);
/* Early NT_UP, DAIF-masked API only. This is not the general NT PFN/stack MM. */
NTSTATUS MmArm64AllocateBootStack(PMM_ARM64_BOOT_TABLES Tables, PMM_ARM64_BOOT_STACK Stack);
NTSTATUS MmArm64FreeBootStack(PMM_ARM64_BOOT_TABLES Tables, PMM_ARM64_BOOT_STACK Stack);
NTSTATUS MmArm64BindBootStack(PMM_ARM64_BOOT_TABLES Tables, const MM_ARM64_BOOT_STACK *Stack, PVOID Owner);
NTSTATUS MmArm64UnbindBootStack(PMM_ARM64_BOOT_TABLES Tables, const MM_ARM64_BOOT_STACK *Stack, PVOID Owner);
/* Restricted early implementations of the original NT stack entry points. */
NTSTATUS MmArm64InitializeKernelStacks(PMM_ARM64_BOOT_TABLES Tables);
NTSTATUS MmArm64LookupKernelStack(PVOID Top, PMM_ARM64_BOOT_TABLES *Tables,
                                  PMM_ARM64_BOOT_STACK Stack);
VOID MmArm64NoteKernelStackActivation(PKTHREAD Thread);
NTSTATUS MmArm64MarkKernelStackRetiring(PKTHREAD Thread);
#endif
