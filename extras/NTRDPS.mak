# ntrdps for PINT 3.5 (extras/NTRDPS.sh): the tools and options its makefiles take from a toolchain
# file (tools/build/mk of ntrdps: risc35.inc, risc351d.inc), here those of the NT build for ARM
# (build/msvc.sh), with the headers and libraries of INCLUDE and LIB (build/build-extras.sh).
# In src:          nmake -f arm.mak                 ntrdps.exe and ntrdpshk.dll
# In src\drivers:  nmake -f arm.mak DRIVERS=1 ...   the layered display driver

!IFDEF DRIVERS
# layer.mak: a display driver of NT 3.5, a DLL that GDI loads into CSRSS, with the defines
# makefile.def gives it on ARM, linked as the port links a DLL (build/make-dll.sh): no entry,
# the calls of the graphics engine from winsrv.lib, the helpers of the compiler from libc.lib.
CC       = cl
CFLAGS   = -nologo -c -Zl -W3 -O2 -GF- -Oy- -GS- -Gy -Zc:wchar_t- -GR- -Z7 -wd4391 -D_ARM_=1 \
           -DNT_INST=0 -DCONDITION_HANDLING=1 -DWIN32_LEAN_AND_MEAN=1 -DWIN32=100 -D_NT1X_=100 \
           -DDBG=0 -DDEVL=1 -DFPO=1 -DNDEBUG -FIstcrt.h -I.
LINK     = link
LFLAGS   = -nologo -machine:arm -release -opt:ref,noicf -nodefaultlib
IMPLIB   = lib -nologo -machine:arm

DISPDLL_FLAGS = -dll -noentry -subsystem:console
DISPDLL_LIBS  = kernel32.lib winsrv.lib libc.lib
LAYER_DEF     = layer\layer351.def
LAYER_RES     = layer\layer.res
RCFLAGS       =

!include layer.mak

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
LFLAGS     = -nologo -machine:arm -release
IMPLIB     = lib -nologo -machine:arm

EXE_CON    = -subsystem:console
LIBS_MT    = libcmt.lib kernel32.lib
DLL_FLAGS  = -dll -nodefaultlib -subsystem:windows -entry:DllMain
DLL_RTLIB  = libc.lib
WINLIBS    = user32.lib gdi32.lib advapi32.lib wsock32.lib

!include ntrdps.mak
!ENDIF
