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
