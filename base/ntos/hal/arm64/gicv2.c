/* Initial single-CPU GICv2 without Security Extensions. Register geometry
 * comes from public ACPI, then hardware identity/capabilities are checked.
 * The physical timer and self-targeted APC/dispatch SGIs share this IRQ path. */
#include "ki.h"
#include "gicv2.h"
#include "../../mm/arm64/bootio.h"
#include "../../ke/arm64/clock.h"
#include "../../ke/arm64/scheduler.h"
static HAL_ARM64_BOOT_PLATFORM platform;
static volatile HAL_ARM64_IRQ_STATS stats;
static volatile BOOLEAN profile_time;
static ULONG64 deadline;
#define GICD_SGIR 0xf00
#define GICD_SGIR_SELF (2u << 24)
/* The local SGIs use INTIDs equal to their software interrupt levels. */
#define SOFTWARE_IRQ_MASK ((1u << APC_LEVEL) | (1u << DISPATCH_LEVEL))
C_ASSERT(APC_LEVEL < 4 && DISPATCH_LEVEL < 4);
static ULONG read32(ULONG64 base, ULONG offset)
{
    ULONG value=*(volatile ULONG *)(ULONG_PTR)(base+offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static VOID write32(ULONG64 base, ULONG offset, ULONG value)
{
    *(volatile ULONG *)(ULONG_PTR)(base+offset)=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static ULONG64 mask_irq(VOID)
{
    ULONG64 saved;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2\nisb" : "=r"(saved) :: "memory");
    return saved;
}
static VOID restore_irq(ULONG64 saved)
{
    __asm__ volatile("msr daif, %0\nisb" :: "r"(saved) : "memory");
}
__declspec(noreturn) static VOID fatal(ULONG code)
{
    stats.LastFailure=code;
    __asm__ volatile("msr daifset, #15\nbrk #0x7fd" ::: "memory");
    for (;;) __asm__ volatile("wfe");
}
static ULONG priority(KIRQL level) { return (HIGH_LEVEL-level)<<4; }
ULONG64 HalArm64ReadCounter(VOID)
{
    ULONG64 value;
    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(value));
    return value;
}
LARGE_INTEGER HalArm64QueryPerformanceCounter(PLARGE_INTEGER frequency)
{
    if (frequency) {
        ULONG64 hertz;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hertz));
        frequency->QuadPart = (ULONG)hertz;
    }
    LARGE_INTEGER counter;
    counter.QuadPart = HalArm64ReadCounter();
    return counter;
}
ULONG64 HalArm64ReadTimerControl(VOID)
{
    ULONG64 value;__asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(value));return value;
}
ULONG HalArm64ReadPriorityMask(VOID)
{
    if (!stats.Initialized) fatal(1);
    return read32(platform.GicCpuInterface,4)&255;
}
VOID KeRaiseIrql(KIRQL level, PKIRQL old)
{
    ULONG64 saved=mask_irq();
    if (!stats.Initialized || !old || level>HIGH_LEVEL || level<KeGetPcr()->Irql) fatal(2);
    *old=KeGetPcr()->Irql;
    write32(platform.GicCpuInterface,4,priority(level));
    KeGetPcr()->Irql=level;
    __asm__ volatile("isb" ::: "memory");
    restore_irq(saved);
}
VOID KeLowerIrql(KIRQL level)
{
    ULONG64 saved=mask_irq();
    if (!stats.Initialized || level>KeGetPcr()->Irql) fatal(3);
    /* Some NT_UP APC/object paths release their last lock with KeLowerIrql
     * instead of KiUnlockDispatcherDatabase. Hand their selected thread off
     * to the same dispatch SGI while the old mask still applies. */
    if (level<DISPATCH_LEVEL && KeGetCurrentIrql()>=DISPATCH_LEVEL &&
        KeGetCurrentPrcb()->NextThread)
        KiRequestSoftwareInterrupt(DISPATCH_LEVEL);
    KeGetPcr()->Irql=level;
    write32(platform.GicCpuInterface,4,priority(level));
    __asm__ volatile("isb" ::: "memory");
    restore_irq(saved);
}
KIRQL KeRaiseIrqlToDpcLevel(VOID) { KIRQL old;KeRaiseIrql(DISPATCH_LEVEL,&old);return old; }
KIRQL KeRaiseIrqlToSynchLevel(VOID) { KIRQL old;KeRaiseIrql(SYNCH_LEVEL,&old);return old; }
NTSTATUS HalArm64InitializeGicV2(const HAL_ARM64_BOOT_PLATFORM *input)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (!input || stats.Initialized || KeNumberProcessors!=1 ||
        KeGetCurrentIrql()!=HIGH_LEVEL || (daif&0x3c0)!=0x3c0)
        return STATUS_INVALID_DEVICE_STATE;
    if (input->GicVersion!=2 || input->TimerIrq<16 || input->TimerIrq>=32 ||
        (input->TimerFlags&1) || !input->GicDistributor || !input->GicCpuInterface ||
        ((input->GicDistributor|input->GicCpuInterface)&4095))
        return STATUS_NOT_SUPPORTED;
    HAL_ARM64_BOOT_PLATFORM mapped;
    NTSTATUS status = MmArm64MapBootGic(input, &mapped);
    if (!NT_SUCCESS(status)) return status;
    ULONG version=(read32(mapped.GicDistributor,0xfe8)>>4)&15;
    ULONG typer=read32(mapped.GicDistributor,4);
    status=STATUS_NOT_SUPPORTED;
    if (version!=2 || (typer&(1u<<10))) goto release;
    ULONG banks=(typer&31)+1;
    /* Never discard an interrupt that firmware has not finished. */
    status=STATUS_DEVICE_BUSY;
    if ((read32(mapped.GicCpuInterface,0x14)&255)!=255) goto release;
    for (ULONG i=0;i<banks;++i)
        if (read32(mapped.GicDistributor,0x300+i*4)) goto release;
    platform=mapped;
    status=STATUS_NOT_SUPPORTED;
    __asm__ volatile("msr cntp_ctl_el0, xzr\nisb" ::: "memory");
    write32(platform.GicCpuInterface,0,0);
    write32(platform.GicDistributor,0,0);
    for (ULONG i=0;i<banks;++i) {
        write32(platform.GicDistributor,0x180+i*4,~0u);
        write32(platform.GicDistributor,0x280+i*4,~0u);
    }
    for (ULONG i=0;i<4;++i) write32(platform.GicDistributor,0xf10+i*4,~0u);
    for (ULONG i=0;i<banks*8;++i) write32(platform.GicDistributor,0x400+i*4,0xf0f0f0f0);
    ULONG software_priorities=0xf0f0f0f0;
    software_priorities &= ~((255u << (APC_LEVEL*8)) | (255u << (DISPATCH_LEVEL*8)));
    software_priorities |= priority(APC_LEVEL) << (APC_LEVEL*8);
    software_priorities |= priority(DISPATCH_LEVEL) << (DISPATCH_LEVEL*8);
    write32(platform.GicDistributor,0x400,software_priorities);
    if (read32(platform.GicDistributor,0x400)!=software_priorities)
        goto release;
    write32(platform.GicDistributor,0x100,SOFTWARE_IRQ_MASK);
    if ((read32(platform.GicDistributor,0x100)&SOFTWARE_IRQ_MASK)!=SOFTWARE_IRQ_MASK)
        goto release;
    ULONG off=0x400+(platform.TimerIrq&~3u);
    ULONG shift=(platform.TimerIrq&3)*8;
    ULONG value=read32(platform.GicDistributor,off);
    value=(value&~(255u<<shift))|(priority(CLOCK_LEVEL)<<shift);
    write32(platform.GicDistributor,off,value);
    off=0xc00+(platform.TimerIrq/16)*4;
    shift=(platform.TimerIrq%16)*2;
    value=read32(platform.GicDistributor,off)&~(2u<<shift);
    write32(platform.GicDistributor,off,value);
    if (read32(platform.GicDistributor,off)&(2u<<shift)) goto release;
    write32(platform.GicCpuInterface,4,0); /* HIGH_LEVEL in hardware */
    write32(platform.GicCpuInterface,8,0);
    write32(platform.GicDistributor,0,1);
    write32(platform.GicCpuInterface,0,1); /* EOI also deactivates */
    ULONG64 frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    stats.Frequency=frequency;
    stats.Initialized=TRUE;
    if (!frequency || HalArm64ReadPriorityMask()!=0) fatal(4);
    return STATUS_SUCCESS;
release:
    MmArm64UnmapBootGic(&mapped);
    return status;
}
/* Start the runtime clock as part of the Phase 1 HAL path. Preserve the
 * Phase 0 GIC mappings, SGI routing and boot IRQ counters across the handoff. */
NTSTATUS HalArm64InitializePhase1(PLOADER_PARAMETER_BLOCK loader)
{
    static BOOLEAN phase1_initialized;
    if (!loader || loader != KeLoaderBlock || !stats.Initialized || phase1_initialized ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || PsGetCurrentProcess() != PsInitialSystemProcess)
        return STATUS_INVALID_DEVICE_STATE;

    ULONG timer = 1u << platform.TimerIrq;
    if (stats.TimerActive || !(read32(platform.GicDistributor, 0) & 1) ||
        !(read32(platform.GicCpuInterface, 0) & 1) ||
        (read32(platform.GicDistributor, 0x100) & (SOFTWARE_IRQ_MASK | timer)) != SOFTWARE_IRQ_MASK ||
        (HalArm64ReadTimerControl() & 1) || HalArm64TimerPendingOrActive())
        return STATUS_DEVICE_BUSY;

    NTSTATUS status = KiArm64EnableTimerDispatch();
    if (!NT_SUCCESS(status)) return status;
    if (!(read32(platform.GicDistributor, 0x100) & timer) ||
        (HalArm64ReadTimerControl() & 3) != 1) {
        KiArm64DisableTimerDispatch();
        return STATUS_IO_DEVICE_ERROR;
    }
    phase1_initialized = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS HalArm64StartBootTimer(ULONG hertz)
{
    ULONG64 saved=mask_irq();
    if (!stats.Initialized || stats.TimerActive || !hertz || hertz>stats.Frequency) {
        restore_irq(saved);return STATUS_INVALID_PARAMETER;
    }
    stats.Period=stats.Frequency/hertz;
    deadline=HalArm64ReadCounter()+stats.Period;
    __asm__ volatile("msr cntp_cval_el0, %0\nisb" :: "r"(deadline) : "memory");
    write32(platform.GicDistributor,0x280,1u<<platform.TimerIrq);
    stats.TimerActive=TRUE;
    write32(platform.GicDistributor,0x100,1u<<platform.TimerIrq);
    ULONG64 enable=1;
    __asm__ volatile("msr cntp_ctl_el0, %0\nisb" :: "r"(enable) : "memory");
    restore_irq(saved);
    return STATUS_SUCCESS;
}
VOID HalArm64StopBootTimer(VOID)
{
    ULONG64 saved=mask_irq();
    if (!stats.Initialized) fatal(5);
    __asm__ volatile("msr cntp_ctl_el0, xzr\nisb" ::: "memory");
    write32(platform.GicDistributor,0x180,1u<<platform.TimerIrq);
    write32(platform.GicDistributor,0x280,1u<<platform.TimerIrq);
    stats.TimerActive=FALSE;
    restore_irq(saved);
}
ULONG HalArm64TimerPendingOrActive(VOID)
{
    if (!stats.Initialized) fatal(6);
    ULONG mask=1u<<platform.TimerIrq;
    return (read32(platform.GicDistributor,0x200)|read32(platform.GicDistributor,0x300))&mask;
}
VOID HalArm64StartProfileInterrupt(KPROFILE_SOURCE source)
{
    if (source != ProfileTime || profile_time || KeGetCurrentIrql() != DISPATCH_LEVEL)
        fatal(16);
    profile_time = TRUE;
}

VOID HalArm64StopProfileInterrupt(KPROFILE_SOURCE source)
{
    if (source != ProfileTime || !profile_time || KeGetCurrentIrql() != DISPATCH_LEVEL)
        fatal(16);
    profile_time = FALSE;
}

VOID KiRequestSoftwareInterrupt(KIRQL level)
{
    if (!stats.Initialized || (level!=APC_LEVEL && level!=DISPATCH_LEVEL)) fatal(12);
    /* Publish the scheduler/APC queue writes before ringing the distributor. */
    __asm__ volatile("dsb ishst" ::: "memory");
    write32(platform.GicDistributor,GICD_SGIR,GICD_SGIR_SELF | level);
}
ULONG HalArm64SoftwareIrqPending(VOID)
{
    if (!stats.Initialized) fatal(13);
    return read32(platform.GicDistributor,0x200)&SOFTWARE_IRQ_MASK;
}
ULONG HalArm64SoftwareIrqActive(VOID)
{
    if (!stats.Initialized) fatal(14);
    return read32(platform.GicDistributor,0x300)&SOFTWARE_IRQ_MASK;
}
BOOLEAN HalArm64HandleBootIrq(PKTRAP_FRAME frame)
{
    if (!stats.Initialized || !frame || (frame->Cpsr&31)!=5) return FALSE;
    ULONG acknowledge=read32(platform.GicCpuInterface,0xc);
    ULONG id=acknowledge&1023;
    if (id==1023) { ++stats.Spurious;return TRUE; }
    if (id>=1020) { stats.LastIntId=id;stats.LastFailure=10;return FALSE; }
    stats.LastIntId=id;
    KIRQL old=frame->PreviousIrql;
    if (old!=KeGetCurrentIrql()) fatal(8);
    if (id==APC_LEVEL || id==DISPATCH_LEVEL) {
        if (old>=id || !KeGetCurrentThread()) fatal(15);
        KIRQL previous;
        KeRaiseIrql((KIRQL)id,&previous);
        if (previous!=old) fatal(8);
        /* Context switching may suspend this frame. Retire the whole IAR
         * token, including its source CPU bits, before running NT callbacks. */
        write32(platform.GicCpuInterface,0x10,acknowledge);
        if (id==DISPATCH_LEVEL) {
            ++stats.DispatchInterrupts;
            KiArm64DispatchInterrupt(old);
        } else {
            ++stats.ApcInterrupts;
            _enable();
            if (KeGetCurrentThread()->ApcState.KernelApcPending)
                KiDeliverApc(KernelMode, NULL, NULL);
            _disable();
            KeLowerIrql(old);
        }
        return TRUE;
    }
    if (id!=platform.TimerIrq || !stats.TimerActive) {
        write32(platform.GicDistributor,0x180+(id/32)*4,1u<<(id%32));
        write32(platform.GicCpuInterface,0x10,acknowledge);
        stats.LastFailure=7;return FALSE;
    }
    if (old>=CLOCK_LEVEL) fatal(8);
    KIRQL previous;
    KeRaiseIrql(CLOCK_LEVEL,&previous);
    if (previous!=old) fatal(8);
    ULONG64 now=HalArm64ReadCounter();
    if (!(HalArm64ReadTimerControl()&4) || (LONG64)(now-deadline)<0) fatal(9);
    ULONG64 periods=(now-deadline)/stats.Period+1;
    deadline+=periods*stats.Period;
    __asm__ volatile("msr cntp_cval_el0, %0\nisb" :: "r"(deadline) : "memory");
    stats.MissedPeriods+=periods-1;
    stats.LastCounter=now;
    stats.LastInterruptedIrql=old;
    ++stats.Interrupts;
    if (!NT_SUCCESS(KiArm64UpdateBootClock(now))) fatal(11);
    if (profile_time) KiArm64ProfileClockInterrupt(frame);
    KiArm64ClockTimerInterrupt(frame);
    /* Retire the actual IAR token after deasserting the timer source. */
    __asm__ volatile("dsb sy" ::: "memory");
    write32(platform.GicCpuInterface,0x10,acknowledge);
    KeLowerIrql(old);
    return TRUE;
}
VOID HalArm64GetIrqStats(HAL_ARM64_IRQ_STATS *out)
{
    ULONG64 saved=mask_irq();
    out->Frequency=stats.Frequency;out->Period=stats.Period;
    out->Interrupts=stats.Interrupts;out->Spurious=stats.Spurious;
    out->DispatchInterrupts=stats.DispatchInterrupts;out->ApcInterrupts=stats.ApcInterrupts;
    out->MissedPeriods=stats.MissedPeriods;out->LastCounter=stats.LastCounter;
    out->LastInterruptedIrql=stats.LastInterruptedIrql;out->LastIntId=stats.LastIntId;
    out->LastFailure=stats.LastFailure;out->Initialized=stats.Initialized;out->TimerActive=stats.TimerActive;
    out->GicDistributorVa=stats.Initialized ? platform.GicDistributor : 0;
    out->GicCpuInterfaceVa=stats.Initialized ? platform.GicCpuInterface : 0;
    restore_irq(saved);
}
