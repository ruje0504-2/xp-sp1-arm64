/* Initial freestanding RTL memory support for normal writable memory. */
#include <nt.h>
#include <ntrtl.h>
#include <stddef.h>

/* Byte stores also handle unaligned buffers. Freestanding build flags prevent
 * the compiler from folding this implementation into a recursive memset. */
void *memset(void *destination, int value, size_t bytes)
{
    unsigned char *cursor = destination;
    while (bytes != 0) {
        *cursor++ = (unsigned char)value;
        --bytes;
    }
    return destination;
}

VOID NTAPI RtlFillMemoryUlong(PVOID destination, SIZE_T bytes, ULONG pattern)
{
    ULONG *cursor = destination;
    SIZE_T words = bytes / sizeof(ULONG);
    while (words != 0) {
        *cursor++ = pattern;
        --words;
    }
}

/* memcpy has the standard non-overlap contract. */
void *memcpy(void *destination, const void *source, size_t bytes)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    while (bytes != 0) {
        *out++ = *in++;
        --bytes;
    }
    return destination;
}
