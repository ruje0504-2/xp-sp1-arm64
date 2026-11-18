/* Resident kernel time publisher. Only the NT_UP boot path and actual
 * clock IRQ may publish. The kernel view is the owned TTBR1 data page. */
#include "ki.h"
#include "clock.h"
#include "../../hal/arm64/gicv2.h"
#include "../../mm/arm64/bootmap.h"

KUSER_SHARED_DATA * const KiArm64SharedUserData=(KUSER_SHARED_DATA *)KI_USER_SHARED_DATA;
static KI_ARM64_CLOCK_STATE clock_state;
ULONG KeMaximumIncrement, KeTimeAdjustment, KiTickOffset;
ULONG ExpTickCountMultiplier;
_Static_assert(sizeof(KUSER_SHARED_DATA)<=PAGE_SIZE, "one resident shared page");

static NTSTATUS validate_shared_page(ULONG64 physical, PULONG64 result, PULONG owner_type)
{
    ULONG64 par=MmArm64QueryTranslation((ULONG_PTR)KiArm64SharedUserData,TRUE);
    /* Require the current coherent Normal-WB/ISH profile, including write
     * access. Other attributes need a separate publication-domain audit. */
    if ((par&1) || ((par>>56)&255)!=255 || ((par>>7)&3)!=3 ||
        (par&0x0000fffffffff000ULL)!=physical || !KeLoaderBlock)
        return STATUS_NOT_SUPPORTED;
    PLIST_ENTRY head=&KeLoaderBlock->MemoryDescriptorListHead,entry=head->Flink;
    for (ULONG i=0;entry!=head && i<1024;++i,entry=entry->Flink) {
        PMEMORY_ALLOCATION_DESCRIPTOR descriptor=
            CONTAINING_RECORD(entry,MEMORY_ALLOCATION_DESCRIPTOR,ListEntry);
        ULONG64 first=(ULONG64)descriptor->BasePage<<PAGE_SHIFT;
        ULONG64 end=first+((ULONG64)descriptor->PageCount<<PAGE_SHIFT);
        if (physical<first || physical+PAGE_SIZE>end) continue;
        if (descriptor->MemoryType!=LoaderMemoryData)
            return STATUS_INVALID_DEVICE_STATE;
        *owner_type=descriptor->MemoryType;*result=par;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_STATE;
}

NTSTATUS KiArm64CounterTo100ns(ULONG64 delta, ULONG64 frequency, PULONG64 value)
{
    if (!value || !frequency || frequency>0xffffffffULL) return STATUS_INVALID_PARAMETER;
    ULONG64 seconds=delta/frequency;
    if (seconds>0x7fffffffffffffffULL/10000000) return STATUS_INTEGER_OVERFLOW;
    ULONG64 result=seconds*10000000;
    ULONG64 fraction=(delta%frequency)*10000000/frequency;
    if (fraction>0x7fffffffffffffffULL-result) return STATUS_INTEGER_OVERFLOW;
    *value=result+fraction;
    return STATUS_SUCCESS;
}
static VOID publish(volatile KSYSTEM_TIME *field, ULONG64 value)
{
    field->High2Time=(LONG)(value>>32);
    __asm__ volatile("dmb ishst" ::: "memory");
    field->LowPart=(ULONG)value;
    __asm__ volatile("dmb ishst" ::: "memory");
    field->High1Time=(LONG)(value>>32);
    __asm__ volatile("dmb ishst" ::: "memory");
}
static BOOLEAN writer_environment(VOID)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return KeNumberProcessors==1 && (daif&0x80) && KeGetCurrentIrql()>=CLOCK_LEVEL;
}
static NTSTATUS advance(ULONG64 now)
{
    if (now<clock_state.LastCounter || now<clock_state.SampleCounter ||
        now>HalArm64ReadCounter()) return STATUS_INVALID_PARAMETER;
    ULONG64 interrupt_time,elapsed;
    NTSTATUS status=KiArm64CounterTo100ns(now-clock_state.BootCounter,
                                         clock_state.Frequency,&interrupt_time);
    if (!NT_SUCCESS(status)) return status;
    status=KiArm64CounterTo100ns(now-clock_state.SampleCounter,
                                clock_state.Frequency,&elapsed);
    if (!NT_SUCCESS(status)) return status;
    if (elapsed>0x7fffffffffffffffULL-clock_state.UtcAtSample)
        return STATUS_INTEGER_OVERFLOW;
    ULONG64 system_time=clock_state.UtcAtSample+elapsed;
    publish(&SharedUserData->InterruptTime,interrupt_time);
    publish(&SharedUserData->SystemTime,system_time);
    clock_state.LastCounter=now;
    clock_state.InterruptTime=interrupt_time;
    clock_state.SystemTime=system_time;
    ++clock_state.Publications;
    return STATUS_SUCCESS;
}
NTSTATUS KiArm64InitializeBootClock(const XP_ARM64_BOOT_TIME *time, ULONG64 physical)
{
    if (!time || clock_state.Initialized || !writer_environment() ||
        KeGetCurrentIrql()!=HIGH_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    ULONG64 frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (time->Status!=STATUS_SUCCESS || time->FirmwareStatus ||
        time->Utc.QuadPart<0 || !frequency || frequency!=time->Frequency ||
        frequency>0xffffffffULL || time->CounterAfter<time->CounterBefore)
        return STATUS_INVALID_PARAMETER;
    /* Verify the whole high page and the retirement of its low alias. */
    ULONG_PTR address=(ULONG_PTR)KiArm64SharedUserData;
    if ((physical&(PAGE_SIZE-1)) || MmArm64QueryPhysicalAddress(address)!=physical ||
        MmArm64QueryPhysicalAddress(address+PAGE_SIZE-1)!=physical+PAGE_SIZE-1 ||
        MmArm64QueryPhysicalAddress(physical)!=~0ULL)
        return STATUS_INVALID_DEVICE_STATE;
    ULONG64 par;
    ULONG owner;
    NTSTATUS status=validate_shared_page(physical,&par,&owner);
    if (!NT_SUCCESS(status)) return status;
    clock_state.Frequency=frequency;
    clock_state.BootCounter=time->CounterBefore;
    clock_state.SampleCounter=time->CounterAfter;
    clock_state.LastCounter=time->CounterAfter;
    clock_state.UtcAtSample=(ULONG64)time->Utc.QuadPart;
    clock_state.SharedPagePhysical=physical;
    clock_state.SharedPagePar=par;
    clock_state.SharedPageLoaderType=owner;
    status=advance(HalArm64ReadCounter());
    if (!NT_SUCCESS(status)) { RtlZeroMemory(&clock_state,sizeof(clock_state));return status; }
    /* The HAL currently offers a fixed 1 ms tick. Restore the XP startup
     * contract before any caller can insert timers or query the increment. */
    KeSetTimeIncrement(KI_ARM64_CLOCK_INCREMENT, KI_ARM64_CLOCK_INCREMENT);
    KiTimeIncrementReciprocal = KiComputeReciprocal((LONG)KeMaximumIncrement, &KiTimeIncrementShiftCount);
    ExpTickCountMultiplier = (ULONG)(((ULONG64)KeMaximumIncrement << 24) / 10000);
    SharedUserData->TickCountMultiplier = ExpTickCountMultiplier;
    clock_state.Initialized=TRUE;
    return STATUS_SUCCESS;
}
/* The reciprocal returned by KiComputeReciprocal is an unsigned bit
 * fraction. UMULH keeps both it and all 64 bits of due time unsigned. */
ULONG64 KiArm64InterruptTicks(ULONG64 time)
{
    ULONG64 high;
    __asm__("umulh %0, %1, %2" : "=r"(high) : "r"(time), "r"((ULONG64)KiTimeIncrementReciprocal.QuadPart));
    return high >> KiTimeIncrementShiftCount;
}

NTSTATUS KiArm64UpdateBootClock(ULONG64 counter)
{
    if (!clock_state.Initialized || !writer_environment()) return STATUS_INVALID_DEVICE_STATE;
    return advance(counter);
}
VOID KiArm64GetBootClockState(KI_ARM64_CLOCK_STATE *state)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2\nisb" : "=r"(daif) :: "memory");
    *state=clock_state;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}
static VOID require_initialized(VOID)
{
    if (!clock_state.Initialized) {
        __asm__ volatile("msr daifset, #15\nbrk #0x7fc" ::: "memory");
        for (;;) __asm__ volatile("wfe");
    }
}
ULONGLONG KeQueryInterruptTime(VOID)
{
    LARGE_INTEGER value;
    require_initialized();
    KiQueryInterruptTime(&value);
    return value.QuadPart;
}
VOID KeQuerySystemTime(PLARGE_INTEGER value)
{
    require_initialized();
    KiQuerySystemTime(value);
}
