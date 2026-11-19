PINT 3.5 SDK
============

Builds programs, DLLs and drivers for PINT 3.5, Windows NT 3.5 on ARM (Raspberry Pi 2, 3,
4, Zero 2 W), with Microsoft C for ARM (MSVC 14.44, the last with an ARM32 target): on
Windows with Visual Studio 2022, or in Docker (tenox7/pint35sdk).

It has the headers of Windows NT 3.5 (Win32, the C run-time, OpenGL) as PINT builds with
them, ahead of the compiler's own (intrin.h, stdint.h, sal.h...), PINT's libraries (the
import libraries of its DLLs and its static C run-times), the ntwin32.mak of the Win32 SDK
with ARM in it, the headers and libraries of a driver kit (Drivers, below), and its own cl
and link: they run those of Microsoft C for ARM and mark each image for Windows NT 3.5 (the
linker for ARM marks it 6.02, which NT 3.5 does not start). No MFC.


Windows
-------

1. Visual Studio 2022 or its Build Tools, with the components
   "MSVC v143 - VS 2022 C++ ARM build tools (Latest)" and a Windows SDK (for rc).
   Visual Studio 2026 has no ARM32 compiler.
2. Unzip this SDK, then in a command prompt:

       C:\pint35sdk\setenv.cmd
       cd C:\pint35sdk\samples\hello
       cl hello.c


Docker
------

Once, install Microsoft C for ARM into a volume (it downloads from Microsoft;
ACCEPT_MSVC_LICENSE=yes accepts the Visual Studio Build Tools license,
https://go.microsoft.com/fwlink/?LinkId=2179911):

    docker run --rm -e ACCEPT_MSVC_LICENSE=yes -v msvc-arm32:/opt/msvc tenox7/pint35sdk msvc-setup

Then, in the directory of the sources:

    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint35sdk cl hello.c
    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint35sdk nmake

The commands are cl, link, nmake and rc of the SDK, and lib, dumpbin, editbin of Microsoft C.
The sources are in /src (Z:\src for the tools). The SDK is /opt/pint35sdk.


Building
--------

    cl hello.c                         a console program, hello.exe
    cl app.c app.res -link -subsystem:windows
                                       a Windows program (WinMain), with resources (rc -r app.rc)
    cl -LD lib.c                       a DLL, lib.dll and lib.lib
    nmake                              a makefile of the Win32 SDK: !include <ntwin32.mak>

The C run-time is static: libcmt (the default), or libc, single-threaded (compile with
-FIstcrt.h, link with -nodefaultlib:libcmt.lib libc.lib). cl links the
libraries a Visual C++ project links: kernel32, user32, gdi32, winspool, comdlg32, advapi32,
shell32, ole32, oleaut32, uuid. Name the others (wsock32.lib, comctl32.lib, winmm.lib...),
as with any Microsoft C. The images are marked for Windows NT 3.5 (subsystem version 3.50);
-subsystem:windows,3.10 marks them 3.10.

ntwin32.mak is that of the Win32 SDK for Windows NT 3.5 with CPU=ARM: $(cc) $(cflags)
$(cvars), $(link) $(conflags) ... $(conlibs), $(guiflags), $(guilibs) and the mt forms of the
C run-time ($(cvarsmt), $(conlibsmt), $(guilibsmt)); not the dll forms (no CRTDLL, below).
It builds debug versions; nmake NODEBUG=1 builds without debug information. On ARM, link
takes .res files: no cvtres.

The samples (samples\) build with nmake: hello, winhello (a window, a menu, a dialog) and
dll (a DLL and a program that calls it).


Drivers
-------

include\ddk has the headers of the DDK of Windows NT 3.5 (ntddk.h, miniport.h, video.h,
winddi.h, ndis.h, srb.h...), lib the libraries of kernel mode: ntoskrnl.lib, hal.lib,
videoprt.lib, ndis.lib, scsiport.lib and libcntpr.lib (the run-time of kernel mode and the
helpers of the compiler), and winsrv.lib, the graphics engine of NT 3.5. Compile with
include\ddk first and the defines the NT build gives a driver:

    cl -c -kernel -Zl -I%PINTSDK%\include\ddk -DNT_INST=0 -DCONDITION_HANDLING=1 -DWIN32=100
       -D_NT1X_=100 -DDBG=0 -DDEVL=1 -DFPO=1 drv.c
    link -driver -nodefaultlib -subsystem:native -base:0x10000 -entry:DriverEntry
       -out:drv.sys drv.obj ntoskrnl.lib hal.lib

A video miniport links with videoprt.lib. A display driver of NT 3.5 is a DLL that GDI loads
into CSRSS: compile it without -kernel, and link it with -dll -noentry -subsystem:console,
kernel32.lib, winsrv.lib and libc.lib. An image linked with -align:N below a page (the
linker for ARM refuses that) is laid out on N by the SDK's link.


Running on PINT
---------------

Copy the program to the SD card (its FAT partition), or onto a share, and run it. In QEMU,
mtools copies onto the disk image: mcopy -i nt-arcfw-sd.img@@1M hello.exe ::/


Limits
------

- Only the functions of Windows NT 3.5, as PINT has them. No MFC.
- C, and C++ without exceptions or run-time types (the run-time of NT 3.5 has neither: cl
  turns run-time types off, -GR-).
- Only the static run-times: no run-time DLL (-MD names msvcrt.lib, and NT 3.5 has no
  msvcrt.dll).
- No old-names library: the headers of NT 3.5 give the old names of the run-time (stricmp)
  as #defines of the new ones (_stricmp), and link takes no oldnames.lib.
- The run-time of NT 3.5 keeps its tables of initializers and terminators in a .CRT
  section of their own, as the linker of 1994 left them. The linker of today warns about
  that on every link (LNK4078, LNK4210), so the SDK's link turns those two warnings off.
- No win32.mak (of the SDKs after NT 3.5), no ARM assembler (armasm) headers, no MIDL, no
  message compiler, no help compiler.
