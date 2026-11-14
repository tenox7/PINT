# pint40sdk

The PINT 4.0 SDK. It builds programs and DLLs for [PINT 4.0](https://github.com/tenox7/PINT/tree/1381), Windows NT
4.0 on ARM (Raspberry Pi 2, 3, 4 and Zero 2 W), with Microsoft C for ARM (MSVC 14.44, the last MSVC with an ARM32
target) under Wine. It runs natively on arm64 (Apple Silicon) and on amd64.

It has:
- the headers of Windows NT 4.0 (Win32, the C run-time, OpenGL, POSIX), as PINT builds with them, ahead of the
  compiler's own (`intrin.h`, `stdint.h`, `sal.h`...)
- PINT's libraries: the import libraries of its DLLs and its C run-times
- the win32.mak of the Win32 SDK, with ARM in it
- its own cl and link: they run those of Microsoft C for ARM and mark each image for Windows NT 4.0 (the linker for
  ARM marks it 6.02, which NT 4.0 does not start)
- samples in /opt/pint40sdk/samples: hello, winhello (a window, a menu, a dialog), dll, cpp

No MFC. The image contains no Microsoft software: `msvc-setup` downloads Microsoft C for ARM from Microsoft into a
volume, as in [tenox7/msvc-arm32](https://hub.docker.com/r/tenox7/msvc-arm32), which this image is built on.

## Install

Once:

    docker run --rm -e ACCEPT_MSVC_LICENSE=yes -v msvc-arm32:/opt/msvc tenox7/pint40sdk msvc-setup

`ACCEPT_MSVC_LICENSE=yes` accepts the [Visual Studio Build Tools license](https://go.microsoft.com/fwlink/?LinkId=2179911).
A volume set up with tenox7/msvc-arm32 works too.

## Use

In the directory of the sources:

    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint40sdk cl hello.c
    docker run --rm -v msvc-arm32:/opt/msvc -v "$PWD":/src tenox7/pint40sdk nmake

| Command | Builds |
|---|---|
| `cl hello.c` | a console program, hello.exe |
| `cl app.c app.res` | a Windows program (WinMain) with resources (`rc app.rc`) |
| `cl -LD lib.c` | a DLL, lib.dll and lib.lib |
| `cl -MD hello.c` | with the C run-time DLL, msvcrt.dll (default: static) |
| `nmake` | a makefile of the Win32 SDK: `!include <win32.mak>` |

The commands are `cl link nmake rc` of the SDK and `lib dumpbin editbin` of Microsoft C. The sources are in /src
(Z:\src for the tools). `docker run --rm tenox7/pint40sdk` prints the SDK's README.

Copy the program onto PINT's SD card (its FAT partition) or onto a share, and run it.

## Limits

- Only the functions of Windows NT 4.0, as PINT has them. No MFC.
- C and C++ (exceptions with -EHsc or -EHa, run-time types). With -MD, a program that throws needs PINT 4.0 of
  2026-11-04 or later. The C++ library is that of Visual C++ 4.2 (`<iostream.h>`); no STL.
- No ARM assembler headers, no MIDL, no message compiler, no help compiler.
