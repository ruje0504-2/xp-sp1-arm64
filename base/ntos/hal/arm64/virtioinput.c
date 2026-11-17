/* VirtIO 1.2 modern MMIO and split input eventq on coherent QEMU virt.
 * The device owns device-writable descriptors until it publishes used.idx. */
#include "virtioinput.h"
#include "gicv2.h"
#include "../../mm/arm64/bootframes.h"
#include "../../ex/arm64/bootwin32.h"
#include "../../ex/arm64/bootworkers.h"

enum {
    MmioMagic = 0x000, MmioVersion = 0x004, MmioDeviceId = 0x008,
    MmioDeviceFeatures = 0x010, MmioDeviceFeaturesSel = 0x014,
    MmioDriverFeatures = 0x020, MmioDriverFeaturesSel = 0x024,
    MmioQueueSel = 0x030, MmioQueueMax = 0x034, MmioQueueNum = 0x038,
    MmioQueueReady = 0x044, MmioNotify = 0x050, MmioInterrupt = 0x060,
    MmioAck = 0x064, MmioStatus = 0x070, MmioDesc = 0x080,
    MmioAvail = 0x090, MmioUsed = 0x0a0, MmioGeneration = 0x0fc,
    MmioConfig = 0x100
};
enum {
    VirtioAcknowledge = 1, VirtioDriver = 2, VirtioDriverOk = 4,
    VirtioFeaturesOk = 8, VirtioNeedsReset = 64, VirtioFailed = 128,
    InputQueueLength = 16
};

typedef struct { ULONG64 Address; ULONG Length; USHORT Flags, Next; } INPUT_DESCRIPTOR;
typedef struct { USHORT Flags, Index, Ring[InputQueueLength]; } INPUT_AVAILABLE;
typedef struct { ULONG Id, Length; } INPUT_USED_ENTRY;
typedef struct { USHORT Flags, Index; INPUT_USED_ENTRY Ring[InputQueueLength]; } INPUT_USED;
typedef struct { USHORT Type, Code; LONG Value; } INPUT_RECORD;
typedef struct {
    INPUT_DESCRIPTOR Desc[InputQueueLength];
    INPUT_AVAILABLE Avail;
    INPUT_USED Used;
    INPUT_RECORD Events[InputQueueLength];
} INPUT_PAGE;

typedef struct {
    volatile UCHAR *Regs;
    volatile INPUT_PAGE *Queue;
    PFN_NUMBER Frame;
    KDPC Dpc;
    WORK_QUEUE_ITEM Work;
    ULONG Irq;
    USHORT Consumed;
    BOOLEAN Keyboard, Connected, WorkPending, Failed;
    ULONG64 Interrupts, Events, Faults;
} INPUT_DEVICE;

_Static_assert(sizeof(INPUT_RECORD) == 8 && sizeof(INPUT_DESCRIPTOR) == 16, "VirtIO layouts");
_Static_assert(sizeof(INPUT_PAGE) <= PAGE_SIZE && FIELD_OFFSET(INPUT_PAGE, Used) % 4 == 0, "split ring");

static INPUT_DEVICE keyboard, mouse;
static NTSTATUS start_status = STATUS_PENDING;

static ULONG reg_read(const INPUT_DEVICE *dev, ULONG offset)
{
    ULONG value = *(const volatile ULONG *)(dev->Regs + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static VOID reg_write(const INPUT_DEVICE *dev, ULONG offset, ULONG value)
{
    *(volatile ULONG *)(dev->Regs + offset) = value;
    __asm__ volatile("dsb osh" ::: "memory");
}

static VOID reg_write_address(const INPUT_DEVICE *dev, ULONG offset, ULONG64 address)
{
    reg_write(dev, offset, (ULONG)address);
    reg_write(dev, offset + 4, (ULONG)(address >> 32));
}

static BOOLEAN input_capable(const INPUT_DEVICE *dev, UCHAR type, ULONG code)
{
    for (ULONG retry = 0; retry < 4; ++retry) {
        ULONG generation = reg_read(dev, MmioGeneration);
        dev->Regs[MmioConfig] = 0x11; /* VIRTIO_INPUT_CFG_EV_BITS */
        dev->Regs[MmioConfig + 1] = type;
        __asm__ volatile("dsb osh" ::: "memory");
        ULONG bytes = dev->Regs[MmioConfig + 2];
        BOOLEAN present = bytes <= 128 && code / 8 < bytes &&
                          (dev->Regs[MmioConfig + 8 + code / 8] & (1u << (code % 8)));
        if (reg_read(dev, MmioGeneration) == generation) return present;
    }
    return FALSE;
}

static VOID receive_worker(PVOID context)
{
    INPUT_DEVICE *dev = context;
    for (;;) {
        INPUT_RECORD batch[InputQueueLength];
        KIRQL old;
        KeRaiseIrql(HAL_ARM64_BOOT_DEVICE_LEVEL, &old);
        volatile INPUT_PAGE *queue = dev->Queue;
        USHORT count = (USHORT)(queue->Used.Index - dev->Consumed);
        __asm__ volatile("dmb oshld" ::: "memory");
        if (count > InputQueueLength || (reg_read(dev, MmioStatus) & VirtioNeedsReset))
            dev->Failed = TRUE;
        ULONG seen = 0;
        for (ULONG i = 0; !dev->Failed && i < count; ++i) {
            ULONG slot = (dev->Consumed + i) % InputQueueLength;
            ULONG id = queue->Used.Ring[slot].Id;
            if (id >= InputQueueLength || (seen & (1u << id)) ||
                queue->Used.Ring[slot].Length != sizeof(INPUT_RECORD)) {
                dev->Failed = TRUE;
                break;
            }
            seen |= 1u << id;
            batch[i] = queue->Events[id];
            queue->Avail.Ring[(queue->Avail.Index + i) % InputQueueLength] = (USHORT)id;
        }
        if (dev->Failed) {
            ++dev->Faults;
            reg_write(dev, MmioStatus, reg_read(dev, MmioStatus) | VirtioFailed);
        }
        if (!count || dev->Failed) {
            dev->WorkPending = FALSE;
            KeLowerIrql(old);
            return;
        }
        dev->Consumed += count;
        __asm__ volatile("dmb oshst" ::: "memory");
        queue->Avail.Index += count;
        __asm__ volatile("dmb oshst" ::: "memory");
        reg_write(dev, MmioNotify, 0);
        dev->Events += count;
        KeLowerIrql(old);
        for (ULONG i = 0; i < count; ++i)
            ExpArm64RouteBootInput(dev->Keyboard, batch[i].Type, batch[i].Code, batch[i].Value);
    }
}

static VOID receive_dpc(PKDPC dpc, PVOID context, PVOID unused1, PVOID unused2)
{
    INPUT_DEVICE *dev = context;
    KIRQL old;
    KeRaiseIrql(HAL_ARM64_BOOT_DEVICE_LEVEL, &old);
    BOOLEAN queue = !dev->WorkPending && !dev->Failed;
    if (queue) dev->WorkPending = TRUE;
    KeLowerIrql(old);
    if (queue) ExQueueWorkItem(&dev->Work, CriticalWorkQueue);
}

static VOID input_interrupt(PVOID context)
{
    INPUT_DEVICE *dev = context;
    ULONG status = reg_read(dev, MmioInterrupt) & 3;
    if (!status) return;
    reg_write(dev, MmioAck, status);
    ++dev->Interrupts;
    if (!dev->Failed) KeInsertQueueDpc(&dev->Dpc, NULL, NULL);
}

static BOOLEAN reset_device(INPUT_DEVICE *dev)
{
    reg_write(dev, MmioStatus, 0);
    for (ULONG attempt = 0; attempt < 10000; ++attempt)
        if (!reg_read(dev, MmioStatus)) return TRUE;
    return FALSE;
}

static NTSTATUS allocate_queue(INPUT_DEVICE *dev)
{
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    NTSTATUS status = MmArm64TakeBootFrame(dev, &dev->Frame);
    if (NT_SUCCESS(status)) {
        ULONG64 physical = (ULONG64)dev->Frame << PAGE_SHIFT;
        RtlZeroMemory((PVOID)(ULONG_PTR)physical, PAGE_SIZE);
        dev->Queue = (volatile INPUT_PAGE *)(ULONG_PTR)physical;
    }
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
    return status;
}

static VOID release_queue(INPUT_DEVICE *dev)
{
    if (!dev->Queue) return;
    ULONG64 daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(daif) :: "memory");
    if (!NT_SUCCESS(MmArm64ReturnBootFrame(dev, dev->Frame)))
        KeBugCheckEx(MEMORY_MANAGEMENT, 0xa618, dev->Frame, (ULONG_PTR)dev, 0);
    dev->Queue = NULL;
    __asm__ volatile("msr daif, %0\nisb" :: "r"(daif) : "memory");
}

static NTSTATUS start_device(INPUT_DEVICE *dev, const HAL_ARM64_VIRTIO_MMIO *resource)
{
    if (!reset_device(dev)) return STATUS_IO_DEVICE_ERROR;
    ULONG status = VirtioAcknowledge | VirtioDriver;
    reg_write(dev, MmioStatus, VirtioAcknowledge);
    reg_write(dev, MmioStatus, status);
    reg_write(dev, MmioDeviceFeaturesSel, 1);
    if (!(reg_read(dev, MmioDeviceFeatures) & 1)) return STATUS_NOT_SUPPORTED;
    reg_write(dev, MmioDriverFeaturesSel, 0);
    reg_write(dev, MmioDriverFeatures, 0);
    reg_write(dev, MmioDriverFeaturesSel, 1);
    reg_write(dev, MmioDriverFeatures, 1); /* VIRTIO_F_VERSION_1 only. */
    status |= VirtioFeaturesOk;
    reg_write(dev, MmioStatus, status);
    if (!(reg_read(dev, MmioStatus) & VirtioFeaturesOk)) return STATUS_NOT_SUPPORTED;
    reg_write(dev, MmioQueueSel, 0);
    if (reg_read(dev, MmioQueueReady) || reg_read(dev, MmioQueueMax) < InputQueueLength)
        return STATUS_NOT_SUPPORTED;
    NTSTATUS result = allocate_queue(dev);
    if (!NT_SUCCESS(result)) return result;

    ULONG64 base = (ULONG64)dev->Frame << PAGE_SHIFT;
    volatile INPUT_PAGE *queue = dev->Queue;
    for (ULONG i = 0; i < InputQueueLength; ++i) {
        queue->Desc[i].Address = base + FIELD_OFFSET(INPUT_PAGE, Events) + i * sizeof(INPUT_RECORD);
        queue->Desc[i].Length = sizeof(INPUT_RECORD);
        queue->Desc[i].Flags = 2; /* VIRTQ_DESC_F_WRITE */
        queue->Avail.Ring[i] = (USHORT)i;
    }
    __asm__ volatile("dmb oshst" ::: "memory");
    queue->Avail.Index = InputQueueLength;
    __asm__ volatile("dmb oshst" ::: "memory");
    reg_write(dev, MmioQueueNum, InputQueueLength);
    reg_write_address(dev, MmioDesc, base + FIELD_OFFSET(INPUT_PAGE, Desc));
    reg_write_address(dev, MmioAvail, base + FIELD_OFFSET(INPUT_PAGE, Avail));
    reg_write_address(dev, MmioUsed, base + FIELD_OFFSET(INPUT_PAGE, Used));
    reg_write(dev, MmioQueueReady, 1);
    if (!reg_read(dev, MmioQueueReady)) return STATUS_IO_DEVICE_ERROR;
    KeInitializeDpc(&dev->Dpc, receive_dpc, dev);
    ExInitializeWorkItem(&dev->Work, receive_worker, dev);
    reg_write(dev, MmioStatus, status | VirtioDriverOk);
    if (reg_read(dev, MmioStatus) != (status | VirtioDriverOk)) return STATUS_IO_DEVICE_ERROR;
    result = HalArm64ConnectBootSpi(resource->Irq, resource->Trigger, input_interrupt, dev);
    if (!NT_SUCCESS(result)) return result;
    dev->Irq = resource->Irq;
    dev->Connected = TRUE;
    reg_write(dev, MmioNotify, 0);
    return STATUS_SUCCESS;
}

NTSTATUS HalArm64InitializeVirtioInput(VOID)
{
    if (start_status != STATUS_PENDING) return start_status;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !KeLoaderBlock ||
        !ExWorkerQueue[CriticalWorkQueue].Info.WorkerCount) return STATUS_INVALID_DEVICE_STATE;
    const XP_ARM64_BOOT_BLOCK *block = CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt);
    if (!NT_SUCCESS(block->VirtioStatus) || block->Virtio.Count > HAL_ARM64_VIRTIO_MMIO_LIMIT) {
        start_status = STATUS_NOT_FOUND;
        return start_status;
    }

    start_status = STATUS_NOT_FOUND;
    for (ULONG i = 0; i < block->Virtio.Count; ++i) {
        const HAL_ARM64_VIRTIO_MMIO *resource = &block->Virtio.Devices[i];
        if (resource->Bytes < 0x188) continue;
        PHYSICAL_ADDRESS physical = {.QuadPart = resource->Physical};
        INPUT_DEVICE probe = {.Regs = MmMapIoSpace(physical, resource->Bytes, MmNonCached)};
        if (!probe.Regs) continue;
        INPUT_DEVICE *dev = NULL;
        if (reg_read(&probe, MmioMagic) == 0x74726976 && reg_read(&probe, MmioVersion) == 2 &&
            reg_read(&probe, MmioDeviceId) == 18) {
            if (!keyboard.Regs && input_capable(&probe, 1, 30)) dev = &keyboard;
            else if (!mouse.Regs && input_capable(&probe, 2, 0) && input_capable(&probe, 2, 1) &&
                     input_capable(&probe, 1, 0x110)) dev = &mouse;
        }
        if (dev) {
            dev->Regs = probe.Regs;
            dev->Keyboard = dev == &keyboard;
            start_status = start_device(dev, resource);
            if (NT_SUCCESS(start_status)) {
                if (keyboard.Connected && mouse.Connected) break;
                continue;
            }
            if (!reset_device(dev))
                KeBugCheckEx(DRIVER_CORRUPTED_MMPOOL, 0xa618, (ULONG_PTR)dev, start_status, 0);
            release_queue(dev);
            dev->Regs = NULL;
        }
        MmUnmapIoSpace((PVOID)probe.Regs, resource->Bytes);
    }
    if (keyboard.Connected || mouse.Connected) start_status = STATUS_SUCCESS;
    return start_status;
}

VOID HalArm64GetInputStats(HAL_ARM64_INPUT_STATS *stats)
{
    const XP_ARM64_BOOT_BLOCK *block = CONTAINING_RECORD(KeLoaderBlock, XP_ARM64_BOOT_BLOCK, Nt);
    KIRQL old;
    KeRaiseIrql(HAL_ARM64_BOOT_DEVICE_LEVEL, &old);
    *stats = (HAL_ARM64_INPUT_STATS){
        block->VirtioStatus, start_status, block->Virtio.Count, keyboard.Irq, mouse.Irq,
        keyboard.Interrupts, mouse.Interrupts, keyboard.Events, mouse.Events, keyboard.Faults + mouse.Faults
    };
    KeLowerIrql(old);
}
