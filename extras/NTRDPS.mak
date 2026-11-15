# ntrdps for PINT 4.0 (extras/NTRDPS.sh): the tools and options its makefiles take from a toolchain
# file (tools/build/mk of ntrdps: risc35.inc, risc40d.inc), here those of the NT build for ARM
# (build/msvc.sh), with the headers and libraries of INCLUDE and LIB (build/build-extras.sh, ENV=nt).
# In src:          nmake -f arm.mak                 ntrdps.exe and ntrdpshk.dll
# In src\drivers:  nmake -f arm.mak DRIVERS=1 ...   the mirror driver and its video miniport

!IFDEF DRIVERS
# mirror.mak: kernel mode, the defines makefile.def gives a driver on ARM, images of the native
# subsystem that import from win32k.sys (the display driver) or videoprt.sys (the miniport) alone,
# the helpers of the compiler from libcntpr.lib. NT 4.0's win32k.sys has no event calls
# (EngMapEvent): its mirror driver is ntrdpsm0.dll, the one for NT 4.0 before Service Pack 3.
CC       = cl
CFLAGS   = -nologo -c -Zl -kernel -W3 -O2 -GF- -Oy- -GS- -Gy -Zc:wchar_t- -GR- -Z7 -wd4391 -D_ARM_=1 \
           -DNT_INST=0 -DCONDITION_HANDLING=1 -DWIN32_LEAN_AND_MEAN=1 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 \
           -D_WIN32_WINNT=0x0400 -DDEVL=1 -DFPO=1 -DNDEBUG -FIwarning.h -FIstcrt.h -I.
CFLAGS_MINIPORT = $(CFLAGS)
LINK     = link
LFLAGS   = -nologo -machine:arm -release -opt:ref,noicf -driver -nodefaultlib -version:4.00 -osversion:4.00
IMPLIB   = lib -nologo -machine:arm
RC       = rc

GDIDRV_FLAGS   = -dll -subsystem:native -base:0x10000 -entry:DrvEnableDriver
GDIDRV_LIBS    = win32k.lib libcntpr.lib
MINIPORT_FLAGS = -subsystem:native -base:0x10000 -entry:DriverEntry
MINIPORT_LIBS  = videoprt.lib libcntpr.lib

!include mirror.mak

!ELSE
# ntrdps.mak: with TLS and the login, BearSSL at -Od, PINT's static C run-times (libcmt.lib for
# the service, libc.lib for the hook DLL, single-threaded: stcrt.h).
CC         = cl
CFLAGS     = -nologo -c -Zl -W3 -O2 -GF- -Oy- -GS- -Gy -Zc:wchar_t- -GR- -Z7 -wd4391 -D_ARM_=1 -DWIN32 -D_WIN32
CFLAGS_MT  = $(CFLAGS) -MT -DNTRDPS_TLS -DNTRDPS_LOGIN
CFLAGS_DLL = $(CFLAGS) -FIstcrt.h
TLS        = 1
BRCFLAGS   = -nologo -c -Zl -W0 -Od -GS- -Zc:wchar_t- -GR- -Z7 -D_ARM_=1 -DWIN32 -D_WIN32 -MT
BRSTDINT   = -Itls\bearssl\compat

LINK       = link
LFLAGS     = -nologo -machine:arm -release -version:4.00 -osversion:4.00
IMPLIB     = lib -nologo -machine:arm

EXE_CON    = -subsystem:console
LIBS_MT    = libcmt.lib kernel32.lib
DLL_FLAGS  = -dll -nodefaultlib -subsystem:windows -entry:DllMain
DLL_RTLIB  = libc.lib
WINLIBS    = user32.lib gdi32.lib advapi32.lib wsock32.lib

!include ntrdps.mak
!ENDIF
