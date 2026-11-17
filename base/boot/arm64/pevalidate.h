#ifndef XP_ARM64_PEVALIDATE_H
#define XP_ARM64_PEVALIDATE_H
#include "bootimage.h"
#include <ntimage.h>

static BOOLEAN within(ULONG64 start, ULONG64 length, ULONG64 limit)
{
    return start<=limit && length<=limit-start;
}
static ULONG rounded_page(ULONG length)
{
    return (length+PAGE_SIZE-1)&~(PAGE_SIZE-1);
}
static BOOLEAN in_file_section(const XP_ARM64_BOOT_IMAGE *kernel,
    const IMAGE_SECTION_HEADER *sections, ULONG rva, ULONG bytes)
{
    for (ULONG i=0;i<kernel->SectionCount;++i) {
        ULONG raw=sections[i].SizeOfRawData;
        ULONG size=kernel->Sections[i].Size;
        if (raw>size) raw=size;
        if (rva>=sections[i].VirtualAddress &&
            within(rva-sections[i].VirtualAddress,bytes,raw))
            return TRUE;
    }
    return FALSE;
}
static const IMAGE_NT_HEADERS64 *validate_headers(const UCHAR *file, ULONG64 size,
    PXP_ARM64_BOOT_IMAGE kernel, BOOLEAN user_image)
{
    if (size<sizeof(IMAGE_DOS_HEADER)) return NULL;
    const IMAGE_DOS_HEADER *dos=(const VOID *)file;
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<sizeof(*dos) ||
        (dos->e_lfanew&7) || !within(dos->e_lfanew,sizeof(IMAGE_NT_HEADERS64),size))
        return NULL;
    const IMAGE_NT_HEADERS64 *nt=(const VOID *)(file+dos->e_lfanew);
    const IMAGE_OPTIONAL_HEADER64 *opt=&nt->OptionalHeader;
    ULONG section_count=nt->FileHeader.NumberOfSections;
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_ARM64 ||
        !(nt->FileHeader.Characteristics&IMAGE_FILE_EXECUTABLE_IMAGE) ||
        (nt->FileHeader.Characteristics&IMAGE_FILE_DLL) ||
        (!user_image && (nt->FileHeader.Characteristics&IMAGE_FILE_RELOCS_STRIPPED)) ||
        nt->FileHeader.SizeOfOptionalHeader!=sizeof(*opt) || opt->Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        opt->Subsystem!=IMAGE_SUBSYSTEM_NATIVE || opt->NumberOfRvaAndSizes!=IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        opt->SectionAlignment!=PAGE_SIZE || opt->FileAlignment<512 || opt->FileAlignment>PAGE_SIZE ||
        (opt->FileAlignment&(opt->FileAlignment-1)) || !section_count || section_count>XP_BOOT_IMAGE_SECTIONS ||
        !opt->ImageBase || opt->ImageBase>=(1ULL<<48) || (opt->ImageBase&65535) ||
        !opt->SizeOfImage || opt->SizeOfImage>XP_BOOT_IMAGE_LIMIT || (opt->SizeOfImage&(PAGE_SIZE-1)) ||
        !opt->SizeOfHeaders || opt->SizeOfHeaders>opt->SizeOfImage || opt->SizeOfHeaders>size ||
        (opt->SizeOfHeaders&(opt->FileAlignment-1)) ||
        !within(dos->e_lfanew+sizeof(*nt),section_count*sizeof(IMAGE_SECTION_HEADER),opt->SizeOfHeaders))
        return NULL;

    /* Both early image profiles are self-contained. The kernel requires
     * unwind and relocation data. Preferred-base user images may omit both. */
    for (ULONG i=0;i<IMAGE_NUMBEROF_DIRECTORY_ENTRIES;++i) {
        const IMAGE_DATA_DIRECTORY *directory=&opt->DataDirectory[i];
        if (i==IMAGE_DIRECTORY_ENTRY_EXCEPTION || i==IMAGE_DIRECTORY_ENTRY_BASERELOC) {
            if (user_image && !directory->VirtualAddress && !directory->Size) continue;
            if (!directory->VirtualAddress || !directory->Size ||
                !within(directory->VirtualAddress,directory->Size,opt->SizeOfImage))
                return NULL;
        } else if (directory->VirtualAddress || directory->Size) {
            return NULL;
        }
    }

    const IMAGE_SECTION_HEADER *sections=(const VOID *)(nt+1);
    ULONG end=rounded_page(opt->SizeOfHeaders);
    BOOLEAN valid_entry=FALSE;
    for (ULONG i=0;i<section_count;++i) {
        const IMAGE_SECTION_HEADER *section=&sections[i];
        ULONG virtual_size=section->Misc.VirtualSize;
        ULONG raw=section->SizeOfRawData;
        if (!virtual_size) virtual_size=raw;
        ULONG rva=section->VirtualAddress, flags=section->Characteristics;
        if (!virtual_size || rva<end || (rva&(PAGE_SIZE-1)) ||
            !within(rva,virtual_size,opt->SizeOfImage) || !(flags&IMAGE_SCN_MEM_READ) ||
            ((flags&IMAGE_SCN_MEM_WRITE) && (flags&IMAGE_SCN_MEM_EXECUTE)))
            return NULL;
        if (raw && (section->PointerToRawData<opt->SizeOfHeaders ||
            (section->PointerToRawData&(opt->FileAlignment-1)) ||
            (raw&(opt->FileAlignment-1)) || !within(section->PointerToRawData,raw,size)))
            return NULL;
        kernel->Sections[i]=(XP_ARM64_IMAGE_SECTION){rva,virtual_size,flags};
        end=rva+rounded_page(virtual_size);
        ULONG entry=opt->AddressOfEntryPoint;
        if ((flags&IMAGE_SCN_MEM_EXECUTE) && !(entry&3) && entry>=rva &&
            within(entry-rva,4,virtual_size) && within(entry-rva,4,raw))
            valid_entry=TRUE;
    }
    if (!valid_entry || end!=opt->SizeOfImage) return NULL;

    kernel->FileSize=size;
    kernel->PreferredBase=opt->ImageBase;
    kernel->Size=opt->SizeOfImage;
    kernel->HeaderSize=opt->SizeOfHeaders;
    kernel->EntryRva=opt->AddressOfEntryPoint;
    kernel->CheckSum=opt->CheckSum;
    kernel->SectionCount=section_count;
    const IMAGE_DATA_DIRECTORY *reloc=&opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const IMAGE_DATA_DIRECTORY *unwind=&opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if ((reloc->VirtualAddress&3) || (unwind->VirtualAddress&3) ||
        (unwind->Size&7) || (reloc->Size && !in_file_section(kernel,sections,reloc->VirtualAddress,reloc->Size)) ||
        (unwind->Size && !in_file_section(kernel,sections,unwind->VirtualAddress,unwind->Size)))
        return NULL;
    return nt;
}

#endif
