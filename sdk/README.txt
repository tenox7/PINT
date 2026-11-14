PINT 4.0 SDK
============

Builds programs and DLLs for PINT 4.0, Windows NT 4.0 on ARM (Raspberry Pi 2, 3, 4, Zero 2 W),
with Microsoft C for ARM (MSVC 14.44, the last with an ARM32 target): on Windows with Visual
Studio 2022, or in Docker (tenox7/pint40sdk).

It has the headers of Windows NT 4.0 (Win32, the C run-time, OpenGL, POSIX) as PINT builds
with them, ahead of the compiler's own (intrin.h, stdint.h, sal.h...), PINT's libraries (the
import libraries of its DLLs and its C run-times), the
win32.mak of the Win32 SDK with ARM in it, and its own cl and link: they run those of
Microsoft C for ARM and mark each image for Windows NT 4.0 (the linker for ARM marks it 6.02,
which NT 4.0 does not start). No MFC.


Windows
-------

1. Visual Studio 2022 or its Build Tools, with the components
   "MSVC v143 - VS 2022 C++ ARM build tools (Latest)" and a Windows SDK (for rc).
   Visual Studio 2026 has no ARM32 compiler.
2. Unzip this SDK, then in a command prompt:

       C:\pint40sdk\setenv.cmd
       cd C:\pint40sdk\samples\hello
       cl hello.c


Docker
------

Once, install Microsoft C for ARM into a volume (it downloads from Microsoft;
ACCEPT_MSVC_LICENSE=yes accepts the Visual Studio Build Tools license,
https://go.microsoft.com/fwlink/?LinkId=2179911):

    docker run --rm -e ACCEPT_MSVC_LICENSE=yes -v msvc-arm32:/opt/msvc tenox7/pint40sdk msvc-setup

Then, in the directory of the sources:

    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint40sdk cl hello.c
    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint40sdk nmake

The commands are cl, link, nmake and rc of the SDK, and lib, dumpbin, editbin of Microsoft C.
The sources are in /src (Z:\src for the tools). The SDK is /opt/pint40sdk.


Building
--------

    cl hello.c                         a console program, hello.exe
    cl app.c app.res                   a Windows program (WinMain), with resources (rc app.rc)
    cl -LD lib.c                       a DLL, lib.dll and lib.lib
    cl -MD hello.c                     with the C run-time DLL, msvcrt.dll (default: static)
    nmake                              a makefile of the Win32 SDK: !include <win32.mak>

cl links the libraries a Visual C++ project links: kernel32, user32, gdi32, winspool,
comdlg32, advapi32, shell32, ole32, oleaut32, uuid. Name the others (ws2_32.lib, comctl32.lib,
opengl32.lib...), as with any Microsoft C.

win32.mak is that of the Win32 SDK for Windows NT 4.0 with CPU=ARM: $(cc) $(cflags) $(cvars),
$(link) $(conflags) ... $(conlibs), $(guiflags), $(guilibs), the mt and dll forms of the C
run-time, $(dlllflags), $(olelibs), $(psxlflags) and $(psxlibs) for POSIX programs. It builds
debug versions; nmake NODEBUG=1 builds without debug information.

The samples (samples\) build with nmake: hello, winhello (a window, a menu, a dialog), dll
(a DLL and a program that calls it) and cpp (C++).


Running on PINT
---------------

Copy the program to the SD card (its FAT partition), or onto a share, and run it. In QEMU,
mtools copies onto the disk image: mcopy -i nt-arcfw-sd.img@@1M hello.exe ::/


Limits
------

- Only the functions of Windows NT 4.0, as PINT has them. No MFC.
- C and C++ (exceptions with -EHsc or -EHa, run-time types). With -MD, a program that
  throws needs PINT 4.0 newer than 2026-11-02 (its msvcrt.dll); -MT (the default) links the
  run-time into the program, which then runs on that release too. The C++ library is that
  of Visual C++ 4.2's run-time: <iostream.h> with -MD and msvcirt.lib (msvcrt40.dll). No
  STL (<string>, <vector>...), no <iostream>.
- The libraries of the C run-time: libcmt (static, the default), msvcrt (-MD), libc (static,
  single-threaded: win32.mak's $(cvars) and $(conlibs)).
- No ARM assembler (armasm) headers, no MIDL, no message compiler, no help compiler.
