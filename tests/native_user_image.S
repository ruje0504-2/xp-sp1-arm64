#include "xp-user-services.h"
#include "xp-win32-services.h"
#include "kernel_offsets.inc"
.text
/* One native user thread. Read identity from its own TEB, call a service
 * using its own stack, then return the application-selected exit status. */
.p2align 2
.globl XpChildUserProgram
.globl XpChildUserEnd
XpChildUserProgram:
    b start_program
    b dispatch_exception       /* Loader-known thunks at entry + 4, + 8, + 12. */
    b dispatch_apc
    b normal_apc
    b dispatch_callback
    b user_console_program
    b dispatch_user_window
    b user_post_program
start_program:
    mov x19, x0
    adrp x14, initialized_pointer
    ldr x14, [x14, :lo12:initialized_pointer]
    ldr x15, [x14]
    cbnz x15, 1f
    mov x15, #42
    str x15, [x14]
    ldr x15, [x14]
    str x15, [x19, #80]
    ldr x10, [x18, #0x60]
    ldr x11, [x10, #0x10]
    stp x10, x11, [x19, #48]
    ldrh w12, [x11]
    adr x13, XpChildUserProgram
    stp x12, x13, [x19, #64]
    mov x9, #0x1234
    fmov d7, x9
faulting_store:
    str wzr, [x11]             /* The PE header page is read-only. */
    str x0, [x19, #152]
    fmov x9, d7
    str x9, [x19, #160]
    sub sp, sp, #16
    ldr x9, [x18, #0x30]
    str x9, [x19]
    ldp x9, x10, [x18, #0x40]
    stp x9, x10, [x19, #8]
    mov x0, sp
    add x1, sp, #8
    mov w8, #XP_SERVICE_QUERY_PERFORMANCE_COUNTER
    svc #0
    cbnz w0, 1f
    ldp x9, x10, [sp]
    stp x9, x10, [x19, #24]
    add sp, sp, #16
    mov x0, xzr
    mov w1, #42
    mov w8, #XP_SERVICE_TERMINATE_THREAD
    svc #0
    str w0, [x19, #40]
    sub sp, sp, #16
    movn x10, #0x423f
    movk x10, #0xfff0, lsl #16   /* -1000000 ticks (100 ms) */
    str x10, [sp]
    mov w10, #1
    str x10, [x19, #176]
    mov x0, xzr                 /* non-alertable, first APC stays queued */
    mov x1, sp
    mov w8, #XP_SERVICE_DELAY_EXECUTION
    svc #0
    cbnz w0, dispatch_failed
    ldr x10, [x19, #184]
    cbnz x10, dispatch_failed
    mov w8, #XP_SERVICE_TEST_ALERT
    svc #0
    cbnz w0, dispatch_failed
    ldr x10, [x19, #184]
    cmp x10, #1
    b.ne dispatch_failed
    mov w10, #2
    str x10, [x19, #176]
    movn x10, #0x967f
    movk x10, #0xff67, lsl #16   /* -10000000 ticks (1 s) */
    str x10, [sp]
    mov w0, #1                  /* alertable and already waiting when queued */
    mov x1, sp
    mov w8, #XP_SERVICE_DELAY_EXECUTION
    svc #0
    str w0, [x19, #192]
    ldr x10, [x19, #184]
    cmp x10, #2
    b.ne dispatch_failed
    mov w10, #3
    str x10, [x19, #176]
    movn x10, #0x423f
    movk x10, #0xfff0, lsl #16
    str x10, [sp]
    mov x0, xzr                 /* third APC reaches thread exit still queued */
    mov x1, sp
    mov w8, #XP_SERVICE_DELAY_EXECUTION
    svc #0
    cbnz w0, dispatch_failed
    add sp, sp, #16
    mov x0, xzr
    mov x1, xzr
    mov x2, xzr
    mov w8, #XP_SERVICE_CALLBACK_RETURN
    svc #0
    str w0, [x19, #232]
    mov w0, #1
    mov w1, #0x1235
    mov w8, #0x1000
    svc #0
    str w0, [x19, #256]
    mov x0, xzr
    mov x1, xzr
    mov x2, xzr
    mov w8, #XP_SERVICE_CALLBACK_RETURN
    svc #0
    str w0, [x19, #280]
    mov w0, #3
    mov w1, #0x1237
    mov w8, #0x1000
    svc #0
    b dispatch_failed
1:  brk #0x42
dispatch_exception:
    ldr w10, [x0]
    mov w11, #0x80000003
    cmp w10, w11
    b.eq handle_breakpoint
    mov w11, #5
    movk w11, #0xc000, lsl #16
    cmp w10, w11
    b.ne dispatch_failed
    ldr x10, [x0, #16]         /* EXCEPTION_RECORD.ExceptionAddress */
    adr x11, faulting_store
    cmp x10, x11
    b.ne dispatch_failed
    str x10, [x19, #88]
    ldr w11, [x0, #24]
    cmp w11, #2
    b.ne dispatch_failed
    ldp x10, x11, [x0, #32]    /* access type and fault address */
    stp x11, x10, [x19, #96]
    mov x21, x1
    mov x22, sp
    str x22, [x19, #168]
nested_breakpoint:
    brk #0x27
    cmp sp, x22
    b.ne dispatch_failed
    add x0, x21, #1
    mov x1, xzr
    mov w8, #XP_SERVICE_CONTINUE
    svc #0
    str w0, [x19, #128]
    mov x0, #0x1000
    mov w8, #XP_SERVICE_CONTINUE
    svc #0
    str w0, [x19, #136]
    ldr x23, [x21, #CTX_PC]
    add x9, x23, #1
    str x9, [x21, #CTX_PC]
    mov x0, x21
    mov w8, #XP_SERVICE_CONTINUE
    svc #0
    str w0, [x19, #144]
    add x9, x23, #4
    str x9, [x21, #CTX_PC]
    mov x9, #0x5678
    str x9, [x21, #CTX_X]      /* successful NtContinue must preserve restored x0 */
    fmov d7, xzr
    mov x0, x21
    mov w1, #1
    mov w8, #XP_SERVICE_CONTINUE
    svc #0
    b dispatch_failed
handle_breakpoint:
    ldr x10, [x0, #16]
    adr x11, nested_breakpoint
    cmp x10, x11
    b.ne dispatch_failed
    mov x12, sp
    stp x10, x12, [x19, #112]
    add x10, x10, #4
    str x10, [x1, #CTX_PC]
    mov x0, x1
    mov x1, xzr
    mov w8, #XP_SERVICE_CONTINUE
    svc #0
dispatch_apc:
    mov x16, x0
    mov x20, x4
    mov x0, x1
    mov x1, x2
    mov x2, x3
    blr x16
    mov x0, x20
    mov w1, #1
    mov w8, #XP_SERVICE_CONTINUE
    svc #0
    b dispatch_failed
normal_apc:
    ldr x8, [x0, #184]
    add x8, x8, #1
    str x8, [x0, #184]
    add x9, x0, #200
    add x9, x9, x8, lsl #4
    stp x1, x2, [x9, #-16]
    ret
dispatch_callback:
    cmp w2, #16
    b.ne dispatch_failed
    mov x9, sp
    cmp x1, x9
    b.ne dispatch_failed
    ldp x9, x10, [x1]
    add x11, x0, #0x234
    add x11, x11, #1, lsl #12
    cmp x9, x11
    b.ne dispatch_failed
    add x11, x0, #0x500
    cmp x10, x11
    b.ne dispatch_failed
    cmp w0, #4
    b.eq exit_in_callback
    cmp w0, #3
    b.eq exit_with_parent_callback
    cmp w0, #2
    b.eq inner_callback
    cmp w0, #1
    b.ne dispatch_failed
    mov x9, sp
    str x9, [x19, #272]
    mov w0, #2
    mov w1, #0x1236
    mov w8, #0x1000
    svc #0
    str w0, [x19, #248]
    mov x9, #0x601
    mov w2, wzr
    b finish_callback
inner_callback:
    mov x9, sp
    str x9, [x19, #264]
    mov x0, #0x1000
    mov w1, #8
    mov w2, wzr
    mov w8, #XP_SERVICE_CALLBACK_RETURN
    svc #0
    str w0, [x19, #240]
    mov x9, #0x602
    mov w2, #0x23
    movk w2, #0xc000, lsl #16
finish_callback:
    str x9, [sp]
    mov x0, sp
    mov w1, #8
    mov w8, #XP_SERVICE_CALLBACK_RETURN
    svc #0
    b dispatch_failed
exit_with_parent_callback:
    mov w0, #4
    mov w1, #0x1238
    mov w8, #0x1000
    svc #0
    b dispatch_failed
exit_in_callback:
    mov x0, #-2
    mov w1, #42
    mov w8, #XP_SERVICE_TERMINATE_THREAD
    svc #0
dispatch_failed:
    mov x0, #-2
    mov w1, #70
    mov w8, #XP_SERVICE_TERMINATE_THREAD
    svc #0
XpChildUserEnd:

/* Independent native probe linked into the same PE. All calls go through
 * actual XP Win32 ordinals and use the calling process's handle table. */
.macro call_win32 service
    mov w8, #\service
    svc #0
.endm

user_console_program:
    mov x19, x0
    sub sp, sp, #64
    stp xzr, xzr, [sp]
    stp xzr, xzr, [sp, #16]
    stp xzr, xzr, [sp, #32]
    mov w9, #48
    str w9, [sp]
    add x9, sp, #48
    str x9, [sp, #16]
    mov w9, #0x40
    str w9, [sp, #24]
    call_win32 XP_UserGetProcessWindowStation
    cbnz x0, dispatch_failed

    mov x0, #0x1000
    mov w1, #1
    call_win32 XP_UserOpenWindowStation
    cbnz x0, dispatch_failed
    adrp x9, console_path
    add x9, x9, :lo12:console_path
    str x9, [sp, #56]
    mov w9, #console_path_end-console_path
    strh w9, [sp, #48]
    strh w9, [sp, #50]
    mov x0, sp
    mov w1, #0x01000000        /* ACCESS_SYSTEM_SECURITY belongs to the privilege path. */
    call_win32 XP_UserOpenWindowStation
    cbnz x0, dispatch_failed
    mov x0, sp
    mov w1, #0x201             /* WINSTA_ENUMDESKTOPS | WINSTA_READSCREEN */
    call_win32 XP_UserOpenWindowStation
    cbz x0, dispatch_failed
    mov x20, x0
    str x20, [x19]
    call_win32 XP_UserSetProcessWindowStation
    cbz w0, dispatch_failed
    call_win32 XP_UserGetProcessWindowStation
    cmp x0, x20
    b.ne dispatch_failed

    adrp x9, other_console_path
    add x9, x9, :lo12:other_console_path
    str x9, [sp, #56]
    mov w9, #other_console_path_end-other_console_path
    strh w9, [sp, #48]
    strh w9, [sp, #50]
    mov x0, sp
    mov w1, #1
    call_win32 XP_UserOpenWindowStation
    cbz x0, dispatch_failed
    mov x22, x0
    str x22, [x19, #8]

    adrp x9, desktop_leaf
    add x9, x9, :lo12:desktop_leaf
    str x9, [sp, #56]
    mov w9, #desktop_leaf_end-desktop_leaf
    strh w9, [sp, #48]
    strh w9, [sp, #50]
    str x20, [sp, #8]
    mov x0, sp
    mov x1, xzr
    mov x2, xzr
    call_win32 XP_UserOpenDesktop
    cbz x0, dispatch_failed
    mov x21, x0
    str x21, [x19, #16]
    call_win32 XP_UserSetThreadDesktop
    cbz w0, dispatch_failed
    ldr w0, [x18, #0x48]
    mov x1, xzr
    call_win32 XP_UserGetThreadDesktop
    cmp x0, x21
    b.ne dispatch_failed

    mov x0, x20
    mov w8, #XP_SERVICE_CLOSE
    svc #0
    str w0, [x19, #48]
    mov x0, x21
    call_win32 XP_UserCloseDesktop
    cbnz w0, dispatch_failed
    mov x0, x20
    call_win32 XP_UserCloseWindowStation
    cbnz w0, dispatch_failed

    str x22, [sp, #8]
    mov x0, sp
    mov x1, xzr
    mov x2, xzr
    call_win32 XP_UserOpenDesktop
    cbz x0, dispatch_failed
    mov x23, x0
    str x23, [x19, #24]
    call_win32 XP_UserSetThreadDesktop
    cbnz w0, dispatch_failed
    mov x0, x22
    call_win32 XP_UserSetProcessWindowStation
    cbnz w0, dispatch_failed
    mov x0, x23
    call_win32 XP_UserCloseDesktop
    cbz w0, dispatch_failed
    mov x0, x22
    call_win32 XP_UserCloseWindowStation
    cbz w0, dispatch_failed

    str x20, [sp, #8]
    mov x0, sp
    mov x1, xzr
    mov w2, #2                /* DESKTOP_CREATEWINDOW */
    call_win32 XP_UserOpenDesktop
    cbz x0, dispatch_failed
    mov x24, x0
    str x24, [x19, #32]
    call_win32 XP_UserSetThreadDesktop
    cbz w0, dispatch_failed
    mov x0, x21
    call_win32 XP_UserCloseDesktop
    cbz w0, dispatch_failed
    ldr w0, [x18, #0x48]
    mov x1, xzr
    call_win32 XP_UserGetThreadDesktop
    cmp x0, x24
    b.ne dispatch_failed
    str x0, [x19, #40]
    add sp, sp, #64
    b user_gdi_program

user_gdi_program:
    mov w0, #8
    mov w1, #6
    mov w2, #1
    mov w3, #32
    adrp x4, source_bits
    add x4, x4, :lo12:source_bits
    call_win32 XP_GdiCreateBitmap
    cbz w0, dispatch_failed
    mov x20, x0
    str x20, [x19, #56]
    mov w0, #16
    mov w1, #12
    mov w2, #1
    mov w3, #32
    mov x4, xzr
    call_win32 XP_GdiCreateBitmap
    cbz w0, dispatch_failed
    mov x21, x0
    str x21, [x19, #64]
    mov x0, xzr
    call_win32 XP_GdiCreateCompatibleDC
    cbz w0, dispatch_failed
    mov x22, x0
    call_win32 XP_GdiCreateCompatibleDC
    cbz w0, dispatch_failed
    mov x23, x0
    mov x0, x22
    mov x1, x20
    call_win32 XP_GdiSelectBitmap
    cbz w0, dispatch_failed
    mov x24, x0                /* Default stock bitmap selected by CreateCompatibleDC. */
    mov x0, x23
    mov x1, x21
    call_win32 XP_GdiSelectBitmap
    cmp x0, x24
    b.ne dispatch_failed
    mov x0, x23
    mov x1, x20
    call_win32 XP_GdiSelectBitmap
    cbnz w0, dispatch_failed   /* Source bitmap is already selected into another DC. */
    mov x0, x22
    mov x1, xzr
    mov x2, xzr
    call_win32 XP_GdiGetPixel
    mov w9, #0x2010
    movk w9, #0x0030, lsl #16
    cmp w0, w9
    b.ne dispatch_failed       /* Input BGR DWORD 0x00102030 becomes COLORREF 0x00302010. */
    str w0, [x19, #72]
    mov x0, x22
    mov w1, #-1
    mov x2, xzr
    call_win32 XP_GdiGetPixel
    cmn w0, #1
    b.ne dispatch_failed

    mov w27, wzr
gdi_paint_row:
    mov w28, wzr
gdi_paint_pixel:
    lsr w9, w28, #1
    eor w9, w9, w27, lsr #1
    tst w9, #1
    mov w10, #0xa020
    movk w10, #0xe0, lsl #16
    mov w11, #0x50f0
    movk w11, #0x30, lsl #16
    csel w25, w10, w11, eq
    mov x0, x22
    mov w1, w28
    mov w2, w27
    mov w3, w25
    call_win32 XP_GdiSetPixel
    cmp w0, w25
    b.ne dispatch_failed
    add w28, w28, #1
    cmp w28, #8
    b.lt gdi_paint_pixel
    add w27, w27, #1
    cmp w27, #6
    b.lt gdi_paint_row

    sub sp, sp, #32
    mov w9, #0x20
    movk w9, #0xcc, lsl #16
    stp x9, xzr, [sp]          /* rop4, background color */
    str xzr, [sp, #16]         /* flags */
    mov x0, x23
    mov w1, #3
    mov w2, #3
    mov w3, #8
    mov w4, #6
    mov x5, x22
    mov x6, xzr
    mov x7, xzr
    call_win32 XP_GdiBitBlt
    cbz w0, dispatch_failed
    mov x0, x23
    mov w1, #-3
    mov w2, #-2
    mov w3, #8
    mov w4, #6
    mov x5, x22
    mov x6, xzr
    mov x7, xzr
    call_win32 XP_GdiBitBlt
    cbz w0, dispatch_failed
    mov x0, x23
    mov w1, #5
    mov w2, #5
    mov w3, #8
    mov w4, #6
    mov x5, x23
    mov w6, #3
    mov w7, #3
    call_win32 XP_GdiBitBlt
    cbz w0, dispatch_failed
    add sp, sp, #32

    mov x0, x21
    mov x1, xzr
    mov x2, xzr
    call_win32 XP_GdiGetBitmapBits
    cmp w0, #768
    b.ne dispatch_failed
    mov x0, x21
    mov w1, #768
    mov x2, #0x1000
    call_win32 XP_GdiGetBitmapBits
    cbnz w0, dispatch_failed
    mov x0, x21
    mov w1, #768
    add x2, x19, #256
    call_win32 XP_GdiGetBitmapBits
    cmp w0, #768
    b.ne dispatch_failed
    str w0, [x19, #76]

    /* Firmware without a writable GOP remains usable headlessly. */
    mov x0, xzr
    call_win32 XP_UserGetDC
    cbz w0, gdi_display_done
    mov x26, x0
    str x26, [x19, #80]
    sub sp, sp, #32
    mov w9, #0x20
    movk w9, #0xcc, lsl #16
    stp x9, xzr, [sp]
    str xzr, [sp, #16]
    mov w27, #32
gdi_display_row:
    mov w28, #32
gdi_display_tile:
    mov x0, x26
    mov w1, w28
    mov w2, w27
    mov w3, #16
    mov w4, #12
    mov x5, x23
    mov x6, xzr
    mov x7, xzr
    call_win32 XP_GdiBitBlt
    cbz w0, dispatch_failed
    add w28, w28, #16
    cmp w28, #192
    b.lt gdi_display_tile
    add w27, w27, #12
    cmp w27, #152
    b.lt gdi_display_row
    add sp, sp, #32
    mov x0, x26
    mov w1, #32
    mov w2, #32
    call_win32 XP_GdiGetPixel
    mov w9, #0xa020
    movk w9, #0xe0, lsl #16
    cmp w0, w9
    b.ne dispatch_failed
    str w0, [x19, #88]
gdi_display_done:
    mov x0, x20
    call_win32 XP_GdiDeleteObjectApp
    cbnz w0, dispatch_failed
    mov x0, x22
    mov x1, x24
    call_win32 XP_GdiSelectBitmap
    cmp x0, x20
    b.ne dispatch_failed
    mov x0, x20
    call_win32 XP_GdiDeleteObjectApp
    cbz w0, dispatch_failed
    mov x0, x20
    mov x1, xzr
    mov x2, xzr
    call_win32 XP_GdiGetBitmapBits
    cbnz w0, dispatch_failed
    mov x0, x22
    call_win32 XP_GdiDeleteObjectApp
    cbz w0, dispatch_failed
    b user_window_program

/* class + two overlapping popups, client clipping, show/hide and rundown */
.macro window_call service, source_line
    mov w9, #\source_line
    str w9, [x19, #132]
    call_win32 \service
    str w0, [x19, #136]
.endm

user_window_program:
    sub sp, sp, #224
    mov x9, #0
window_zero_stack:
    str xzr, [sp, x9]
    add x9, x9, #8
    cmp x9, #224
    b.lo window_zero_stack
    mov w9, #80
    str w9, [sp, #64]          /* WNDCLASSEX */
    adr x9, window_procedure
    str x9, [sp, #72]
    adrp x9, window_class_name
    add x9, x9, :lo12:window_class_name
    str x9, [sp, #128]
    str x9, [sp, #152]         /* WNDCLASSEX and UNICODE_STRING name */
    mov w10, #window_class_name_end-window_class_name
    strh w10, [sp, #144]
    strh w10, [sp, #146]
    add x9, sp, #200
    str x9, [sp, #192]         /* CLSMENUNAME points to empty UNICODE_STRING */
    mov x0, #0x1000
    add x1, sp, #144
    mov x2, x1
    add x3, sp, #176
    mov w4, wzr
    mov w5, #0x80             /* CSF_WIN40COMPAT */
    mov x6, xzr
    window_call XP_UserRegisterClassExWOW, __LINE__
    cbnz w0, dispatch_failed
    add x0, sp, #64
    add x1, sp, #144
    mov x2, x1
    add x3, sp, #176
    mov w4, wzr
    mov w5, #0x80
    mov x6, xzr
    window_call XP_UserRegisterClassExWOW, __LINE__
    cmp w0, #0xc000
    b.lo dispatch_failed
    mov w20, w0
    str x20, [x19, #96]
    add x0, sp, #64
    add x1, sp, #144
    mov x2, x1
    add x3, sp, #176
    mov w4, wzr
    mov w5, #0x80
    mov x6, xzr
    window_call XP_UserRegisterClassExWOW, __LINE__
    cbnz w0, dispatch_failed

    mov x9, #10
    str x9, [sp]              /* nHeight */
    mov x9, #1
    str x9, [sp, #48]         /* pActCtx, fifteenth argument */
    mov w0, wzr
    mov w1, w20
    mov w2, w20
    mov x3, xzr
    mov w4, #0x90000000
    mov w5, #256
    mov w6, #64
    mov w7, #12
    window_call XP_UserCreateWindowEx, __LINE__
    cbnz w0, dispatch_failed
    str xzr, [sp, #48]
    mov w0, wzr
    mov w1, w20
    mov w2, w20
    mov x3, xzr
    mov w4, #0x90000000
    mov w5, #256
    mov w6, #64
    mov w7, #12
    window_call XP_UserCreateWindowEx, __LINE__
    cbz w0, dispatch_failed
    mov w25, w0
    str x25, [x19, #104]
    window_call XP_UserGetDC, __LINE__
    cbz w0, dispatch_failed
    mov w27, w0

    mov w9, #0x20
    movk w9, #0xcc, lsl #16
    str x9, [sp]
    mov x0, x27
    mov w1, #-2
    mov w2, #-1
    mov w3, #16
    mov w4, #12
    mov x5, x23
    mov x6, xzr
    mov x7, xzr
    window_call XP_GdiBitBlt, __LINE__
    cbz w0, dispatch_failed
    mov x0, x27
    mov w1, #12
    mov w2, #2
    mov w3, #0xffff
    window_call XP_GdiSetPixel, __LINE__
    cmn w0, #1
    b.ne dispatch_failed
    mov x0, x27
    mov w1, #1
    mov w2, #2
    window_call XP_GdiGetPixel, __LINE__
    mov w9, #0x50f0
    movk w9, #0x30, lsl #16
    cmp w0, w9                 /* Destination tile at (3,3), after the clipped blit. */
    b.ne dispatch_failed

    mov x9, #8
    str x9, [sp]
    mov w0, wzr
    mov w1, w20
    mov w2, w20
    mov x3, xzr
    mov w4, #0x80000000
    mov w5, #262
    mov w6, #68
    mov w7, #8
    window_call XP_UserCreateWindowEx, __LINE__
    cbz w0, dispatch_failed
    mov w28, w0
    str x28, [x19, #112]
    window_call XP_UserGetDC, __LINE__
    cbz w0, dispatch_failed
    mov w24, w0
    mov x0, x24
    mov w1, #1
    mov w2, #1
    mov w3, #0xf8ce
    movk w3, #0x4a, lsl #16
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed
    mov x0, x28
    mov w1, #5
    window_call XP_UserShowWindow, __LINE__
    cbnz w0, dispatch_failed
    mov x0, x27
    mov w1, #7
    mov w2, #5
    mov w3, #0xa5f3
    movk w3, #0x22, lsl #16
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed

    add x10, x19, #128
    mov w9, #1
    stlr w9, [x10]
window_wait_for_inspection:
    yield
    ldar w9, [x10]
    cmp w9, #2
    b.ne window_wait_for_inspection

    mov x0, x28
    mov w1, wzr
    window_call XP_UserShowWindow, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr x26, [x19, #80]
    cbz x26, window_no_screen
    mov x0, x26
    mov w1, #263
    mov w2, #69
    window_call XP_GdiGetPixel, __LINE__
    mov w9, #0xa5f3
    movk w9, #0x22, lsl #16
    cmp w0, w9
    b.ne dispatch_failed
    str w0, [x19, #120]
window_no_screen:
    mov x0, x24
    window_call XP_GdiDeleteObjectApp, __LINE__
    cbnz w0, dispatch_failed
    mov x0, x24
    mov w1, #XP_SFI_RELEASEDC
    window_call XP_UserCallOneParam, __LINE__
    cbz w0, dispatch_failed
    mov x0, x24
    mov w1, #XP_SFI_RELEASEDC
    window_call XP_UserCallOneParam, __LINE__
    cbnz w0, dispatch_failed
    mov x0, x28
    window_call XP_UserGetDC, __LINE__
    cbz w0, dispatch_failed
    mov w24, w0
    mov x0, x28
    mov w1, #0x8055
    mov x2, xzr
    mov x3, xzr
    window_call XP_UserPostMessage, __LINE__
    cbz w0, dispatch_failed
    mov x0, x28
    window_call XP_UserDestroyWindow, __LINE__
    cbz w0, dispatch_failed
    mov x0, x28
    window_call XP_UserGetDC, __LINE__
    cbnz w0, dispatch_failed
    mov x0, x24
    mov x1, xzr
    mov x2, xzr
    window_call XP_GdiGetPixel, __LINE__
    cmn w0, #1
    b.ne dispatch_failed
    mov x0, x28
    window_call XP_UserDestroyWindow, __LINE__
    cbnz w0, dispatch_failed

    bl window_paint_probe
    bl window_message_probe
    bl window_input_probe
    mov x9, #4
    str x9, [sp]
    mov w0, wzr
    mov w1, w20
    mov w2, w20
    mov x3, xzr
    mov w4, #0x90000000
    mov w5, #-2
    mov w6, #-2
    mov w7, #4
    window_call XP_UserCreateWindowEx, __LINE__
    cbz w0, dispatch_failed
    cmp w0, w28
    b.eq dispatch_failed
    str w0, [x19, #124]
    window_call XP_UserGetDC, __LINE__
    cbz w0, dispatch_failed
    mov w1, #2
    mov w2, #2
    mov w3, #0x1739
    window_call XP_GdiSetPixel, __LINE__
    mov w9, #0x1739
    cmp w0, w9
    b.ne dispatch_failed
    cbz x26, window_exit
    mov x0, x26
    mov w1, wzr
    mov w2, wzr
    window_call XP_GdiGetPixel, __LINE__
    mov w9, #0x1739
    cmp w0, w9
    b.ne dispatch_failed
window_exit:
    add sp, sp, #224
    /* Two windows, their DCs and the class still belong to this thread/process. */
    mov x0, #-2
    mov w1, #42
    mov w8, #XP_SERVICE_TERMINATE_THREAD
    svc #0
    b dispatch_failed

/* The three services below use the same 16-byte RECT and half-open bounds. */
.macro inval_rect left, top, right, bottom, erase
    mov w9, #\left
    mov w10, #\top
    stp w9, w10, [sp, #48]
    mov w9, #\right
    mov w10, #\bottom
    stp w9, w10, [sp, #56]
    mov x0, x25
    add x1, sp, #48
    mov w2, #\erase
    window_call XP_UserInvalidateRect, __LINE__
    cbz w0, dispatch_failed
.endm

.macro check_paint_rect displacement, left, top, right, bottom
    ldp w9, w10, [sp, #\displacement]
    cmp w9, #\left
    b.ne dispatch_failed
    cmp w10, #\top
    b.ne dispatch_failed
    ldp w9, w10, [sp, #(\displacement + 8)]
    cmp w9, #\right
    b.ne dispatch_failed
    cmp w10, #\bottom
    b.ne dispatch_failed
.endm

.macro check_window_pixel dc, x, y, color
    mov x0, \dc
    mov w1, #\x
    mov w2, #\y
    window_call XP_GdiGetPixel, __LINE__
    movz w9, #(\color & 0xffff)
    movk w9, #(\color >> 16), lsl #16
    cmp w0, w9
    b.ne dispatch_failed
.endm

window_paint_probe:
    sub sp, sp, #96
    str x30, [sp, #88]

    mov x0, x25
    mov x1, #0x1000
    window_call XP_UserBeginPaint, __LINE__
    cbnz x0, dispatch_failed
    mov x0, x25
    add x1, sp, #48
    mov w2, wzr
    window_call XP_UserGetUpdateRect, __LINE__
    cbz w0, dispatch_failed
    check_paint_rect 48, 0, 0, 12, 10
    mov x0, x25
    mov x1, sp
    window_call XP_UserBeginPaint, __LINE__
    cbz x0, dispatch_failed
    check_paint_rect 12, 0, 0, 12, 10
    mov x0, x25
    mov x1, sp
    window_call XP_UserEndPaint, __LINE__
    cbz w0, dispatch_failed

    mov x0, x27
    mov w1, #5
    mov w2, #4
    mov w3, #0x2e1f
    movk w3, #0x4b, lsl #16
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed
    mov x0, x27
    mov w1, #10
    mov w2, #9
    mov w3, #0x2233
    movk w3, #0x11, lsl #16
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed

    inval_rect 1, 1, 4, 3, 1
    inval_rect 7, 5, 9, 8, 0
    mov x0, x25
    add x1, sp, #48
    mov w2, wzr
    window_call XP_UserGetUpdateRect, __LINE__
    cbz w0, dispatch_failed
    check_paint_rect 48, 1, 1, 9, 8

    mov x9, #0x7777
    str x9, [sp]
    mov x0, sp
    mov x1, x25
    mov w2, #0x20
    mov w3, #0x20
    mov w4, #1
    window_call XP_UserPeekMessage, __LINE__
    cbnz w0, dispatch_failed
    ldr x9, [sp]
    mov x10, #0x7777
    cmp x9, x10
    b.ne dispatch_failed
    mov x0, #0x1000
    mov x1, x25
    mov w2, #0xf
    mov w3, #0xf
    mov w4, #1
    window_call XP_UserPeekMessage, __LINE__
    cbnz w0, dispatch_failed
    mov x0, sp
    mov x1, x25
    mov w2, #0xf
    mov w3, #0xf
    mov w4, #0x200000
    window_call XP_UserPeekMessage, __LINE__
    cbz w0, dispatch_failed
    mov x0, sp
    mov x1, x25
    mov w2, #0xf
    mov w3, #0xf
    mov w4, #1
    window_call XP_UserPeekMessage, __LINE__
    cbz w0, dispatch_failed
    ldr x9, [sp]
    cmp x9, x25
    b.ne dispatch_failed
    ldr w9, [sp, #8]
    cmp w9, #0xf
    b.ne dispatch_failed

    mov w9, #1
    str w9, [x19, #144]
    mov x0, sp
    window_call XP_UserDispatchMessage, __LINE__
    movz x9, #0x7788
    movk x9, #0x5566, lsl #16
    movk x9, #0x3344, lsl #32
    movk x9, #0x1122, lsl #48
    cmp x0, x9
    b.ne dispatch_failed
    str x0, [x19, #168]

    check_window_pixel x27, 1, 1, 0x3050f0
    str w0, [x19, #164]
    check_window_pixel x27, 5, 4, 0x4b2e1f
    str w0, [x19, #156]
    check_window_pixel x27, 7, 5, 0x631450
    check_window_pixel x27, 10, 9, 0x112233
    mov x0, x25
    add x1, sp, #48
    mov w2, wzr
    window_call XP_UserGetUpdateRect, __LINE__
    cbz w0, dispatch_failed
    check_paint_rect 48, 10, 9, 12, 10

    mov w9, #2
    str w9, [x19, #144]
    mov x0, x25
    mov w1, #XP_SFI_UPDATEWINDOW
    window_call XP_UserCallHwndLock, __LINE__
    cbz w0, dispatch_failed
    check_window_pixel x27, 10, 9, 0x674455
    str w0, [x19, #160]

    mov x0, x25
    add x1, sp, #48
    mov w2, wzr
    window_call XP_UserGetUpdateRect, __LINE__
    cbnz w0, dispatch_failed
    check_paint_rect 48, 0, 0, 0, 0
    mov x0, x25
    mov w1, #XP_SFI_UPDATEWINDOW
    window_call XP_UserCallHwndLock, __LINE__
    cbz w0, dispatch_failed
    mov x0, sp
    mov x1, x25
    mov w2, wzr
    mov w3, wzr
    mov w4, #1
    window_call XP_UserPeekMessage, __LINE__
    cbnz w0, dispatch_failed

    cbz x26, paint_probe_done
    check_window_pixel x26, 261, 68, 0x4b2e1f
    check_window_pixel x26, 266, 73, 0x674455
    check_window_pixel x26, 257, 65, 0x3050f0
paint_probe_done:
    ldr x30, [sp, #88]
    add sp, sp, #96
    ret

#define TEST_WINDOW_MESSAGE 0x8031
#define TEST_FOLLOWUP_MESSAGE 0x8032
#define TEST_LOCAL_THREAD_MESSAGE 0x8012
#define TEST_PEER_THREAD_MESSAGE 0x8013

window_message_probe:
    sub sp, sp, #64
    str x30, [sp, #56]

    mov x0, x25
    mov w1, #0xc                 /* WM_SETTEXT must remain synchronous. */
    mov x2, xzr
    mov x3, sp
    window_call XP_UserPostMessage, __LINE__
    cbnz w0, dispatch_failed
    mov x0, xzr
    mov w1, #TEST_LOCAL_THREAD_MESSAGE
    mov w2, #0x1234
    mov w3, #0x5678
    window_call XP_UserPostMessage, __LINE__
    cbz w0, dispatch_failed
    ldr w9, [x18, #0x48]
    str w9, [x19, #188]          /* Receiver TID for the peer's PostThreadMessage. */
    mov w9, #1
    add x10, x19, #176
    stlr w9, [x10]
    /* There is a live but unmatched thread message in the queue here. */
    mov x0, sp
    mov x1, x25
    mov w2, #TEST_WINDOW_MESSAGE
    mov w3, #TEST_WINDOW_MESSAGE
    window_call XP_UserGetMessage, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr x9, [sp]
    cmp x9, x25
    b.ne dispatch_failed
    ldr w9, [sp, #8]
    mov w10, #TEST_WINDOW_MESSAGE
    cmp w9, w10
    b.ne dispatch_failed
    mov x0, sp
    window_call XP_UserDispatchMessage, __LINE__
    movz x9, #0x3344
    movk x9, #0x1122, lsl #16
    movk x9, #0xface, lsl #32
    movk x9, #0xfeed, lsl #48
    cmp x0, x9
    b.ne dispatch_failed
    str x0, [x19, #192]

    add x10, x19, #184
queue_wait_for_peer:
    ldar w9, [x10]
    cbnz w9, queue_peer_ready
    yield
    b queue_wait_for_peer
queue_peer_ready:
    mov x0, #0x1000             /* Rejected copyout must preserve the post. */
    mov x1, #-1
    mov w2, wzr
    mov w3, wzr
    mov w4, #1
    window_call XP_UserPeekMessage, __LINE__
    cbnz w0, dispatch_failed
    mov x0, sp
    mov x1, #-1
    mov w2, wzr
    mov w3, wzr
    mov w4, wzr                  /* PM_NOREMOVE and thread-only filter. */
    window_call XP_UserPeekMessage, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr x9, [sp]
    cbnz x9, dispatch_failed
    ldr w9, [sp, #8]
    mov w10, #TEST_LOCAL_THREAD_MESSAGE
    cmp w9, w10
    b.ne dispatch_failed

    mov w0, #27
    mov w1, #XP_SFI_POSTQUITMESSAGE
    window_call XP_UserCallOneParam, __LINE__
    cbz w0, dispatch_failed
    mov x0, sp
    mov x1, xzr
    mov w2, wzr
    mov w3, wzr
    window_call XP_UserGetMessage, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr w9, [sp, #8]
    mov w10, #TEST_LOCAL_THREAD_MESSAGE
    cmp w9, w10
    b.ne dispatch_failed
    ldp x9, x10, [sp, #16]
    mov x11, #0x1234
    cmp x9, x11
    b.ne dispatch_failed
    mov x11, #0x5678
    cmp x10, x11
    b.ne dispatch_failed
    mov x0, sp
    mov x1, #-1
    mov w2, wzr
    mov w3, wzr
    window_call XP_UserGetMessage, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr w9, [sp, #8]
    mov w10, #TEST_PEER_THREAD_MESSAGE
    cmp w9, w10
    b.ne dispatch_failed
    ldp x9, x10, [sp, #16]
    mov x11, #0x5152
    cmp x9, x11
    b.ne dispatch_failed
    mov x11, #0x5354
    cmp x10, x11
    b.ne dispatch_failed
    mov x0, sp
    mov x1, x25
    mov w2, wzr
    mov w3, wzr
    window_call XP_UserGetMessage, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr w9, [sp, #8]
    mov w10, #TEST_FOLLOWUP_MESSAGE
    cmp w9, w10
    b.ne dispatch_failed
    ldr x9, [sp, #16]
    cmp x9, #43
    b.ne dispatch_failed

    mov x0, sp
    mov x1, x25
    mov w2, #0x9000              /* Synthesize WM_QUIT after posted messages. */
    mov w3, #0x9000
    window_call XP_UserGetMessage, __LINE__
    cbnz w0, dispatch_failed
    ldr x9, [sp]
    cbnz x9, dispatch_failed
    ldr w9, [sp, #8]
    cmp w9, #0x12
    b.ne dispatch_failed
    ldr x9, [sp, #16]
    cmp x9, #27
    b.ne dispatch_failed
    str w9, [x19, #200]

    mov x0, #0x1000
    mov x1, x25
    mov w2, wzr
    mov w3, wzr
    window_call XP_UserGetMessage, __LINE__
    cmn w0, #1
    b.ne dispatch_failed
    mov x0, sp
    mov x1, x28                 /* A destroyed HWND must return -1. */
    mov w2, wzr
    mov w3, wzr
    window_call XP_UserGetMessage, __LINE__
    cmn w0, #1
    b.ne dispatch_failed
    ldr x30, [sp, #56]
    add sp, sp, #64
    ret

user_post_program:
    mov x19, x0
    ldr x0, [x19, #32]
    call_win32 XP_UserSetThreadDesktop
    cbz w0, dispatch_failed
    mov w0, #0x7fff
    mov w1, #TEST_PEER_THREAD_MESSAGE
    mov x2, xzr
    mov x3, xzr
    call_win32 XP_UserPostThreadMessage
    cbnz w0, dispatch_failed
    ldr w0, [x19, #188]
    mov w1, #TEST_PEER_THREAD_MESSAGE
    mov w2, #0x5152
    mov w3, #0x5354
    call_win32 XP_UserPostThreadMessage
    cbz w0, dispatch_failed
    ldr x0, [x19, #104]
    mov w1, #TEST_WINDOW_MESSAGE
    movz x2, #0x7788
    movk x2, #0x5566, lsl #16
    movk x2, #0x3344, lsl #32
    movk x2, #0x1122, lsl #48
    movz x3, #0x3210
    movk x3, #0x7654, lsl #16
    movk x3, #0xba98, lsl #32
    movk x3, #0xfedc, lsl #48
    call_win32 XP_UserPostMessage
    cbz w0, dispatch_failed
    ldr x0, [x19, #104]
    mov w1, #TEST_FOLLOWUP_MESSAGE
    mov w2, #43
    mov x3, xzr
    call_win32 XP_UserPostMessage
    cbz w0, dispatch_failed
    mov w9, #0x4422
    add x10, x19, #184
    stlr w9, [x10]
    mov x0, #-2
    mov w1, #42
    mov w8, #XP_SERVICE_TERMINATE_THREAD
    svc #0
    b dispatch_failed

window_posted_message:
    movz x9, #0x7788
    movk x9, #0x5566, lsl #16
    movk x9, #0x3344, lsl #32
    movk x9, #0x1122, lsl #48
    cmp x2, x9
    b.ne dispatch_failed
    movz x9, #0x3210
    movk x9, #0x7654, lsl #16
    movk x9, #0xba98, lsl #32
    movk x9, #0xfedc, lsl #48
    cmp x3, x9
    b.ne dispatch_failed
    ldr w9, [x19, #180]
    add w9, w9, #1
    str w9, [x19, #180]
    movz x0, #0x3344
    movk x0, #0x1122, lsl #16
    movk x0, #0xface, lsl #32
    movk x0, #0xfeed, lsl #48
    ret

.macro receive_input message, wparam, lparam
    mov x0, sp
    mov x1, x25
    mov w2, #0x100
    mov w3, #0x202
    window_call XP_UserGetMessage, __LINE__
    cmp w0, #1
    b.ne dispatch_failed
    ldr x9, [sp]
    cmp x9, x25
    b.ne dispatch_failed
    ldr w9, [sp, #8]
    cmp w9, #\message
    b.ne dispatch_failed
    ldr x9, [sp, #16]
    cmp x9, #\wparam
    b.ne dispatch_failed
    ldr x9, [sp, #24]
    movz x10, #(\lparam & 0xffff)
    movk x10, #(\lparam >> 16), lsl #16
    cmp x9, x10
    b.ne dispatch_failed
    ldp w9, w10, [sp, #36]
    cmp w9, #260
    b.ne dispatch_failed
    cmp w10, #68
    b.ne dispatch_failed
    mov x0, sp
    window_call XP_UserDispatchMessage, __LINE__
    cmp x0, #\message
    b.ne dispatch_failed
.endm

window_input_probe:
    ldr w9, [x19, #208]
    cbz w9, no_virtio_probe
    sub sp, sp, #64
    str x30, [sp, #56]
    mov w9, #1
    add x10, x19, #212
    stlr w9, [x10]
    receive_input 0x200, 0, 0x00040004
    receive_input 0x201, 1, 0x00040004
    receive_input 0x202, 0, 0x00040004
    mov w9, #2
    add x10, x19, #212
    stlr w9, [x10]
    receive_input 0x100, 0x41, 0x001e0001
    receive_input 0x101, 0x41, 0xc01e0001
    check_window_pixel x27, 4, 4, 0x7cab5e
    str w0, [x19, #220]
    check_window_pixel x27, 1, 1, 0x4392b1
    str w0, [x19, #224]
    check_window_pixel x26, 260, 68, 0x7cab5e
    check_window_pixel x26, 257, 65, 0x4392b1
    mov w9, #3
    str w9, [x19, #212]
    ldr x30, [sp, #56]
    add sp, sp, #64
no_virtio_probe:
    ret

window_input_message:
    ldr w9, [x19, #216]
    add w9, w9, #1
    str w9, [x19, #216]
    cmp w1, #0x201
    b.eq window_click_paint
    cmp w1, #0x100
    b.eq window_key_paint
    mov w0, w1
    ret
window_click_paint:
    mov w3, #0xab5e
    movk w3, #0x7c, lsl #16
    mov w1, #4
    mov w2, #4
    mov x0, x27
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed
    mov w0, #0x201
    ret
window_key_paint:
    mov w3, #0x92b1
    movk w3, #0x43, lsl #16
    mov w1, #1
    mov w2, #1
    mov x0, x27
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed
    mov w0, #0x100
    ret

/* The port's native callback dispatcher invokes an executable registered
 * procedure from the fnDWORD record and returns a native CALLBACKSTATUS. */
dispatch_user_window:
    cmp w0, #XP_FI_FNDWORD
    b.ne dispatch_failed
    cmp w2, #48
    b.ne dispatch_failed
    ldr x16, [x1, #40]
    ldp x2, x3, [x1, #16]
    ldr x0, [x1]
    ldr w1, [x1, #8]
    blr x16
    sub sp, sp, #32
    stp x0, xzr, [sp]
    str xzr, [sp, #16]
    mov x0, sp
    mov w1, #24
    mov w2, wzr
    mov w8, #XP_SERVICE_CALLBACK_RETURN
    svc #0
    b dispatch_failed

window_procedure:
    cmp x0, x25
    b.ne dispatch_failed
    mov w9, #TEST_WINDOW_MESSAGE
    cmp w1, w9
    b.eq window_posted_message
    cmp w1, #0x100
    b.eq window_input_message
    cmp w1, #0x101
    b.eq window_input_message
    sub w9, w1, #0x200
    cmp w9, #2
    b.ls window_input_message
    cbnz x3, dispatch_failed
    cmp w1, #0x14
    b.eq window_erase_background
    cmp w1, #0xf
    b.ne dispatch_failed
    cbnz x2, dispatch_failed
    sub sp, sp, #128
    str x30, [sp, #120]
    ldr w9, [x19, #148]
    add w9, w9, #1
    str w9, [x19, #148]
    mov x1, sp
    window_call XP_UserBeginPaint, __LINE__
    cbz x0, dispatch_failed
    str x0, [sp, #80]
    ldr w9, [sp, #8]             /* The erase callback handled the background. */
    cbnz w9, dispatch_failed
    ldr w9, [x19, #144]
    cmp w9, #2
    b.eq paint_second_update
    check_paint_rect 12, 1, 1, 9, 8

    ldr x0, [sp, #80]
    mov w1, #XP_SFI_RELEASEDC
    window_call XP_UserCallOneParam, __LINE__
    cbnz w0, dispatch_failed
    mov x0, x25
    add x1, sp, #80
    window_call XP_UserBeginPaint, __LINE__
    cbnz x0, dispatch_failed
    ldr x0, [sp, #80]
    mov w1, #5
    mov w2, #4
    mov w3, #0xffff
    window_call XP_GdiSetPixel, __LINE__
    cmn w0, #1
    b.ne dispatch_failed

    ldr x0, [sp, #80]
    sub sp, sp, #32
    mov w9, #0x20
    movk w9, #0xcc, lsl #16
    stp x9, xzr, [sp]
    str xzr, [sp, #16]
    mov x1, xzr
    mov x2, xzr
    mov w3, #12
    mov w4, #10
    mov x5, x23
    mov x6, xzr
    mov x7, xzr
    window_call XP_GdiBitBlt, __LINE__
    cbz w0, dispatch_failed
    add sp, sp, #32
    ldr x0, [sp, #80]
    mov w1, #7
    mov w2, #5
    mov w3, #0x1450
    movk w3, #0x63, lsl #16
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed
    b finish_window_paint

paint_second_update:
    check_paint_rect 12, 10, 9, 12, 10
    ldr x0, [sp, #80]
    mov w1, #10
    mov w2, #9
    mov w3, #0x4455
    movk w3, #0x67, lsl #16
    window_call XP_GdiSetPixel, __LINE__
    cbz w0, dispatch_failed

finish_window_paint:
    mov x0, x25
    mov x1, #0x1000
    window_call XP_UserEndPaint, __LINE__
    cbnz w0, dispatch_failed
    mov x0, x25
    mov x1, sp
    window_call XP_UserEndPaint, __LINE__
    cbz w0, dispatch_failed
    ldr x0, [sp, #80]
    mov w1, #1
    mov w2, #1
    window_call XP_GdiGetPixel, __LINE__
    cmn w0, #1
    b.ne dispatch_failed
    ldr x30, [sp, #120]
    add sp, sp, #128
    movz x0, #0x7788
    movk x0, #0x5566, lsl #16
    movk x0, #0x3344, lsl #32
    movk x0, #0x1122, lsl #48
    ret

window_erase_background:
    sub sp, sp, #80
    str x30, [sp, #72]
    ldr w9, [x19, #152]
    add w9, w9, #1
    str w9, [x19, #152]
    mov x0, x2
    mov w1, #1
    mov w2, #1
    mov w3, #0x8888
    window_call XP_GdiSetPixel, __LINE__
    mov w9, #0x8888
    cmp w0, w9
    b.ne dispatch_failed
    inval_rect 10, 9, 12, 10, 0
    ldr x30, [sp, #72]
    add sp, sp, #80
    mov w0, #1
    ret

.section .rdata,"dr"
.p2align 1
window_class_name:
    .short 'A','r','m','6','4','R','a','s','t','e','r'
window_class_name_end:
console_path:
    .short 92, 'W','i','n','d','o','w','s',92,'W','i','n','d','o','w','S','t','a','t','i','o','n','s',92
    .short 'w','i','n','s','t','a','0'
console_path_end:
other_console_path:
    .short 92, 'W','i','n','d','o','w','s',92,'W','i','n','d','o','w','S','t','a','t','i','o','n','s',92
    .short 'S','e','r','v','i','c','e','S','t','a','t','i','o','n'
other_console_path_end:
desktop_leaf:
    .short 'D','e','f','a','u','l','t'
desktop_leaf_end:

.p2align 2
source_bits:
    .rept 48
    .long 0x00102030
    .endr

.data
.p2align 3
initialized_pointer:
    .quad zero_tail
.bss
.p2align 12
zero_tail:
    .space 4096
