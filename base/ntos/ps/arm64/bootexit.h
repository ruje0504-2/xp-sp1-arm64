#ifndef XP_ARM64_BOOT_EXIT_H
#define XP_ARM64_BOOT_EXIT_H
#include <ntos.h>

/* Retires both caller-owned boot threads and executive-owned System threads. */
NTSTATUS PsArm64StartBootReaper(VOID);
BOOLEAN PsArm64BootReaperReady(VOID);
__declspec(noreturn) VOID PsArm64ExitBootThread(PETHREAD Thread, NTSTATUS ExitStatus);

#endif
