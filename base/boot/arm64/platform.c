/* Public ACPI RSDP/XSDT, MADT GICC/GICD and GTDT fields. No AML or
 * firmware-private object traversal. All followed spans must be mapped. */
#include "platform.h"
#include "../../ntos/mm/arm64/bootmap.h"
typedef struct { UCHAR Guid[16]; PVOID Table; } CONFIG_ENTRY;
_Static_assert(sizeof(CONFIG_ENTRY)==24, "UEFI configuration entry");
static const UCHAR acpi_guid[16] = {
    0x71,0xe8,0x68,0x88,0xf1,0xe4,0xd3,0x11,0xbc,0x22,0,0x80,0xc7,0x3c,0x88,0x81
};
static ULONG le32(const UCHAR *p)
{
    return (ULONG)p[0] | (ULONG)p[1]<<8 | (ULONG)p[2]<<16 | (ULONG)p[3]<<24;
}
static ULONG64 le64(const UCHAR *p) { return le32(p) | (ULONG64)le32(p+4)<<32; }
static BOOLEAN same(const UCHAR *a, const UCHAR *b, ULONG n)
{
    for (ULONG i=0;i<n;++i) if (a[i]!=b[i]) return FALSE;
    return TRUE;
}
static BOOLEAN mapped(ULONG_PTR at, ULONG size)
{
    if (!at || !size || at > ~(ULONG_PTR)0 - size) return FALSE;
    ULONG_PTR end = at + size - 1;
    for (ULONG_PTR page=at & ~(ULONG_PTR)4095;;page+=PAGE_SIZE) {
        if (MmArm64QueryPhysicalAddress(page)!=page) return FALSE;
        if (end-page < PAGE_SIZE) break;
    }
    return TRUE;
}
static BOOLEAN checksum(const UCHAR *p, ULONG n)
{
    UCHAR sum=0;
    for (ULONG i=0;i<n;++i) sum=(UCHAR)(sum+p[i]);
    return sum==0;
}
static BOOLEAN table(const UCHAR *p, const char *signature, ULONG minimum, PULONG size)
{
    if (!mapped((ULONG_PTR)p,36) || !same(p,(const UCHAR *)signature,4)) return FALSE;
    *size=le32(p+4);
    return *size>=minimum && *size<=1024*1024 && mapped((ULONG_PTR)p,*size) && checksum(p,*size);
}
NTSTATUS BlArm64DiscoverAcpiPlatform(XP_EFI_SYSTEM_TABLE *system, HAL_ARM64_BOOT_PLATFORM *result)
{
    if (!system || !result || !system->NumberOfTableEntries ||
        system->NumberOfTableEntries>128 ||
        !mapped((ULONG_PTR)system->ConfigurationTable,(ULONG)system->NumberOfTableEntries*sizeof(CONFIG_ENTRY)))
        return STATUS_INVALID_PARAMETER;
    const CONFIG_ENTRY *entries=system->ConfigurationTable;
    const UCHAR *rsdp=NULL;
    for (ULONG_PTR i=0;i<system->NumberOfTableEntries;++i)
        if (same(entries[i].Guid,acpi_guid,16)) { rsdp=entries[i].Table; break; }
    if (!rsdp) return STATUS_NOT_FOUND;
    if (!mapped((ULONG_PTR)rsdp,36) || !same(rsdp,(const UCHAR *)"RSD PTR ",8) ||
        rsdp[15]<2 || !checksum(rsdp,20)) return STATUS_INVALID_PARAMETER;
    ULONG rsdp_length=le32(rsdp+20);
    if (rsdp_length<36 || rsdp_length>4096 || !mapped((ULONG_PTR)rsdp,rsdp_length) ||
        !checksum(rsdp,rsdp_length)) return STATUS_INVALID_PARAMETER;
    const UCHAR *xsdt=(PVOID)(ULONG_PTR)le64(rsdp+24);
    ULONG size;
    if (!table(xsdt,"XSDT",36,&size) || (size-36)%8 || (size-36)/8>128)
        return STATUS_INVALID_PARAMETER;
    const UCHAR *madt=NULL,*gtdt=NULL;
    for (ULONG at=36;at<size;at+=8) {
        const UCHAR *candidate=(PVOID)(ULONG_PTR)le64(xsdt+at);
        if (!mapped((ULONG_PTR)candidate,36)) return STATUS_INVALID_PARAMETER;
        if (same(candidate,(const UCHAR *)"APIC",4)) madt=candidate;
        if (same(candidate,(const UCHAR *)"GTDT",4)) gtdt=candidate;
    }
    if (!madt || !gtdt) return STATUS_NOT_FOUND;
    ULONG madt_size,gtdt_size;
    if (!table(madt,"APIC",44,&madt_size) || !table(gtdt,"GTDT",80,&gtdt_size))
        return STATUS_INVALID_PARAMETER;
    HAL_ARM64_BOOT_PLATFORM found;
    RtlZeroMemory(&found,sizeof(found));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(found.Mpidr));
    BOOLEAN cpu=FALSE,dist=FALSE;
    for (ULONG at=44;at<madt_size;) {
        if (madt_size-at<2 || madt[at+1]<2 || madt[at+1]>madt_size-at)
            return STATUS_INVALID_PARAMETER;
        const UCHAR *entry=madt+at;
        ULONG length=entry[1];
        if (entry[0]==11 && length>=76 && (le32(entry+12)&1) &&
            (le64(entry+68)&0xff00ffffffULL)==(found.Mpidr&0xff00ffffffULL)) {
            if (cpu) return STATUS_INVALID_PARAMETER;
            found.GicCpuInterface=le64(entry+32);
            found.CpuInterfaceNumber=le32(entry+4);
            cpu=TRUE;
        }
        if (entry[0]==12 && length>=24) {
            if (dist) return STATUS_NOT_SUPPORTED;
            found.GicDistributor=le64(entry+8);
            found.GicVersion=entry[20]; dist=TRUE;
        }
        at+=length;
    }
    found.TimerIrq=le32(gtdt+56);
    found.TimerFlags=le32(gtdt+60);
    found.MadtRevision=madt[8]; found.GtdtRevision=gtdt[8];
    if (!cpu || !dist) return STATUS_NOT_FOUND;
    if (found.GicVersion!=2 || found.TimerIrq<16 || found.TimerIrq>=32 ||
        (found.TimerFlags&1) || !found.GicDistributor || !found.GicCpuInterface ||
        ((found.GicDistributor|found.GicCpuInterface)&4095))
        return STATUS_NOT_SUPPORTED;
    *result=found;
    return STATUS_SUCCESS;
}

/* DTSpec v17. The boot loader admits only the QEMU virt root's directly
 * addressed coherent transports and confirms their GIC against ACPI. */
#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u

static const UCHAR fdt_guid[16] = {
    0xd5,0x21,0xb6,0xb1,0x9c,0xf1,0xa5,0x41,0x83,0x0b,0xd9,0x15,0x2c,0x69,0xaa,0xe0
};

typedef struct {
    const UCHAR *Compatible, *Reg, *IrqSpec;
    ULONG CompatibleBytes, RegBytes, IrqBytes;
    ULONG Parent, Phandle, AddressCells, SizeCells, InterruptCells;
    BOOLEAN Coherent, Disabled;
} FDT_NODE;

static ULONG be32(const UCHAR *p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

static ULONG64 be64(const UCHAR *p)
{
    return (ULONG64)be32(p) << 32 | be32(p + 4);
}

static ULONG fdt_string_size(const UCHAR *p, ULONG bytes)
{
    for (ULONG i = 0; i < bytes; ++i)
        if (!p[i]) return i + 1;
    return 0;
}

static BOOLEAN fdt_matches(const UCHAR *p, ULONG bytes, const char *name, ULONG size)
{
    if (size < 1) return FALSE;
    for (ULONG offset = 0; offset < bytes;) {
        ULONG span = fdt_string_size(p + offset, bytes - offset);
        if (!span) return FALSE;
        if (span == size && same(p + offset, (const UCHAR *)name, span)) return TRUE;
        offset += span;
    }
    return FALSE;
}

#define FDT_NAME(p, n, literal) ((n) == sizeof(literal) && same((p), (const UCHAR *)(literal), sizeof(literal)))
#define FDT_COMPAT(node, literal) fdt_matches((node)->Compatible, (node)->CompatibleBytes, literal, sizeof(literal))

static VOID fdt_node_property(FDT_NODE *node, const UCHAR *name, ULONG chars, const UCHAR *data, ULONG bytes)
{
    if (FDT_NAME(name, chars, "compatible")) {
        node->Compatible = data;
        node->CompatibleBytes = bytes;
    } else if (FDT_NAME(name, chars, "reg")) {
        node->Reg = data;
        node->RegBytes = bytes;
    } else if (FDT_NAME(name, chars, "interrupts")) {
        node->IrqSpec = data;
        node->IrqBytes = bytes;
    } else if (FDT_NAME(name, chars, "status")) {
        node->Disabled = !FDT_NAME(data, bytes, "okay") && !FDT_NAME(data, bytes, "ok");
    } else if (!bytes && FDT_NAME(name, chars, "dma-coherent")) {
        node->Coherent = TRUE;
    } else if (bytes == 4) {
        ULONG value = be32(data);
        if (FDT_NAME(name, chars, "interrupt-parent")) node->Parent = value;
        else if (FDT_NAME(name, chars, "phandle")) node->Phandle = value;
        else if (FDT_NAME(name, chars, "#address-cells")) node->AddressCells = value;
        else if (FDT_NAME(name, chars, "#size-cells")) node->SizeCells = value;
        else if (FDT_NAME(name, chars, "#interrupt-cells")) node->InterruptCells = value;
    }
}

static BOOLEAN fdt_add_transport(const FDT_NODE *parent, const FDT_NODE *node, HAL_ARM64_VIRTIO_BUS *bus)
{
    if (bus->Count == HAL_ARM64_VIRTIO_MMIO_LIMIT || node->RegBytes != 16 || node->IrqBytes != 12 ||
        !node->Coherent || (node->Parent && node->Parent != parent->Parent) || be32(node->IrqSpec) != 0)
        return FALSE;
    ULONG64 address = be64(node->Reg), bytes = be64(node->Reg + 8);
    ULONG spi = be32(node->IrqSpec + 4), trigger = be32(node->IrqSpec + 8);
    if (!address || address >= (1ULL << 48) || (address & 3) || bytes < 0x108 ||
        bytes > PAGE_SIZE - (address & (PAGE_SIZE - 1)) ||
        spi >= 988 || (trigger != 1 && trigger != 4)) return FALSE;
    ULONG irq = spi + 32;
    for (ULONG i = 0; i < bus->Count; ++i) {
        const HAL_ARM64_VIRTIO_MMIO *other = &bus->Devices[i];
        if (other->Irq == irq || (other->Physical < address + bytes &&
            address < other->Physical + other->Bytes)) return FALSE;
    }
    bus->Devices[bus->Count++] = (HAL_ARM64_VIRTIO_MMIO){address, (ULONG)bytes, irq, trigger};
    return TRUE;
}

static NTSTATUS scan_virtio_fdt(const UCHAR *dtb, const HAL_ARM64_BOOT_PLATFORM *platform,
                                HAL_ARM64_VIRTIO_BUS *out)
{
    if (!mapped((ULONG_PTR)dtb, 40) || be32(dtb) != FDT_MAGIC) return STATUS_NOT_FOUND;
    ULONG total = be32(dtb + 4), pos = be32(dtb + 8), names = be32(dtb + 12);
    ULONG version = be32(dtb + 20), names_bytes = be32(dtb + 32), struct_bytes = be32(dtb + 36);
    if (total < 40 || total > 1024 * 1024 || version < 17 || !mapped((ULONG_PTR)dtb, total) ||
        pos < 40 || (pos & 3) || pos > total || struct_bytes > total - pos || (struct_bytes & 3) ||
        names < pos + struct_bytes || names > total || names_bytes > total - names)
        return STATUS_INVALID_PARAMETER;

    ULONG end = pos + struct_bytes, depth = 0, gic = 0;
    BOOLEAN saw_root = FALSE, closed_root = FALSE;
    FDT_NODE root = {0}, child = {0};
    HAL_ARM64_VIRTIO_BUS bus = {0};
    while (end - pos >= 4) {
        ULONG token = be32(dtb + pos);
        pos += 4;
        if (token == FDT_NOP) continue;
        if (token == FDT_END) {
            if (!closed_root || depth || pos != end || !gic || root.Parent != gic || !bus.Count)
                return STATUS_NOT_FOUND;
            *out = bus;
            return STATUS_SUCCESS;
        }
        if (closed_root) return STATUS_INVALID_PARAMETER;
        if (token == FDT_BEGIN_NODE) {
            ULONG chars = fdt_string_size(dtb + pos, end - pos);
            if (!chars || (!depth && chars != 1) || depth >= 64) return STATUS_INVALID_PARAMETER;
            pos += (chars + 3) & ~3u;
            if (pos > end) return STATUS_INVALID_PARAMETER;
            if (depth == 1) {
                if (!saw_root) {
                    saw_root = TRUE;
                    if (!FDT_COMPAT(&root, "linux,dummy-virt") || root.AddressCells != 2 ||
                        root.SizeCells != 2 || !root.Parent) return STATUS_NOT_SUPPORTED;
                }
                RtlZeroMemory(&child, sizeof(child));
            }
            ++depth;
        } else if (token == FDT_PROP) {
            if (!depth || end - pos < 8 || (depth == 1 && saw_root)) return STATUS_INVALID_PARAMETER;
            ULONG bytes = be32(dtb + pos), name = be32(dtb + pos + 4);
            pos += 8;
            if (bytes > end - pos || name >= names_bytes) return STATUS_INVALID_PARAMETER;
            ULONG chars = fdt_string_size(dtb + names + name, names_bytes - name);
            if (!chars) return STATUS_INVALID_PARAMETER;
            if (depth <= 2) fdt_node_property(depth == 1 ? &root : &child, dtb + names + name, chars, dtb + pos, bytes);
            pos += (bytes + 3) & ~3u;
            if (pos > end) return STATUS_INVALID_PARAMETER;
        } else if (token == FDT_END_NODE) {
            if (!depth) return STATUS_INVALID_PARAMETER;
            if (depth == 2 && !child.Disabled) {
                if (FDT_COMPAT(&child, "arm,cortex-a15-gic") && child.RegBytes >= 32 &&
                    child.InterruptCells == 3 && be64(child.Reg) == platform->GicDistributor &&
                    be64(child.Reg + 16) == platform->GicCpuInterface) {
                    if (gic || !child.Phandle) return STATUS_INVALID_PARAMETER;
                    gic = child.Phandle;
                }
                if (FDT_COMPAT(&child, "virtio,mmio") && !fdt_add_transport(&root, &child, &bus))
                    return STATUS_INVALID_PARAMETER;
            }
            if (!--depth) closed_root = TRUE;
        } else return STATUS_INVALID_PARAMETER;
    }
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS BlArm64DiscoverVirtioMmio(XP_EFI_SYSTEM_TABLE *system, const HAL_ARM64_BOOT_PLATFORM *platform,
                                   HAL_ARM64_VIRTIO_BUS *bus)
{
    if (!system || !platform || !bus || !system->NumberOfTableEntries ||
        system->NumberOfTableEntries > 128 ||
        !mapped((ULONG_PTR)system->ConfigurationTable, (ULONG)system->NumberOfTableEntries * sizeof(CONFIG_ENTRY)))
        return STATUS_INVALID_PARAMETER;
    const CONFIG_ENTRY *entries = system->ConfigurationTable;
    for (ULONG_PTR i = 0; i < system->NumberOfTableEntries; ++i)
        if (same(entries[i].Guid, fdt_guid, sizeof(fdt_guid)))
            return scan_virtio_fdt(entries[i].Table, platform, bus);

    /* QEMU virt provides its DTB at the documented RAM base even when
     * firmware presents ACPI without installing EFI_DTB_TABLE_GUID. */
    return scan_virtio_fdt((const UCHAR *)0x40000000, platform, bus);
}
