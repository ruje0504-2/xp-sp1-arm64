#include "ki.h"
#include "../../../boot/arm64/boot.h"
#include "../../hal/arm64/gicv2.h"
#include "../../mm/arm64/bootframes.h"
#include "../../mm/arm64/bootpool.h"
#include "../../mm/arm64/bootio.h"
#include "../../mm/arm64/bootimage.h"
#include "../../ex/arm64/bootexec.h"
#include "../../ob/arm64/bootobject.h"
#include "../../ps/arm64/bootprocess.h"
#include "clock.h"
#include "scheduler.h"
#include <ntimage.h>

extern IMAGE_DOS_HEADER __ImageBase;
VOID MmArm64InitializeSectionState(VOID);
/* This live callback makes the image use its original NT DIR64 path. */
static PKARM64_TRAP_HANDLER volatile boot_trap=KiArm64DispatchBootTrap;

BOOLEAN KiArm64DispatchBootTrap(PKTRAP_FRAME frame, ULONG kind)
{
    if (kind == 0 && MmArm64ResolvePagedPoolFault(frame)) return TRUE;
    return KiArm64BootTrap(frame, kind);
}
/* Initial clock state only. No timer ticks/completions are synthesized. */
volatile LARGE_INTEGER KeTickCount;

NTSTATUS KiArm64InitializeBootStructures(PXP_ARM64_BOOT_BLOCK block)
{
    if (!block || block->Size != sizeof(*block) ||
        block->Version != XP_ARM64_BOOT_VERSION || !block->BootServicesExited ||
        block->Nt.Prcb != (ULONG_PTR)&block->Pcr.Prcb ||
        block->Nt.Process != (ULONG_PTR)&block->InitialProcess.Pcb ||
        block->Nt.Thread || !block->DescriptorCount ||
        !block->Tables.Active || block->KernelImage.VirtualBase!=(ULONG_PTR)&__ImageBase ||
        block->KernelImage.VirtualBase!=XP_ARM64_KERNEL_LOAD_VA ||
        block->KernelImage.VirtualBase+block->KernelImage.EntryRva!=(ULONG_PTR)KiArm64EnterBoot ||
        !block->KernelImage.Relocations || boot_trap!=KiArm64DispatchBootTrap)
        return STATUS_INVALID_PARAMETER;
    ULONG_PTR sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (sp < block->StackBase || sp >= block->Nt.KernelStack ||
        block->Nt.KernelStack != block->StackBase + block->StackSize)
        return STATUS_INVALID_PARAMETER;
    NTSTATUS status = KiArm64InitializePcr(&block->Pcr, 0, block->SharedLocks, boot_trap);
    if (!NT_SUCCESS(status)) return status;
    __asm__ volatile("msr tpidr_el1, %0\nmov x18, %0\nmsr vbar_el1, %1\nisb"
                     :: "r"(&block->Pcr), "r"(KiArm64VectorTable) : "memory");
    block->InitialNode.ProcessorMask = 1;
    block->Pcr.Prcb.ParentNode = &block->InitialNode;
    PKSPECIAL_REGISTERS regs = &block->Pcr.Prcb.ProcessorState.SpecialRegisters;
#define READ_BOOT_REGISTER(field, reg) __asm__ volatile("mrs %0, " #reg : "=r"(regs->field))
    READ_BOOT_REGISTER(SctlrEl1, sctlr_el1);
    READ_BOOT_REGISTER(TcrEl1, tcr_el1);
    READ_BOOT_REGISTER(Ttbr0El1, ttbr0_el1);
    READ_BOOT_REGISTER(Ttbr1El1, ttbr1_el1);
    READ_BOOT_REGISTER(MairEl1, mair_el1);
    READ_BOOT_REGISTER(VbarEl1, vbar_el1);
    READ_BOOT_REGISTER(TpidrEl1, tpidr_el1);
    READ_BOOT_REGISTER(MpidrEl1, mpidr_el1);
#undef READ_BOOT_REGISTER
    /* ARM64 kernel bootstrap DirectoryTableBase: [0]=owned TTBR0,
     * [1]=owned TTBR1. Process address-space switching remains separate. */
    ULONG_PTR roots[2] = {
        regs->Ttbr0El1 & 0x0000fffffffff000ULL,
        regs->Ttbr1El1 & 0x0000fffffffff000ULL
    };
    if (roots[0]!=block->Tables.Root || roots[1]!=block->Tables.KernelRoot ||
        regs->TcrEl1!=block->Tables.ActiveTcr ||
        MmArm64QueryPhysicalAddress(block->KernelImage.VirtualBase)!=block->KernelImage.Base)
        return STATUS_INVALID_DEVICE_STATE;
    ULONG64 shared=block->SharedPagePfn<<PAGE_SHIFT;
    KeLoaderBlock = &block->Nt;
    KeNumberProcessors = 1; /* Only this actual boot processor is online. */
    status=MmArm64InitializeBootFrames(block);
    if (!NT_SUCCESS(status)) return status;
    status=MmArm64InitializeNonPagedPool(&block->Tables);
    if (!NT_SUCCESS(status)) return status;
    status=MmArm64InitializePagedPool(&block->Tables);
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(&block->InitialProcess, sizeof(block->InitialProcess));
    KeInitializeProcess(&block->InitialProcess.Pcb, 0, 1, roots, FALSE);
    if (!NT_SUCCESS(block->PlatformStatus)) return block->PlatformStatus;
    status = MmArm64InitializeBootIo(block);
    if (!NT_SUCCESS(status)) return status;
    block->HalStatus = HalArm64InitializeGicV2(&block->Platform);
    if (!NT_SUCCESS(block->HalStatus)) return block->HalStatus;
    status=KiArm64InitializeBootClock(&block->Time,shared);
    if (!NT_SUCCESS(status)) return status;
    status = MmArm64InitializeKernelStacks(&block->Tables);
    if (!NT_SUCCESS(status)) return status;
    status = KiArm64InitializeDispatcher(&block->InitialProcess.Pcb);
    if (!NT_SUCCESS(status)) return status;
    ExInitPoolLookasidePointers();
    KeLowerIrql(PASSIVE_LEVEL);
    status = ExpArm64InitializeBootResources();
    KIRQL previous;
    KeRaiseIrql(HIGH_LEVEL, &previous);
    if (!NT_SUCCESS(status)) return status;
    block->InitializationStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/* Bind the already executing boot stack to the original idle-thread path.
 * The executive phase of KiInitializeKernel is a separate dependency. */
NTSTATUS KiArm64InitializeBootIdle(PXP_ARM64_BOOT_BLOCK block)
{
    ULONG_PTR sp;
    ULONG64 daif;
    __asm__ volatile("mov %0, sp\nmrs %1, daif" : "=r"(sp), "=r"(daif));
    PKPRCB prcb=KeGetCurrentPrcb();
    if (!block || KeLoaderBlock!=&block->Nt || block->InitializationStatus!=STATUS_SUCCESS ||
        !block->Tables.Active || prcb!=&block->Pcr.Prcb || KeNumberProcessors!=1 ||
        KeGetCurrentIrql()!=HIGH_LEVEL || (daif&0x3c0)!=0x3c0 || block->Nt.Thread ||
        prcb->CurrentThread || prcb->IdleThread || sp<block->StackBase ||
        sp>=block->Nt.KernelStack || block->Nt.KernelStack-block->StackBase!=KERNEL_STACK_SIZE ||
        block->StackBase!=block->StackAllocationBase+PAGE_SIZE || *block->Tables.GuardPte)
        return STATUS_INVALID_DEVICE_STATE;

    PKPROCESS process=&block->InitialProcess.Pcb;
    KeLowerIrql(PASSIVE_LEVEL);
    InitializePool(NonPagedPool, 0);
    const ULONG tag = 'erhT';
    PETHREAD initial_thread = ExAllocatePoolWithTag(NonPagedPool, sizeof(*initial_thread), tag);
    NTSTATUS status = initial_thread ? MmArm64PromotePhysicalMemoryDescriptor(block) :
        STATUS_INSUFFICIENT_RESOURCES;
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(initial_thread, sizeof(*initial_thread));
        block->Nt.Thread = (ULONG_PTR)&initial_thread->Tcb;
        status = KeInitializeThread(&initial_thread->Tcb, (PVOID)block->Nt.KernelStack,
                                    NULL, NULL, NULL, NULL, NULL, process);
    }
    if (!NT_SUCCESS(status)) {
        block->Nt.Thread=0;
        if (initial_thread) ExFreePoolWithTag(initial_thread, tag);
        KIRQL previous;
        KeRaiseIrql(HIGH_LEVEL,&previous);
        return status;
    }
    PKTHREAD idle = &initial_thread->Tcb;
    /* The idle thread is already running. Its first outgoing switch saves
     * the live frame, so no synthetic entry frame is needed on this stack. */
    idle->NextProcessor=prcb->Number;
    idle->EnableStackSwap=FALSE;
    idle->State=Running;
    KeActiveProcessors=prcb->SetMember;
    process->ActiveProcessors|=prcb->SetMember;
    prcb->CurrentThread=idle;
    prcb->IdleThread=idle;
    KiIdleSummary=prcb->SetMember;
    InitializePool(PagedPool, 0);
    MmArm64InitializeSectionState();
    status = MmArm64InitializeBootImage(block);
    if (!NT_SUCCESS(status)) return status;
    status = ObArm64InitializeBootObjectState();
    if (!NT_SUCCESS(status)) return status;
    if (!SeInitSystem())
        return STATUS_UNSUCCESSFUL;
    if (!PsArm64InitializePhase0Objects())
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}
