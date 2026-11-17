/* Execute the real XP bitmap implementation as a QEMU ARM64 UEFI application.
 * Semihosting is confined to reporting and process exit in this test image. */
#include <nt.h>
#include <ntrtl.h>
#include "../base/ntos/ex/arm64/interlocked.h"

#include "uefi_test.h"
#define MAX_BITS 257u
#define WORDS ((MAX_BITS + 31u) / 32u)
static struct {
    ULONG Before;
    ULONG Data[WORDS];
    ULONG After;
} backing;
static BOOLEAN expected[MAX_BITS];
static ULONG state = 0x735219ab;
static ULONG random_value(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static void verify(PRTL_BITMAP map)
{
    ULONG set = 0;
    for (ULONG bit = 0; bit < map->SizeOfBitMap; ++bit) {
        BOOLEAN actual = (BOOLEAN)((map->Buffer[bit / 32] >> (bit % 32)) & 1u);
        CHECK(actual == expected[bit]);
        set += actual;
    }
    CHECK(RtlNumberOfSetBits(map) == set);
    CHECK(RtlNumberOfClearBits(map) == map->SizeOfBitMap - set);
    CHECK(backing.Before == 0x51a2b3c4 && backing.After == 0x51a2b3c4);
    for (ULONG word = (map->SizeOfBitMap + 31) / 32; word < WORDS; ++word)
        CHECK(backing.Data[word] == 0x51a2b3c4);
}
static BOOLEAN exists(PRTL_BITMAP map, ULONG length, BOOLEAN value)
{
    if (length > map->SizeOfBitMap) return FALSE;
    ULONG run = 0;
    for (ULONG bit = 0; bit < map->SizeOfBitMap; ++bit) {
        run = expected[bit] == value ? run + 1 : 0;
        if (run == length) return TRUE;
    }
    return FALSE;
}
static void check_range(PRTL_BITMAP map, ULONG start, ULONG length, BOOLEAN value)
{
    CHECK(start < map->SizeOfBitMap);
    CHECK(length <= map->SizeOfBitMap - start);
    for (ULONG i = 0; i < length; ++i) CHECK(expected[start + i] == value);
}
static void bitmap_checks(void)
{
    const ULONG sizes[] = {1, 7, 8, 9, 31, 32, 33, 63, 64, 65, 127, 128, 129, 193, 257};
    for (ULONG which = 0; which < sizeof(sizes) / sizeof(sizes[0]); ++which) {
        RTL_BITMAP map;
        ULONG size = sizes[which];
        backing.Before = backing.After = 0x51a2b3c4;
        for (ULONG i = 0; i < WORDS; ++i) backing.Data[i] = 0x51a2b3c4;
        RtlInitializeBitMap(&map, backing.Data, size);
        RtlClearAllBits(&map);
        for (ULONG i = 0; i < size; ++i) expected[i] = FALSE;
        verify(&map);
        RtlSetAllBits(&map);
        for (ULONG i = 0; i < size; ++i) expected[i] = TRUE;
        verify(&map);
        for (ULONG iteration = 0; iteration < 1000; ++iteration) {
            ULONG start = random_value() % size;
            ULONG length = 1 + random_value() % (size - start);
            BOOLEAN value = (BOOLEAN)(random_value() & 1u);
            if (value) RtlSetBits(&map, start, length);
            else RtlClearBits(&map, start, length);
            for (ULONG i = 0; i < length; ++i) expected[start + i] = value;
            verify(&map);
            CHECK(value ? RtlAreBitsSet(&map, start, length) : RtlAreBitsClear(&map, start, length));
            length = 1 + random_value() % (size + 1);
            ULONG hint = random_value() % (size + 17);
            ULONG found = RtlFindClearBits(&map, length, hint);
            if (found == MAXULONG) CHECK(!exists(&map, length, FALSE));
            else check_range(&map, found, length, FALSE);
            CHECK(RtlFindClearBitsAndSet(&map, length, hint) == found);
            if (found != MAXULONG)
                for (ULONG i = 0; i < length; ++i) expected[found + i] = TRUE;
            verify(&map);
            found = RtlFindSetBits(&map, length, hint);
            if (found == MAXULONG) CHECK(!exists(&map, length, TRUE));
            else check_range(&map, found, length, TRUE);
            CHECK(RtlFindSetBitsAndClear(&map, length, hint) == found);
            if (found != MAXULONG)
                for (ULONG i = 0; i < length; ++i) expected[found + i] = FALSE;
            verify(&map);
        }
    }
}
ULONG64 EfiMain(PVOID image_handle, PVOID system_table)
{
    (void)image_handle;
    (void)system_table;
    XpTestReport("XP RTL ARM64 entry reached through UEFI PE loader\n");
    CHECK(sizeof(LONG) == 4 && sizeof(PVOID) == 8);
    volatile LONG word = 0;
    CHECK(InterlockedIncrement(&word) == 1);
    CHECK(InterlockedCompareExchange(&word, 9, 1) == 1 && word == 9);
    bitmap_checks();
    XpCheckKernelTraps();
    XpTestReport("PASS: XP RTL bitmap, 15 sizes, 15000 mutations, search/allocation/count/bounds on ARM64\n");
    XpTestFinish(0);
}
