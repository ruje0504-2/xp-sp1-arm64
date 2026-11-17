#ifndef XP_ARM64_BOOT_OBJECT_H
#define XP_ARM64_BOOT_OBJECT_H
#include <ntos.h>

extern PHANDLE_TABLE ObpKernelHandleTable;
extern POBJECT_DIRECTORY ObpRootDirectoryObject, ObpTypeDirectoryObject;
extern WORK_QUEUE_ITEM ObpRemoveObjectWorkItem;
extern POBJECT_TYPE ObpTypeObjectType, ObpDirectoryObjectType, ObpSymbolicLinkObjectType;
extern GENERAL_LOOKASIDE ObpCreateInfoLookasideList, ObpNameBufferLookasideList;
extern KEVENT ObpDefaultObject;
extern EX_PUSH_LOCK ObpLock;
extern FAST_MUTEX ObpDeviceMapLock;
extern PVOID ObpRemoveObjectList;
extern PDEVICE_MAP ObSystemDeviceMap;

NTSTATUS ObArm64InitializeBootObjectState(VOID);
NTSTATUS ObArm64InitializeBootObjectTypes(VOID);
NTSTATUS ObArm64InitializeBootNamespace(VOID);
NTSTATUS ObArm64InitializePhase1(VOID);
NTSTATUS ObArm64CreateBootDirectory(PCWSTR Name, POBJECT_DIRECTORY *Directory);
NTSTATUS ObArm64AllocateBootNamedObject(PCWSTR Name, POBJECT_TYPE Type, ULONG Size, PVOID *Object);
NTSTATUS ObArm64AllocateBootUnicodeObject(PCUNICODE_STRING Name, POBJECT_TYPE Type, ULONG Size, PVOID *Object);
NTSTATUS ObArm64AllocateBootServiceObject(POBJECT_ATTRIBUTES Attributes, POBJECT_TYPE Type,
                                         ULONG Size, PVOID *Object);
NTSTATUS ObArm64InsertBootServiceObject(POBJECT_ATTRIBUTES Attributes, PVOID Object,
                                       ACCESS_MASK Access, PHANDLE Handle);
NTSTATUS ObArm64MakeBootObjectTemporary(HANDLE Handle);
NTSTATUS ObArm64CreateBootPermanentRootDirectory(PCWSTR Name, PSECURITY_DESCRIPTOR Security);
NTSTATUS ObArm64CaptureBootTokenSecurity(POBJECT_TYPE Type, PSECURITY_DESCRIPTOR Explicit,
                                        PSECURITY_DESCRIPTOR *Relative);
PDEVICE_MAP ObArm64ReferenceSystemDeviceMap(VOID);
VOID ObArm64DereferenceSystemDeviceMap(PDEVICE_MAP Map);
NTSTATUS ObArm64InsertBootDirectoryEntry(POBJECT_DIRECTORY Directory, POBJECT_HEADER Header);
PVOID ObArm64LookupBootDirectoryEntry(POBJECT_DIRECTORY Directory, PUNICODE_STRING Name, ULONG Attributes);
NTSTATUS ObArm64RemoveBootDirectoryEntry(POBJECT_DIRECTORY Directory, PUNICODE_STRING Name);
NTSTATUS ObArm64LookupBootObjectName(HANDLE Root, PCUNICODE_STRING Name, ULONG Attributes,
                                    POBJECT_TYPE Type, PVOID *Object);
NTSTATUS ObArm64OpenBootObjectByName(HANDLE Root, PCUNICODE_STRING Name, ULONG Attributes,
                                    POBJECT_TYPE Type, ACCESS_MASK Access, PHANDLE Handle);
/* At PASSIVE_LEVEL, consumes the creation reference for a named, unpublished
 * kernel object from ObpAllocateObject(NULL, ...) and its optional pool-owned,
 * validated self-relative descriptor. OBJ_PERMANENT takes effect only for
 * a new name after a handle is created. */
NTSTATUS ObArm64InsertBootObject(HANDLE Root, PVOID Object, ULONG Attributes,
                                 PSECURITY_DESCRIPTOR Descriptor, ACCESS_MASK Access, PHANDLE Handle);
NTSTATUS ObArm64CreateProcessHandle(PEPROCESS Process, PVOID Object, ACCESS_MASK Access, PHANDLE Handle);
BOOLEAN ObArm64SweepProcessHandle(PHANDLE_TABLE_ENTRY Entry, HANDLE Handle, PVOID Context);
NTSTATUS ObArm64CloseBootObjectHandle(HANDLE Handle);
NTSTATUS ObArm64CloseUserHandle(HANDLE Handle, POBJECT_TYPE Type);
NTSTATUS ObArm64CreateBootHandle(PVOID Object, ACCESS_MASK Access, BOOLEAN KernelHandle, PHANDLE Handle);
NTSTATUS ObpInitSecurityDescriptorCache(VOID);
VOID PsInitializeQuotaSystem(VOID);
#endif
