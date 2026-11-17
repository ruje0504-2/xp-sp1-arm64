#ifndef XP_ARM64_BOOT_IMAGE_H
#define XP_ARM64_BOOT_IMAGE_H
#define XP_BOOT_IMAGE_SECTIONS 16
#define XP_ARM64_KERNEL_LOAD_VA 0xfffff80000000000ULL
#define XP_BOOT_IMAGE_LIMIT (16u * 1024 * 1024)

typedef struct {
    ULONG Rva, Size, Characteristics;
} XP_ARM64_IMAGE_SECTION;

typedef struct _XP_ARM64_BOOT_IMAGE {
    ULONG64 Base, VirtualBase, PreferredBase, FileSize;
    ULONG Size, HeaderSize, EntryRva, CheckSum, SectionCount, Relocations;
    XP_ARM64_IMAGE_SECTION Sections[XP_BOOT_IMAGE_SECTIONS];
} XP_ARM64_BOOT_IMAGE, *PXP_ARM64_BOOT_IMAGE;

#endif
