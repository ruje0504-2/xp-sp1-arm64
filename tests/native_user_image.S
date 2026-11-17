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
    mov x2, xzr
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
    /* Destination DC and selected bitmap are intentionally left to exit rundown. */
    mov x0, #-2
    mov w1, #42
    mov w8, #XP_SERVICE_TERMINATE_THREAD
    svc #0
    b dispatch_failed

.section .rdata,"dr"
.p2align 1
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
