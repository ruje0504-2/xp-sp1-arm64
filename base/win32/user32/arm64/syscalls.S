#include "xp-win32-services.h"

.text
.p2align 2
.macro user_service name, service
.globl \name
\name:
    mov w8, #\service
    svc #0
    ret
.endm

user_service XpUserInitializeClient, XP_UserInitializeClientPfnArrays
user_service XpUserRegisterClass, XP_UserRegisterClassExWOW
user_service XpUserSetWindowFnid, XP_UserSetWindowFNID
user_service XpUserInvalidate, XP_UserInvalidateRect
user_service XpUserBeginPaint, XP_UserBeginPaint
user_service XpUserEndPaint, XP_UserEndPaint
user_service XpUserDrawText, XP_GdiExtTextOutW
