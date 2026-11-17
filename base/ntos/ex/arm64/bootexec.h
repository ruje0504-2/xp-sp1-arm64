#ifndef XP_ARM64_BOOT_EXEC_H
#define XP_ARM64_BOOT_EXEC_H
#include <ntos.h>

extern FAST_MUTEX ExpEnvironmentLock;
extern ERESOURCE ExpKeyManipLock;
NTSTATUS ExpArm64InitializeBootResources(VOID);
VOID ExpArm64InitializePhase1PoolLookasides(VOID);
#endif
