#ifndef XP_ARM64_USER32_EDIT_H
#define XP_ARM64_USER32_EDIT_H

/* These descriptors use the native XP Win32 syscall layout. */
typedef struct {
    unsigned Length, Capacity;
    const unsigned short *Buffer;
} XP_USER32_LARGE_STRING;

extern const XP_USER32_LARGE_STRING XpUser32EditClass;

unsigned XpUser32InitializeEdit(unsigned long long module);
unsigned long long XpUser32EditWndProc(unsigned long long window, unsigned message,
                                      unsigned long long wparam, unsigned long long lparam);

#endif
