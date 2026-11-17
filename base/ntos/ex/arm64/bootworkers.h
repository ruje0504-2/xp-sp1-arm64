#ifndef XP_EX_ARM64_BOOT_WORKERS_H
#define XP_EX_ARM64_BOOT_WORKERS_H
#include <ntos.h>

NTSTATUS ExpArm64InitializeWorkers(VOID);
extern PETHREAD ExpWorkerThreadBalanceManagerPtr;
extern EX_WORK_QUEUE ExWorkerQueue[MaximumWorkQueue];

#endif
