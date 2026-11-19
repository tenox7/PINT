# pint35sdk

The PINT 3.5 SDK. It builds programs, DLLs and drivers for [PINT 3.5](https://github.com/tenox7/PINT/tree/782),
Windows NT 3.5 on ARM (Raspberry Pi 2, 3, 4 and Zero 2 W), with Microsoft C for ARM (MSVC 14.44, the last MSVC with
an ARM32 target) under Wine. It runs natively on arm64 (Apple Silicon) and on amd64.

It has:
- the headers of Windows NT 3.5 (Win32, the C run-time, OpenGL), as PINT builds with them, ahead of the compiler's
  own (`intrin.h`, `stdint.h`, `sal.h`...)
- PINT's libraries: the import libraries of its DLLs and its static C run-times
- the ntwin32.mak of the Win32 SDK for Windows NT 3.5, with ARM in it: `nmake` builds a makefile that has
  `!include <ntwin32.mak>`
- a driver kit: the headers of NT 3.5's DDK (`include/ddk`) and the libraries of kernel mode; the SDK's README has
  how to build a driver
- its own cl and link: they run those of Microsoft C for ARM and mark each image for Windows NT 3.5 (the linker for
  ARM marks it 6.02, which NT 3.5 does not start)
- samples in /opt/pint35sdk/samples: hello, winhello (a window, a menu, a dialog), dll

No MFC. The image contains no Microsoft software: `msvc-setup` downloads Microsoft C for ARM from
Microsoft into a volume, as in [tenox7/msvc-arm32](https://hub.docker.com/r/tenox7/msvc-arm32), which this image is
built on.

## Install

Once:

    docker run --rm -e ACCEPT_MSVC_LICENSE=yes -v msvc-arm32:/opt/msvc tenox7/pint35sdk msvc-setup

`ACCEPT_MSVC_LICENSE=yes` accepts the [Visual Studio Build Tools license](https://go.microsoft.com/fwlink/?LinkId=2179911).
A volume set up with tenox7/msvc-arm32 works too.

## Use

In the directory of the sources:

    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint35sdk cl hello.c
    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint35sdk nmake

| Command | Builds |
|---|---|
| `cl hello.c` | a console program, hello.exe |
| `cl app.c app.res -link -subsystem:windows` | a Windows program (WinMain) with resources (`rc -r app.rc`) |
| `cl -LD lib.c` | a DLL, lib.dll and lib.lib |

The commands are `cl link nmake rc` of the SDK and `lib dumpbin editbin` of Microsoft C. The sources are in /src
(Z:\src for the tools). `docker run --rm tenox7/pint35sdk` prints the SDK's README.

Copy the program onto PINT's SD card (its FAT partition) or onto a share, and run it.

## Limits

- Only the functions of Windows NT 3.5, as PINT has them. No MFC.
- C, and C++ without exceptions or run-time types. Static C run-times only (no msvcrt.dll on NT 3.5).
- No win32.mak (of the SDKs after NT 3.5), no ARM assembler headers, no MIDL, no message compiler, no help compiler.
