#include <ntos.h>
#include "../base/ntos/mm/arm64/userspace.h"
#include "uefi_test.h"

ULONG64 XpQueryUserTranslation(ULONG64 Root, ULONG_PTR Va, BOOLEAN Write);

VOID XpCheckUserSpaces(PXP_ARM64_BOOT_BLOCK boot)
{
    MM_ARM64_USER_SPACE a = {0}, b = {0};
    const ULONG_PTR va = 0x3ffff000;
    const ULONG64 mask = 0x0000fffffffff000ULL;
    ULONG64 value = 0x123456789abcdef0ULL, copy = 1;
    MM_ARM64_BOOT_PFN_COUNTS before = MmArm64BootFrameCounts();
    CHECK(NT_SUCCESS(MmArm64CreateUserSpace(&a, boot)));
    CHECK(NT_SUCCESS(MmArm64CreateUserSpace(&b, boot)));
    CHECK(NT_SUCCESS(MmArm64MapUserPages(&a, va, 2 * PAGE_SIZE, PAGE_READWRITE)));
    CHECK(NT_SUCCESS(MmArm64MapUserPages(&b, va, 2 * PAGE_SIZE, PAGE_READONLY)));
    ULONG64 first = XpQueryUserTranslation(a.Root, va, TRUE);
    ULONG64 second = XpQueryUserTranslation(b.Root, va, FALSE);
    CHECK(!(first & 1) && !(second & 1) && (first & mask) != (second & mask));
    CHECK(XpQueryUserTranslation(b.Root, va, TRUE) & 1);
    CHECK(XpQueryUserTranslation(a.Root, KI_USER_SHARED_DATA, FALSE) & 1);
    CHECK(XpQueryUserTranslation(a.Root, 0, FALSE) & 1);
    CHECK(XpQueryUserTranslation(a.Root, boot->Tables.PoolBase, FALSE) & 1);
    ULONG64 shared = XpQueryUserTranslation(a.Root, MM_ARM64_USER_SHARED_VA, FALSE);
    CHECK(!(shared & 1) && (shared & mask) == boot->SharedPagePfn << PAGE_SHIFT);
    CHECK(XpQueryUserTranslation(a.Root, MM_ARM64_USER_SHARED_VA, TRUE) & 1);
    CHECK(NT_SUCCESS(MmArm64CopyUserBuffer(&a, va + PAGE_SIZE - 3, &value, sizeof(value), TRUE)));
    CHECK(NT_SUCCESS(MmArm64CopyUserBuffer(&b, va + PAGE_SIZE - 3, &copy, sizeof(copy), FALSE)) && copy == 0);
    CHECK(NT_SUCCESS(MmArm64CopyUserBuffer(&a, va + PAGE_SIZE - 3, &copy, sizeof(copy), FALSE)) && copy == value);
    CHECK(MmArm64MapUserPages(&a, va - PAGE_SIZE, PAGE_SIZE * 2, PAGE_READWRITE) == STATUS_CONFLICTING_ADDRESSES);
    CHECK(XpQueryUserTranslation(a.Root, va - PAGE_SIZE, FALSE) & 1);
    CHECK(NT_SUCCESS(MmArm64ProtectUserPages(&a, va, PAGE_SIZE, PAGE_EXECUTE_READ)));
    CHECK(XpQueryUserTranslation(a.Root, va, TRUE) & 1);
    CHECK(MmArm64CopyUserBuffer(&a, va, &copy, sizeof(copy), TRUE) == STATUS_ACCESS_VIOLATION);
    CHECK(NT_SUCCESS(MmArm64UnmapUserPages(&a, va, 2 * PAGE_SIZE)));
    CHECK(XpQueryUserTranslation(a.Root, va, FALSE) & 1);
    CHECK(!(XpQueryUserTranslation(b.Root, va, FALSE) & 1));
    MmArm64DestroyUserSpace(&b);
    MmArm64DestroyUserSpace(&a);
    MM_ARM64_BOOT_PFN_COUNTS after = MmArm64BootFrameCounts();
    CHECK(after.Free == before.Free && after.Allocated == before.Allocated);
    XpTestReport("PASS: private low user roots, shared XP page, protection and PFN reclamation\n");
}
