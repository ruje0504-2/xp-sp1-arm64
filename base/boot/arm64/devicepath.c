/* UEFI 2.10 Device Path Protocol. The boot volume's ARC component encodes
 * its byte-packed single-instance firmware path, including the end node. */
#include "boot.h"

ULONG BlArm64EncodeArcDevicePath(const UCHAR *path, ULONG available, PCHAR arc_name)
{
    static const CHAR hexadecimal[] = "0123456789abcdef";
    *arc_name = 0;
    if (!path || available > XP_ARM64_DEVICE_PATH_CAPACITY) return 0;

    ULONG bytes = 0;
    while (available - bytes >= 4) {
        const UCHAR *node = path + bytes;
        ULONG length = node[2] | ((ULONG)node[3] << 8);
        if (length < 4 || length > available - bytes) return 0;
        if (node[0] == 0x7f) {
            if (!bytes || node[1] != 0xff || length != 4) return 0;
            bytes += length;
            RtlCopyMemory(arc_name, "uefi(", sizeof("uefi(") - 1);
            PCHAR digit = arc_name + sizeof("uefi(") - 1;
            for (ULONG i = 0; i < bytes; ++i) {
                *digit++ = hexadecimal[path[i] >> 4];
                *digit++ = hexadecimal[path[i] & 15];
            }
            *digit++ = ')';
            *digit = 0;
            return bytes;
        }
        bytes += length;
    }
    return 0;
}
