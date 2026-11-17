// Derived from .NET runtime ARM64 unwinder, MIT licensed.
// See third_party/dotnet-unwind/LICENSE.TXT and source.json for the immutable
// upstream, commit, hashes and license. The .NET Foundation notice is retained.
// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// XP adaptation: bounded image/stack reads, transactional context outputs,
// register-index validation and explicit rejection of private frame/end_c
// opcodes. No CLR debugger, DAC, process reader or Windows private object ABI.
#include "unwind.h"
#define CONTEXT_UNWOUND_TO_CALL 0x20000000u
#define STATUS_UNWIND_UNSUPPORTED_VERSION STATUS_NOT_SUPPORTED
#define STATUS_UNWIND_NOT_IN_FUNCTION STATUS_BAD_FUNCTION_TABLE
#define STATUS_UNWIND_INVALID_SEQUENCE STATUS_BAD_FUNCTION_TABLE
typedef struct {
    const RTL_ARM64_UNWIND_IMAGE *Image;
    ULONG_PTR StackLow, StackHigh;
    ULONG64 FrameSp;
    BOOLEAN BadRead;
    PKNONVOLATILE_CONTEXT_POINTERS ContextPointers;
} ARM64_UNWIND_PARAMS, *PARM64_UNWIND_PARAMS;
static BOOLEAN RtlpRange(ULONG_PTR low, ULONG_PTR high, ULONG_PTR at, ULONG64 bytes)
{
    return at >= low && at <= high && bytes <= high - at;
}
static BOOLEAN RtlpStackRange(PARM64_UNWIND_PARAMS params, ULONG_PTR at, ULONG64 bytes)
{
    return !(at & 7) && RtlpRange(params->StackLow, params->StackHigh, at, bytes);
}
static ULONG64 RtlpReadImage(PARM64_UNWIND_PARAMS params, ULONG_PTR at, ULONG size)
{
    if (!RtlpRange(params->Image->Base, params->Image->Base + params->Image->Size, at, size)) {
        params->BadRead = TRUE; return 0;
    }
    ULONG64 value = 0;
    for (ULONG i = 0; i < size; ++i) value |= (ULONG64)((const UCHAR *)at)[i] << (i * 8);
    return value;
}
static ULONG64 RtlpReadStack(PARM64_UNWIND_PARAMS params, ULONG_PTR at)
{
    if (!RtlpStackRange(params, at, 8)) { params->BadRead = TRUE; return 0; }
    return *(const ULONG64 *)at;
}
#define MEMORY_READ_BYTE(p,a) ((UCHAR)RtlpReadImage((p),(a),1))
#define MEMORY_READ_DWORD(p,a) ((ULONG)RtlpReadImage((p),(a),4))
#define MEMORY_READ_QWORD(p,a) RtlpReadStack((p),(a))
#define UPDATE_CONTEXT_POINTERS(p,r,a) do { if ((p)->ContextPointers && (r)>=19 && (r)<=30) \
    (&(p)->ContextPointers->X19)[(r)-19] = (PULONG64)(a); } while(0)
#define UPDATE_FP_CONTEXT_POINTERS(p,r,a) do { if ((p)->ContextPointers && (r)>=8 && (r)<=15) \
    (&(p)->ContextPointers->D8)[(r)-8] = (PULONG64)(a); } while(0)
#define OPCODE_IS_END(Op) (((Op) & 0xfe) == 0xe4)
static const UCHAR UnwindCodeSizeTable[256] =
{
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
    4,1,2,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1
};

static ULONG
RtlpComputeScopeSize(
    ULONG_PTR UnwindCodePtr,
    ULONG_PTR UnwindCodesEndPtr,
    BOOLEAN IsEpilog,
    PARM64_UNWIND_PARAMS UnwindParams
    )

/*++

Routine Description:

    Computes the size of an prolog or epilog, in words.

Arguments:

    UnwindCodePtr - Supplies a pointer to the start of the unwind
        code sequence.

    UnwindCodesEndPtr - Supplies a pointer to the byte immediately
        following the unwind code table, as described by the header.

    IsEpilog - Specifies TRUE if the scope describes an epilog,
        or FALSE if it describes a prolog.

    UnwindParams - Additional parameters shared with caller.

Return Value:

    The size of the scope described by the unwind codes, in halfword units.

--*/

{
    ULONG ScopeSize;
    UCHAR Opcode;

    //
    // Iterate through the unwind codes until we hit an end marker.
    // While iterating, accumulate the total scope size.
    //

    ScopeSize = 0;
    Opcode = 0;
    while (UnwindCodePtr < UnwindCodesEndPtr) {
        Opcode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
        if (OPCODE_IS_END(Opcode)) {
            break;
        }

        UnwindCodePtr += UnwindCodeSizeTable[Opcode];
        ScopeSize++;
    }

    //
    // Epilogs have one extra instruction at the end that needs to be
    // accounted for.
    //

    if (IsEpilog) {
        ScopeSize++;
    }

    return ScopeSize;
}

static NTSTATUS
RtlpUnwindRestoreRegisterRange(
    PCONTEXT ContextRecord,
    LONG SpOffset,
    ULONG FirstRegister,
    ULONG RegisterCount,
    PARM64_UNWIND_PARAMS UnwindParams
    )

/*++

Routine Description:

    Restores a series of integer registers from the stack.

Arguments:

    ContextRecord - Supplies the address of a context record.

    SpOffset - Specifies a stack offset. Positive values are simply used
        as a base offset. Negative values assume a predecrement behavior:
        a 0 offset is used for restoration, but the absolute value of the
        offset is added to the final Sp.

    FirstRegister - Specifies the index of the first register to restore.

    RegisterCount - Specifies the number of registers to restore.

    UnwindParams - Additional parameters shared with caller.

Return Value:

    None.

--*/

{
    ULONG_PTR CurAddress;
    ULONG RegIndex;
    NTSTATUS Status;

    //
    // Compute the source address and validate it.
    //

    if (FirstRegister > 30 || RegisterCount > 31 - FirstRegister)
        return STATUS_BAD_FUNCTION_TABLE;
    CurAddress = ContextRecord->Sp;
    if (SpOffset >= 0) {
        CurAddress += SpOffset;
    }

    Status = STATUS_SUCCESS;
    if (!RtlpStackRange(UnwindParams, CurAddress, 8ULL * RegisterCount))
        return STATUS_BAD_STACK;
    if (Status != STATUS_SUCCESS) {
        return Status;
    }

    //
    // Restore the registers
    //

    for (RegIndex = 0; RegIndex < RegisterCount; RegIndex++) {
        UPDATE_CONTEXT_POINTERS(UnwindParams, FirstRegister + RegIndex, CurAddress);
#ifdef __GNUC__
        *(&ContextRecord->X0 + FirstRegister + RegIndex) = MEMORY_READ_QWORD(UnwindParams, CurAddress);
#else
        ContextRecord->X[FirstRegister + RegIndex] = MEMORY_READ_QWORD(UnwindParams, CurAddress);
#endif
        CurAddress += 8;
    }
    if (SpOffset < 0) {
        ContextRecord->Sp -= SpOffset;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
RtlpUnwindRestoreFpRegisterRange(
    PCONTEXT ContextRecord,
    LONG SpOffset,
    ULONG FirstRegister,
    ULONG RegisterCount,
    PARM64_UNWIND_PARAMS UnwindParams
    )

/*++

Routine Description:

    Restores a series of floating-point registers from the stack.

Arguments:

    ContextRecord - Supplies the address of a context record.

    SpOffset - Specifies a stack offset. Positive values are simply used
        as a base offset. Negative values assume a predecrement behavior:
        a 0 offset is used for restoration, but the absolute value of the
        offset is added to the final Sp.

    FirstRegister - Specifies the index of the first register to restore.

    RegisterCount - Specifies the number of registers to restore.

    UnwindParams - Additional parameters shared with caller.

Return Value:

    None.

--*/

{
    ULONG_PTR CurAddress;
    ULONG RegIndex;
    NTSTATUS Status;

    //
    // Compute the source address and validate it.
    //

    if (FirstRegister < 8 || FirstRegister > 15 || RegisterCount > 16 - FirstRegister)
        return STATUS_BAD_FUNCTION_TABLE;
    CurAddress = ContextRecord->Sp;
    if (SpOffset >= 0) {
        CurAddress += SpOffset;
    }

    Status = STATUS_SUCCESS;
    if (!RtlpStackRange(UnwindParams, CurAddress, 8ULL * RegisterCount))
        return STATUS_BAD_STACK;
    if (Status != STATUS_SUCCESS) {
        return Status;
    }

    //
    // Restore the registers
    //

    for (RegIndex = 0; RegIndex < RegisterCount; RegIndex++) {
        UPDATE_FP_CONTEXT_POINTERS(UnwindParams, FirstRegister + RegIndex, CurAddress);
        ContextRecord->V[FirstRegister + RegIndex].Low = MEMORY_READ_QWORD(UnwindParams, CurAddress);
        CurAddress += 8;
    }
    if (SpOffset < 0) {
        ContextRecord->Sp -= SpOffset;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
RtlpUnwindFunctionFull(
    ULONG64 ControlPcRva,
    ULONG_PTR ImageBase,
    PRUNTIME_FUNCTION FunctionEntry,
    CONTEXT *ContextRecord,
    PULONG64 EstablisherFrame,
    PEXCEPTION_ROUTINE *HandlerRoutine,
    PVOID *HandlerData,
    PARM64_UNWIND_PARAMS UnwindParams
    )

/*++

Routine Description:

    This function virtually unwinds the specified function by parsing the
    .xdata record to determine where in the function the provided ControlPc
    is, and then executing unwind codes that map to the function's prolog
    or epilog behavior.

    If a context pointers record is specified (in the UnwindParams), then
    the address where each nonvolatile register is restored from is recorded
    in the appropriate element of the context pointers record.

Arguments:

    ControlPcRva - Supplies the address where control left the specified
        function, as an offset relative to the ImageBase.

    ImageBase - Supplies the base address of the image that contains the
        function being unwound.

    FunctionEntry - Supplies the address of the function table entry for the
        specified function. If appropriate, this should have already been
        probed.

    ContextRecord - Supplies the address of a context record.

    EstablisherFrame - Supplies a pointer to a variable that receives the
        the establisher frame pointer value.

    HandlerRoutine - Supplies an optional pointer to a variable that receives
        the handler routine address.  If control did not leave the specified
        function in either the prolog or an epilog and a handler of the
        proper type is associated with the function, then the address of the
        language specific exception handler is returned. Otherwise, NULL is
        returned.

    HandlerData - Supplies a pointer to a variable that receives a pointer
        the language handler data.

    UnwindParams - Additional parameters shared with caller.

Return Value:

    STATUS_SUCCESS if the unwind could be completed, a failure status otherwise.
    Unwind can only fail when validation bounds are specified.

--*/

{
    ULONG AccumulatedSaveNexts;
    ULONG CurCode;
    ULONG EpilogScopeCount;
    PEXCEPTION_ROUTINE ExceptionHandler;
    PVOID ExceptionHandlerData;
    BOOLEAN FinalPcFromLr;
    ULONG FunctionLength;
    ULONG HeaderWord;
    ULONG NextCode;
    ULONG64 OffsetInFunction;
    ULONG ScopeNum;
    ULONG ScopeSize;
    ULONG ScopeStart;
    ULONG64 SkipWords;
    NTSTATUS Status;
    ULONG_PTR UnwindCodePtr;
    ULONG_PTR UnwindCodesEndPtr;
    ULONG_PTR UnwindDataPtr;
    ULONG UnwindIndex;
    ULONG UnwindWords;

    //
    // Unless a special frame is encountered, assume that any unwinding
    // will return us to the return address of a call and set the flag
    // appropriately (it will be cleared again if the special cases apply).
    //

    ContextRecord->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;

    //
    // By default, unwinding is done by popping to the LR, then copying
    // that LR to the PC. However, some special opcodes require different
    // behavior.
    //

    FinalPcFromLr = TRUE;

    //
    // Fetch the header word from the .xdata blob
    //

    UnwindDataPtr = ImageBase + FunctionEntry->UnwindData;
    HeaderWord = MEMORY_READ_DWORD(UnwindParams, UnwindDataPtr);
    UnwindDataPtr += 4;

    //
    // Verify the version before we do anything else
    //

    if (((HeaderWord >> 18) & 3) != 0) {
        return STATUS_UNWIND_UNSUPPORTED_VERSION;
    }

    FunctionLength = HeaderWord & 0x3ffff;
    OffsetInFunction = (ControlPcRva - FunctionEntry->BeginAddress) / 4;

    //
    // Determine the number of epilog scope records and the maximum number
    // of unwind codes.
    //

    UnwindWords = (HeaderWord >> 27) & 31;
    EpilogScopeCount = (HeaderWord >> 22) & 31;
    if (EpilogScopeCount == 0 && UnwindWords == 0) {
        EpilogScopeCount = MEMORY_READ_DWORD(UnwindParams, UnwindDataPtr);
        UnwindDataPtr += 4;
        UnwindWords = (EpilogScopeCount >> 16) & 0xff;
        EpilogScopeCount &= 0xffff;
    }
    if ((HeaderWord & (1 << 21)) != 0) {
        UnwindIndex = EpilogScopeCount;
        EpilogScopeCount = 0;
    } else {
        UnwindIndex = 0;
    }

    //
    // If exception data is present, extract it now.
    //

    ExceptionHandler = NULL;
    ExceptionHandlerData = NULL;
    if ((HeaderWord & (1 << 20)) != 0) {
        ExceptionHandler = (PEXCEPTION_ROUTINE)(ImageBase +
                        MEMORY_READ_DWORD(UnwindParams, UnwindDataPtr + 4 * (EpilogScopeCount + UnwindWords)));
        ExceptionHandlerData = (PVOID)(UnwindDataPtr + 4 * (EpilogScopeCount + UnwindWords + 1));
    }

    //
    // Unless we are in a prolog/epilog, we execute the unwind codes
    // that immediately follow the epilog scope list.
    //

    UnwindCodePtr = UnwindDataPtr + 4 * EpilogScopeCount;
    UnwindCodesEndPtr = UnwindCodePtr + 4 * UnwindWords;
    SkipWords = 0;

    //
    // If we're near the start of the function, and this function has a prolog,
    // compute the size of the prolog from the unwind codes. If we're in the
    // midst of it, we still execute starting at unwind code index 0, but we may
    // need to skip some to account for partial execution of the prolog.
    //
    // N.B. As an optimization here, note that each byte of unwind codes can
    //      describe at most one 32-bit instruction. Thus, the largest prologue
    //      that could possibly be described by UnwindWords (which is 4 * the
    //      number of unwind code bytes) is 4 * UnwindWords words. If
    //      OffsetInFunction is larger than this value, it is guaranteed to be
    //      in the body of the function.
    //

    if (OffsetInFunction < 4 * UnwindWords) {
        ScopeSize = RtlpComputeScopeSize(UnwindCodePtr, UnwindCodesEndPtr, FALSE, UnwindParams);

        if (OffsetInFunction < ScopeSize) {
            SkipWords = ScopeSize - OffsetInFunction;
            ExceptionHandler = NULL;
            ExceptionHandlerData = NULL;
            goto ExecuteCodes;
        }
    }

    //
    // We're not in the prolog, now check to see if we are in the epilog.
    // In the simple case, the 'E' bit is set indicating there is a single
    // epilog that lives at the end of the function. If we're near the end
    // of the function, compute the actual size of the epilog from the
    // unwind codes. If we're in the midst of it, adjust the unwind code
    // pointer to the start of the codes and determine how many we need to skip.
    //
    // N.B. Similar to the prolog case above, the maximum number of halfwords
    //      that an epilog can cover is limited by UnwindWords. In the epilog
    //      case, however, the starting index within the unwind code table is
    //      non-zero, and so the maximum number of unwind codes that can pertain
    //      to an epilog is (UnwindWords * 4 - UnwindIndex), thus further
    //      constraining the bounds of the epilog.
    //

    if ((HeaderWord & (1 << 21)) != 0) {
        if (OffsetInFunction + (4 * UnwindWords - UnwindIndex) >= FunctionLength) {
            ScopeSize = RtlpComputeScopeSize(UnwindCodePtr + UnwindIndex, UnwindCodesEndPtr, TRUE, UnwindParams);
            ScopeStart = FunctionLength - ScopeSize;

            if (OffsetInFunction >= ScopeStart) {
                UnwindCodePtr += UnwindIndex;
                SkipWords = OffsetInFunction - ScopeStart;
                ExceptionHandler = NULL;
                ExceptionHandlerData = NULL;
            }
        }
    }

    //
    // In the multiple-epilog case, we scan forward to see if we are within
    // shooting distance of any of the epilogs. If we are, we compute the
    // actual size of the epilog from the unwind codes and proceed like the
    // simple case above.
    //

    else {
        for (ScopeNum = 0; ScopeNum < EpilogScopeCount; ScopeNum++) {
            HeaderWord = MEMORY_READ_DWORD(UnwindParams, UnwindDataPtr);
            UnwindDataPtr += 4;

            //
            // The scope records are stored in order. If we hit a record that
            // starts after our current position, we must not be in an epilog.
            //

            ScopeStart = HeaderWord & 0x3ffff;
            if (OffsetInFunction < ScopeStart) {
                break;
            }

            UnwindIndex = HeaderWord >> 22;
            if (OffsetInFunction < ScopeStart + (4 * UnwindWords - UnwindIndex)) {
                ScopeSize = RtlpComputeScopeSize(UnwindCodePtr + UnwindIndex, UnwindCodesEndPtr, TRUE, UnwindParams);

                if (OffsetInFunction < ScopeStart + ScopeSize) {

                    UnwindCodePtr += UnwindIndex;
                    SkipWords = OffsetInFunction - ScopeStart;
                    ExceptionHandler = NULL;
                    ExceptionHandlerData = NULL;
                    break;
                }
            }
        }
    }

ExecuteCodes:

    //
    // Skip over unwind codes until we account for the number of halfwords
    // to skip.
    //

    while (UnwindCodePtr < UnwindCodesEndPtr && SkipWords > 0) {
        CurCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
        if (OPCODE_IS_END(CurCode)) {
            break;
        }
        UnwindCodePtr += UnwindCodeSizeTable[CurCode];
        SkipWords--;
    }

    //
    // Now execute codes until we hit the end.
    //

    Status = STATUS_SUCCESS;
    AccumulatedSaveNexts = 0;
    while (UnwindCodePtr < UnwindCodesEndPtr && Status == STATUS_SUCCESS) {

        CurCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
        UnwindCodePtr += 1;

        //
        // alloc_s (000xxxxx): allocate small stack with size < 1024 (2^5 * 16)
        //

        if (CurCode <= 0x1f) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            ContextRecord->Sp += 16 * (CurCode & 0x1f);
        }

        //
        // save_r19r20_x (001zzzzz): save <r19,r20> pair at [sp-#Z*8]!, pre-indexed offset >= -248
        //

        else if (CurCode <= 0x3f) {
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        -8 * (CurCode & 0x1f),
                        19,
                        2 + 2 * AccumulatedSaveNexts,
                        UnwindParams);
            AccumulatedSaveNexts = 0;
        }

        //
        // save_fplr (01zzzzzz): save <r29,lr> pair at [sp+#Z*8], offset <= 504
        //

        else if (CurCode <= 0x7f) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        8 * (CurCode & 0x3f),
                        29,
                        2,
                        UnwindParams);
        }

        //
        // save_fplr_x (10zzzzzz): save <r29,lr> pair at [sp-(#Z+1)*8]!, pre-indexed offset >= -512
        //

        else if (CurCode <= 0xbf) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        -8 * ((CurCode & 0x3f) + 1),
                        29,
                        2,
                        UnwindParams);
        }

        //
        // alloc_m (11000xxx|xxxxxxxx): allocate large stack with size < 32k (2^11 * 16).
        //

        else if (CurCode <= 0xc7) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            ContextRecord->Sp += 16 * ((CurCode & 7) << 8);
            ContextRecord->Sp += 16 * MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
        }

        //
        // save_regp (110010xx|xxzzzzzz): save r(19+#X) pair at [sp+#Z*8], offset <= 504
        //

        else if (CurCode <= 0xcb) {
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        8 * (NextCode & 0x3f),
                        19 + ((CurCode & 3) << 2) + (NextCode >> 6),
                        2 + 2 * AccumulatedSaveNexts,
                        UnwindParams);
            AccumulatedSaveNexts = 0;
        }

        //
        // save_regp_x (110011xx|xxzzzzzz): save pair r(19+#X) at [sp-(#Z+1)*8]!, pre-indexed offset >= -512
        //

        else if (CurCode <= 0xcf) {
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        -8 * ((NextCode & 0x3f) + 1),
                        19 + ((CurCode & 3) << 2) + (NextCode >> 6),
                        2 + 2 * AccumulatedSaveNexts,
                        UnwindParams);
            AccumulatedSaveNexts = 0;
        }

        //
        // save_reg (110100xx|xxzzzzzz): save reg r(19+#X) at [sp+#Z*8], offset <= 504
        //

        else if (CurCode <= 0xd3) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        8 * (NextCode & 0x3f),
                        19 + ((CurCode & 3) << 2) + (NextCode >> 6),
                        1,
                        UnwindParams);
        }

        //
        // save_reg_x (1101010x|xxxzzzzz): save reg r(19+#X) at [sp-(#Z+1)*8]!, pre-indexed offset >= -256
        //

        else if (CurCode <= 0xd5) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        -8 * ((NextCode & 0x1f) + 1),
                        19 + ((CurCode & 1) << 3) + (NextCode >> 5),
                        1,
                        UnwindParams);
        }

        //
        // save_lrpair (1101011x|xxzzzzzz): save pair <r19+2*#X,lr> at [sp+#Z*8], offset <= 504
        //

        else if (CurCode <= 0xd7) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        8 * (NextCode & 0x3f),
                        19 + 2 * (((CurCode & 1) << 2) + (NextCode >> 6)),
                        1,
                        UnwindParams);
            if (Status == STATUS_SUCCESS) {
                Status = RtlpUnwindRestoreRegisterRange(
                        ContextRecord,
                        8 * (NextCode & 0x3f) + 8,
                        30,
                        1,
                        UnwindParams);
            }
        }

        //
        // save_fregp (1101100x|xxzzzzzz): save pair d(8+#X) at [sp+#Z*8], offset <= 504
        //

        else if (CurCode <= 0xd9) {
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreFpRegisterRange(
                        ContextRecord,
                        8 * (NextCode & 0x3f),
                        8 + ((CurCode & 1) << 2) + (NextCode >> 6),
                        2 + 2 * AccumulatedSaveNexts,
                        UnwindParams);
            AccumulatedSaveNexts = 0;
        }

        //
        // save_fregp_x (1101101x|xxzzzzzz): save pair d(8+#X), at [sp-(#Z+1)*8]!, pre-indexed offset >= -512
        //

        else if (CurCode <= 0xdb) {
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreFpRegisterRange(
                        ContextRecord,
                        -8 * ((NextCode & 0x3f) + 1),
                        8 + ((CurCode & 1) << 2) + (NextCode >> 6),
                        2 + 2 * AccumulatedSaveNexts,
                        UnwindParams);
            AccumulatedSaveNexts = 0;
        }

        //
        // save_freg (1101110x|xxzzzzzz): save reg d(9+#X) at [sp+#Z*8], offset <= 504
        //

        else if (CurCode <= 0xdd) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreFpRegisterRange(
                        ContextRecord,
                        8 * (NextCode & 0x3f),
                        8 + ((CurCode & 1) << 2) + (NextCode >> 6),
                        1,
                        UnwindParams);
        }

        //
        // save_freg_x (11011110|xxxzzzzz): save reg d(8+#X) at [sp-(#Z+1)*8]!, pre-indexed offset >= -256
        //

        else if (CurCode == 0xde) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            NextCode = MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
            Status = RtlpUnwindRestoreFpRegisterRange(
                        ContextRecord,
                        -8 * ((NextCode & 0x1f) + 1),
                        8 + (NextCode >> 5),
                        1,
                        UnwindParams);
        }

        //
        // alloc_l (11100000|xxxxxxxx|xxxxxxxx|xxxxxxxx): allocate large stack with size < 256M
        //

        else if (CurCode == 0xe0) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            ContextRecord->Sp += 16 * (MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr) << 16);
            UnwindCodePtr++;
            ContextRecord->Sp += 16 * (MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr) << 8);
            UnwindCodePtr++;
            ContextRecord->Sp += 16 * MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindCodePtr++;
        }

        //
        // set_fp (11100001): set up r29: with: mov r29,sp
        //

        else if (CurCode == 0xe1) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            ContextRecord->Sp = ContextRecord->Fp;
            UnwindParams->FrameSp = ContextRecord->Sp;
        }

        //
        // add_fp (11100010|xxxxxxxx): set up r29 with: add r29,sp,#x*8
        //

        else if (CurCode == 0xe2) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            ContextRecord->Sp = ContextRecord->Fp - 8 * MEMORY_READ_BYTE(UnwindParams, UnwindCodePtr);
            UnwindParams->FrameSp = ContextRecord->Sp;
            UnwindCodePtr++;
        }

        //
        // nop (11100011): no unwind operation is required
        //

        else if (CurCode == 0xe3) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
        }

        //
        // end (11100100): end of unwind code
        //

        else if (CurCode == 0xe4) {
            if (AccumulatedSaveNexts != 0) {
                return STATUS_UNWIND_INVALID_SEQUENCE;
            }
            goto finished;
        }

        //
        // end_c (11100101): end of unwind code in current chained scope
        //

        else if (CurCode == 0xe5) {
            return STATUS_NOT_SUPPORTED;
        }

        //
        // save_next (11100110): save next non-volatile Int or FP register pair.
        //

        else if (CurCode == 0xe6) {
            AccumulatedSaveNexts++;
        }

        //
        // custom_0 (111010xx): restore custom structure
        //

        else if (CurCode >= 0xe8 && CurCode <= 0xeb) {
            return STATUS_NOT_SUPPORTED; /* No Windows private frame layouts. */
        }

        //
        // Anything else is invalid
        //

        else {
            return STATUS_UNWIND_INVALID_SEQUENCE;
        }
    }

    return STATUS_BAD_FUNCTION_TABLE; /* Missing END or failed read/restore. */
finished:
    if (Status == STATUS_SUCCESS) {

        //
        // Since we always POP to the LR, recover the final PC from there, unless
        // it was overwritten due to a special case custom unwinding operation.
        // Also set the establisher frame equal to the final stack pointer.
        //

        if (FinalPcFromLr) {
            ContextRecord->Pc = ContextRecord->Lr;
        }
        *EstablisherFrame = ContextRecord->Sp;

        if (ARGUMENT_PRESENT(HandlerRoutine)) {
            *HandlerRoutine = ExceptionHandler;
        }
        *HandlerData = ExceptionHandlerData;
    }

    return Status;
}

static NTSTATUS
RtlpUnwindFunctionCompact(
    ULONG64 ControlPcRva,
    PRUNTIME_FUNCTION FunctionEntry,
    CONTEXT *ContextRecord,
    PULONG64 EstablisherFrame,
    PEXCEPTION_ROUTINE *HandlerRoutine,
    PVOID *HandlerData,
    PARM64_UNWIND_PARAMS UnwindParams
    )

/*++

Routine Description:

    This function virtually unwinds the specified function by parsing the
    compact .pdata record to determine where in the function the provided
    ControlPc is, and then executing a standard, well-defined set of
    operations.

    If a context pointers record is specified (in the UnwindParams), then
    the address where each nonvolatile register is restored from is recorded
    in the appropriate element of the context pointers record.

Arguments:

    ControlPcRva - Supplies the address where control left the specified
        function, as an offset relative to the ImageBase.

    FunctionEntry - Supplies the address of the function table entry for the
        specified function. If appropriate, this should have already been
        probed.

    ContextRecord - Supplies the address of a context record.

    EstablisherFrame - Supplies a pointer to a variable that receives the
        the establisher frame pointer value.

    HandlerRoutine - Supplies an optional pointer to a variable that receives
        the handler routine address.  If control did not leave the specified
        function in either the prolog or an epilog and a handler of the
        proper type is associated with the function, then the address of the
        language specific exception handler is returned. Otherwise, NULL is
        returned.

    HandlerData - Supplies a pointer to a variable that receives a pointer
        the language handler data.

    UnwindParams - Additional parameters shared with caller.

Return Value:

    STATUS_SUCCESS if the unwind could be completed, a failure status otherwise.
    Unwind can only fail when validation bounds are specified.

--*/

{
    ULONG Count;
    ULONG Cr;
    ULONG CurrentOffset;
    ULONG EpilogLength;
    ULONG Flag;
    ULONG FloatSize;
    ULONG FrameSize;
    ULONG FRegOpcodes;
    ULONG FunctionLength;
    ULONG HBit;
    ULONG HOpcodes;
    ULONG IRegOpcodes;
    ULONG IntSize;
    ULONG LocalSize;
    ULONG64 OffsetInFunction;
    ULONG64 OffsetInScope;
    ULONG PrologLength;
    ULONG RegF;
    ULONG RegI;
    ULONG RegSize;
    ULONG ScopeStart;
    ULONG StackAdjustOpcodes;
    NTSTATUS Status;
    ULONG UnwindData;

    UnwindData = FunctionEntry->UnwindData;
    Status = STATUS_SUCCESS;

    //
    // Compact records always describe an unwind to a call.
    //

    ContextRecord->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;

    //
    // Extract the basic information about how to do a full unwind.
    //

    Flag = UnwindData & 3;
    FunctionLength = (UnwindData >> 2) & 0x7ff;
    RegF = (UnwindData >> 13) & 7;
    RegI = (UnwindData >> 16) & 0xf;
    HBit = (UnwindData >> 20) & 1;
    Cr = (UnwindData >> 21) & 3;
    FrameSize = (UnwindData >> 23) & 0x1ff;

    if (RegI > 10 || !FunctionLength) return STATUS_BAD_FUNCTION_TABLE;
    if (Flag == 3) {
        return STATUS_UNWIND_INVALID_SEQUENCE;
    }
    if (Cr == 2) {
        return STATUS_UNWIND_INVALID_SEQUENCE;
    }

    //
    // Determine the size of the locals
    //

    IntSize = RegI * 8;
    if (Cr == 1) {
        IntSize += 8;
    }
    FloatSize = (RegF == 0) ? 0 : (RegF + 1) * 8;
    RegSize = (IntSize + FloatSize + 8*8 * HBit + 0xf) & ~0xf;
    if (RegSize > 16 * FrameSize) {
        return STATUS_UNWIND_INVALID_SEQUENCE;
    }
    LocalSize = 16 * FrameSize - RegSize;
    if (Cr == 3 && LocalSize < 16) return STATUS_BAD_FUNCTION_TABLE;

    //
    // If we're near the start of the function (within 17 words),
    // see if we are within the prolog.
    //
    // N.B. If the low 2 bits of the UnwindData are 2, then we have
    // no prolog.
    //

    OffsetInFunction = (ControlPcRva - FunctionEntry->BeginAddress) / 4;
    OffsetInScope = 0;
    if (OffsetInFunction < 17 && Flag != 2) {

        //
        // Compute sizes for each opcode in the prolog.
        //

        IRegOpcodes = (IntSize + 8) / 16;
        FRegOpcodes = (FloatSize + 8) / 16;
        HOpcodes = 4 * HBit;
        StackAdjustOpcodes = (Cr == 3) ? 1 : 0;
        if (Cr != 3 || LocalSize > 512) {
            StackAdjustOpcodes += (LocalSize > 4088) ? 2 : (LocalSize > 0) ? 1 : 0;
        }

        //
        // Compute the total prolog length and determine if we are within
        // its scope.
        //
        // N.B. We must execute prolog operations backwards to unwind, so
        // our final scope offset in this case is the distance from the end.
        //

        PrologLength = IRegOpcodes + FRegOpcodes + HOpcodes + StackAdjustOpcodes;

        if (OffsetInFunction < PrologLength) {
            OffsetInScope = PrologLength - OffsetInFunction;
        }
    }

    //
    // If we're near the end of the function (within 15 words), see if
    // we are within the epilog.
    //
    // N.B. If the low 2 bits of the UnwindData are 2, then we have
    // no epilog.
    //

    if (OffsetInScope == 0 && OffsetInFunction + 15 >= FunctionLength && Flag != 2) {

        //
        // Compute sizes for each opcode in the epilog.
        //

        IRegOpcodes = (IntSize + 8) / 16;
        FRegOpcodes = (FloatSize + 8) / 16;
        HOpcodes = HBit;
        StackAdjustOpcodes = (Cr == 3) ? 1 : 0;
        if (Cr != 3 || LocalSize > 512) {
            StackAdjustOpcodes += (LocalSize > 4088) ? 2 : (LocalSize > 0) ? 1 : 0;
        }

        //
        // Compute the total epilog length and determine if we are within
        // its scope.
        //

        EpilogLength = IRegOpcodes + FRegOpcodes + HOpcodes + StackAdjustOpcodes + 1;

        ScopeStart = FunctionLength - EpilogLength;
        if (OffsetInFunction > ScopeStart) {
            OffsetInScope = OffsetInFunction - ScopeStart;
        }
    }

    //
    // Process operations backwards, in the order: stack/frame deallocation,
    // VFP register popping, integer register popping, parameter home
    // area recovery.
    //
    // First case is simple: we process everything with no regard for
    // the current offset within the scope.
    //

    Status = STATUS_SUCCESS;
    if (OffsetInScope == 0) {

        if (Cr == 3) {
            Status = RtlpUnwindRestoreRegisterRange(ContextRecord, 0, 29, 2, UnwindParams);
        }
        ContextRecord->Sp += LocalSize;

        if (RegF != 0 && Status == STATUS_SUCCESS) {
            Status = RtlpUnwindRestoreFpRegisterRange(ContextRecord, IntSize, 8, RegF + 1, UnwindParams);
        }

        if (Cr == 1 && Status == STATUS_SUCCESS) {
            Status = RtlpUnwindRestoreRegisterRange(ContextRecord, IntSize - 8, 30, 1, UnwindParams);
        }
        if (RegI > 0 && Status == STATUS_SUCCESS) {
            Status = RtlpUnwindRestoreRegisterRange(ContextRecord, 0, 19, RegI, UnwindParams);
        }
        ContextRecord->Sp += RegSize;
    }

    //
    // Second case is more complex: we must step along each operation
    // to ensure it should be executed.
    //

    else {

        CurrentOffset = 0;
        if (Cr == 3) {
            if (LocalSize <= 512) {
                if (CurrentOffset++ >= OffsetInScope) {
                    Status = RtlpUnwindRestoreRegisterRange(ContextRecord, -(LONG)LocalSize, 29, 2, UnwindParams);
                }
                LocalSize = 0;
            }
        }
        while (LocalSize != 0) {
            Count = (LocalSize + 4087) % 4088 + 1;
            if (CurrentOffset++ >= OffsetInScope) {
                ContextRecord->Sp += Count;
            }
            LocalSize -= Count;
        }

        if (HBit != 0) {
            CurrentOffset += 4;
        }

        if (RegF != 0 && Status == STATUS_SUCCESS) {
            RegF++;
            while (RegF != 0) {
                Count = 2 - (RegF & 1);
                RegF -= Count;
                if (CurrentOffset++ >= OffsetInScope) {
                    Status = RtlpUnwindRestoreFpRegisterRange(
                               ContextRecord,
                               (RegF == 0 && RegI == 0) ? (-(LONG)RegSize) : (IntSize + 8 * RegF),
                               8 + RegF,
                               Count,
                               UnwindParams);
                }
            }
        }

        if (Cr == 1 && Status == STATUS_SUCCESS) {
            if (RegI % 2 == 0) {
                if (CurrentOffset++ >= OffsetInScope) {
                    Status = RtlpUnwindRestoreRegisterRange(ContextRecord, IntSize - 8, 30, 1, UnwindParams);
                }
            } else {
                if (CurrentOffset++ >= OffsetInScope) {
                    RegI--;
                    Status = RtlpUnwindRestoreRegisterRange(ContextRecord, IntSize - 8, 30, 1, UnwindParams);
                    if (Status == STATUS_SUCCESS) {
                        Status = RtlpUnwindRestoreRegisterRange(ContextRecord, IntSize - 16, 19 + RegI, 1, UnwindParams);
                    }
                }
            }
        }

        while (RegI != 0 && Status == STATUS_SUCCESS) {
            Count = 2 - (RegI & 1);
            RegI -= Count;
            if (CurrentOffset++ >= OffsetInScope) {
                Status = RtlpUnwindRestoreRegisterRange(
                            ContextRecord,
                            (RegI == 0) ? (-(LONG)RegSize) : (8 * RegI),
                            19 + RegI,
                            Count,
                            UnwindParams);
            }
        }
    }

    //
    // If we succeeded, post-process the results a bit
    //

    if (Status == STATUS_SUCCESS) {

        ContextRecord->Pc = ContextRecord->Lr;
        *EstablisherFrame = ContextRecord->Sp;

        if (ARGUMENT_PRESENT(HandlerRoutine)) {
            *HandlerRoutine = NULL;
        }
        *HandlerData = NULL;
    }

    return Status;
}


static NTSTATUS RtlpFunctionLength(PARM64_UNWIND_PARAMS p, const RUNTIME_FUNCTION *entry,
                                   ULONG *length)
{
    if ((entry->BeginAddress & 3) || entry->BeginAddress >= p->Image->Size)
        return STATUS_BAD_FUNCTION_TABLE;
    ULONG flag = entry->UnwindData & 3;
    if (flag == 3) return STATUS_NOT_SUPPORTED;
    if (flag) {
        *length = ((entry->UnwindData >> 2) & 0x7ff) * 4;
    } else {
        ULONG header = MEMORY_READ_DWORD(p, p->Image->Base + entry->UnwindData);
        if (p->BadRead || ((header >> 18) & 3)) return STATUS_BAD_FUNCTION_TABLE;
        *length = (header & 0x3ffff) * 4;
    }
    if (!*length || *length > p->Image->Size - entry->BeginAddress)
        return STATUS_BAD_FUNCTION_TABLE;
    return STATUS_SUCCESS;
}
static NTSTATUS RtlpValidateFull(PARM64_UNWIND_PARAMS p, const RUNTIME_FUNCTION *entry)
{
    ULONG_PTR at = p->Image->Base + entry->UnwindData;
    ULONG header = MEMORY_READ_DWORD(p, at);
    at += 4;
    ULONG words = header >> 27, epilogs = (header >> 22) & 31;
    if (!words && !epilogs) {
        ULONG extension = MEMORY_READ_DWORD(p, at);
        at += 4;
        if (extension >> 24) return STATUS_BAD_FUNCTION_TABLE;
        words = (extension >> 16) & 255;
        epilogs = extension & 65535;
    }
    BOOLEAN packed_epilog = (header & (1u << 21)) != 0;
    ULONG index = packed_epilog ? epilogs : 0;
    ULONG count = packed_epilog ? 0 : epilogs;
    if (!words || count > 4096) return STATUS_NOT_SUPPORTED;
    ULONG_PTR codes = at + (ULONG64)count * 4;
    ULONG bytes = words * 4;
    if (!RtlpRange(p->Image->Base, p->Image->Base + p->Image->Size, at,
        (ULONG64)count * 4 + bytes + ((header & (1u << 20)) ? 4 : 0)))
        return STATUS_BAD_FUNCTION_TABLE;
    UCHAR starts[1024];
    RtlZeroMemory(starts, sizeof(starts));
    for (ULONG off = 0; off < bytes;) {
        UCHAR op = MEMORY_READ_BYTE(p, codes + off);
        if (op == 0xdf || op == 0xe5 || op >= 0xe7) return STATUS_NOT_SUPPORTED;
        ULONG size = UnwindCodeSizeTable[op];
        if (size > bytes - off) return STATUS_BAD_FUNCTION_TABLE;
        starts[off] = 1;
        off += size;
    }
    ULONG length = header & 0x3ffff;
    if (RtlpComputeScopeSize(codes, codes + bytes, FALSE, p) > length)
        return STATUS_BAD_FUNCTION_TABLE;
    if (packed_epilog) {
        if (index >= bytes || !starts[index] ||
            RtlpComputeScopeSize(codes + index, codes + bytes, TRUE, p) > length)
            return STATUS_BAD_FUNCTION_TABLE;
    } else {
        ULONG previous = 0;
        for (ULONG i = 0; i < count; ++i) {
            ULONG scope = MEMORY_READ_DWORD(p, at + (ULONG64)i * 4);
            ULONG begin = scope & 0x3ffff, start = scope >> 22;
            if ((scope & 0x003c0000) || begin < previous || begin >= length ||
                start >= bytes || !starts[start]) return STATUS_BAD_FUNCTION_TABLE;
            ULONG size = RtlpComputeScopeSize(codes + start, codes + bytes, TRUE, p);
            if (size > length - begin) return STATUS_BAD_FUNCTION_TABLE;
            previous = begin;
        }
    }
    return p->BadRead ? STATUS_BAD_FUNCTION_TABLE : STATUS_SUCCESS;
}
NTSTATUS RtlArm64InitializeUnwindImage(PVOID base, ULONG size, RTL_ARM64_UNWIND_IMAGE *out)
{
    if (!base || !out || (ULONG_PTR)base > ~(ULONG_PTR)0 - size ||
        size < sizeof(IMAGE_DOS_HEADER)) return STATUS_INVALID_PARAMETER;
    const IMAGE_DOS_HEADER *dos = base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (ULONG)dos->e_lfanew > size ||
        sizeof(IMAGE_NT_HEADERS64) > size - (ULONG)dos->e_lfanew)
        return STATUS_INVALID_IMAGE_FORMAT;
    const IMAGE_NT_HEADERS64 *nt = (PVOID)((UCHAR *)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage != size ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        return STATUS_INVALID_IMAGE_FORMAT;
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir->Size || dir->Size % sizeof(RUNTIME_FUNCTION) || (dir->VirtualAddress & 3) ||
        dir->VirtualAddress > size || dir->Size > size - dir->VirtualAddress)
        return STATUS_INVALID_IMAGE_FORMAT;
    RTL_ARM64_UNWIND_IMAGE image = {(ULONG_PTR)base, size,
        (PVOID)((UCHAR *)base + dir->VirtualAddress), dir->Size / sizeof(RUNTIME_FUNCTION)};
    for (ULONG i = 0; i < image.Count; ++i)
        if ((image.Functions[i].BeginAddress & 3) || image.Functions[i].BeginAddress >= size ||
            (i && image.Functions[i-1].BeginAddress >= image.Functions[i].BeginAddress))
            return STATUS_INVALID_IMAGE_FORMAT;
    *out = image;
    return STATUS_SUCCESS;
}
NTSTATUS RtlArm64UnwindOne(const RTL_ARM64_UNWIND_IMAGE *image, PCONTEXT context,
    ULONG_PTR low, ULONG_PTR high, RTL_ARM64_UNWIND_RESULT *result)
{
    if (!image || !context || !result || low >= high || (context->Sp & 15) ||
        context->Sp < low || context->Sp > high || image->Base > ~(ULONG_PTR)0 - image->Size ||
        !image->Count || !RtlpRange(image->Base, image->Base + image->Size,
            (ULONG_PTR)image->Functions, (ULONG64)image->Count * sizeof(RUNTIME_FUNCTION)))
        return STATUS_INVALID_PARAMETER;
    ULONG64 pc = context->Pc;
    if (context->ContextFlags & CONTEXT_UNWOUND_TO_CALL) {
        if (pc < 4) return STATUS_BAD_FUNCTION_TABLE;
        pc -= 4;
    }
    if ((pc & 3) || pc < image->Base || pc - image->Base >= image->Size)
        return STATUS_NOT_FOUND;
    ULONG rva = (ULONG)(pc - image->Base);
    ULONG first = 0, last = image->Count;
    while (first < last) {
        ULONG middle = first + (last - first) / 2;
        if (image->Functions[middle].BeginAddress <= rva) first = middle + 1;
        else last = middle;
    }
    if (!first) return STATUS_NOT_FOUND;
    RUNTIME_FUNCTION entry = image->Functions[first - 1];
    CONTEXT temporary = *context;
    RTL_ARM64_UNWIND_RESULT output;
    RtlZeroMemory(&output, sizeof(output));
    ARM64_UNWIND_PARAMS params = {image, low, high, context->Sp, FALSE, &output.Pointers};
    ULONG length;
    NTSTATUS status = RtlpFunctionLength(&params, &entry, &length);
    if (!NT_SUCCESS(status)) return status;
    if (rva - entry.BeginAddress >= length) return STATUS_NOT_FOUND;
    if (entry.UnwindData & 3) {
        status = RtlpUnwindFunctionCompact(rva, &entry, &temporary,
            &output.EstablisherFrame, &output.Handler, &output.HandlerData, &params);
    } else {
        status = RtlpValidateFull(&params, &entry);
        if (!NT_SUCCESS(status)) return status;
        status = RtlpUnwindFunctionFull(rva, image->Base, &entry, &temporary,
            &output.EstablisherFrame, &output.Handler, &output.HandlerData, &params);
    }
    if (!NT_SUCCESS(status)) return status;
    if (params.BadRead || temporary.Sp < context->Sp || temporary.Sp > high ||
        (temporary.Sp & 15) || (temporary.Pc & 3))
        return STATUS_BAD_STACK;
    if ((output.Handler && !RtlpRange(image->Base, image->Base + image->Size,
                                     (ULONG_PTR)output.Handler, 4)) ||
        (output.HandlerData && !RtlpRange(image->Base, image->Base + image->Size,
                                         (ULONG_PTR)output.HandlerData, 1)))
        return STATUS_BAD_FUNCTION_TABLE;
    output.ControlPc = pc;
    output.FunctionBegin = image->Base + entry.BeginAddress;
    output.FunctionEnd = output.FunctionBegin + length;
    output.FrameSp = params.FrameSp;
    *context = temporary;
    *result = output;
    return STATUS_SUCCESS;
}
