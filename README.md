# PINT - Raspberry PI Windows NT

Windows NT 4.0 (build 1381) ported to 32-bit ARM (ARMv7-A) on the Raspberry Pi 2, 3 and 4, with own ARC firmware.

PINT 4.0 is work in progress. PINT 3.5, Windows NT 3.5 (build 782), is on the branch `782`.

![PINT 4.0 in QEMU raspi2b: ntfetch, Task Manager, Control Panel, System Properties and Windows NT Diagnostics](screenshot.png)

## Status

- Runs on a real Pi 2, 3, 4 and Zero 2 W & QEMU.
- Supported: Wired Ethernet, PI display, USB mouse / keyboard, wired ethernet, SMP (multiprocessor)
- Not supported: sound/audio, printing, wireless, Pi 1, Pi 5

## Why not Pi 1 or Pi 5

Microsoft's compiler, the only 32-bit ARM compiler with NT's exception handling, makes only Thumb-2 code, not the ARMv6
code the Pi 1 and Zero need, and the Pi 5 cannot run a 32-bit kernel at all.

## Build

The NT 4.0 source (`private`, `public`) goes beside this tree, and the NT 3.5 tree (`nt35`: a few NT 3.5 files and
DEC's ADK in `arc/adk`, for the ARC DOS shell) and the Windows XP SP1 tree (`XPSP1`: the administrative tools, ping,
tracert, netstat, nbtstat) beside the NT 4.0 tree; none of them is part of this repository. The port's
versions of their files are edit scripts (`*.ntdiff`) the build applies to them (`tools/ntdiff.pl`).

Everything builds in Docker with Microsoft C for ARM ([msvc-arm32](https://hub.docker.com/r/tenox7/msvc-arm32)),
installed once into `msvc` of the NT 3.5 tree; sources and outputs stay on the host, Docker only runs the tools:

    docker run --rm -e ACCEPT_MSVC_LICENSE=yes -v "$PWD/../../nt35/msvc":/opt/msvc tenox7/msvc-arm32 msvc-setup
    build/build-all.sh      # everything from scratch -> obj/arcfw.{elf,bin}, obj/nt-arcfw-sd.img, obj/nt-setup-sd.img
    build/build-update.sh   # only what changed since the last build

`build-update.sh` runs only the steps whose files changed (`obj/stamp`) and copies what they made into the existing
system disk, keeping what NT wrote on it; the disk is made anew when the registry hives change. The steps and their
order are in `build/build-update.sh` and `build/build-nt.sh`. Families of steps (the keyboard layouts, the console
utilities...) run `J` at once (default 8, `J=1` in order), with `CJ` compiles per step (default
12). As NT's build lab did, the build rebases the DLLs and programs (`sdcard/rebase.sh`).

    build/portable-qemu-image.sh  # obj/pint40-qemu.tar.gz: the system disk, firmware and QEMU for macOS, Linux, Windows

Run it on a Mac with Homebrew `qemu` and `dylibbundler`; `IMG=disk.img` packs another disk, `FORCE=1` rebuilds the
bundled QEMU.

## Run

    build/run.sh                  # console on serial in this terminal (Ctrl-A X quits)
    build/run.sh -device usb-kbd  # console on the HDMI window + USB keyboard
    M=raspi3b build/run.sh        # a Pi 3B (M=raspi3ap: a 3A+, M=raspi4b: a Pi 4B, QEMU 11.1 or later)
    IMG=disk.img build/run.sh     # another system disk
    build/run.sh -h               # the common options; the others go to QEMU

The firmware puts the console on HDMI when a USB keyboard is present, otherwise on serial. run.sh always attaches a USB
mouse (click the window to give it to NT, Ctrl+Alt+G releases it) and a DS1307-type RTC at 0x68 on I2C1 (GPIO 2/3),
where the HAL keeps the time. Without an RTC the clock starts each boot at the firmware build date. The boot settings
(System > Startup/Shutdown) are kept on the SD card, as an ARC machine keeps them in NVRAM.

The first run is NT's developer setup (three boots). The first ends with the box "Windows NT will now restart" (OK, or
it closes after 30 s). In the second, OK the boxes (Detected Display, Display Properties) and press a key at the
console's pause. From the third boot on, NT logs on as Administrator by itself (password `password`). Hold Shift while
it logs on to get the Logon box; Ctrl+Alt+Del (QEMU monitor: Ctrl-A C, `sendkey ctrl-alt-delete`, Ctrl-A C) opens
Windows NT Security.

The resolution is `framebuffer_width` and `framebuffer_height` of the SD card's `config.txt` (1920x1080; without them the
display's mode), in QEMU `RES=1280x1024 build/run.sh`.

For a real Pi 2, 3 or 4, write `obj/nt-arcfw-sd.img` to an SD card. USB keyboards and mice, behind hubs too (and KVMs
with an absolute mouse, such as PiKVM), can be plugged in at any time; the boot menu lists the USB devices it found.

## Setup card

The build makes `obj/nt-setup-sd.img` (alone: `sdcard/make-setup-image.sh`).

NT 4.0's own setup, as from its CD on an ARC machine; it installs NT on the card itself. A fresh card has no boot
entry yet: in the firmware's menu choose "Run a program" and type `arm\setupldr`. Text setup (keep the FAT partition,
`\WINNT`), restart, GUI setup.
In QEMU: `IMG=copy-of-nt-setup-sd.img build/run.sh -device usb-kbd`. For the network pick the adapter from the list
(Raspberry Pi Ethernet Adapter); TCP/IP (with DHCP) and NetBEUI install.

## SDK

The PINT 4.0 SDK builds programs and DLLs for PINT outside this tree with Microsoft C for ARM (MSVC 14.44 of Visual
Studio 2022; Visual Studio 2026 has no ARM32 compiler). It has the NT 4.0 headers, PINT's libraries, the Win32 SDK's
win32.mak with ARM, and its own cl and link, which mark each image for NT 4.0 (the ARM linker marks it 6.02, which
NT 4.0 does not start). No MFC. `build/make-sdk.sh` makes `obj/pint40sdk.zip` for Windows (`setenv.cmd`, then nmake
or cl) and, in `obj/sdk/docker`, the Docker image tenox7/pint40sdk on tenox7/msvc-arm32
(`docker build -t tenox7/pint40sdk obj/sdk/docker`). `sdk/README.txt` has the details.

## Roadmap

- Wireless
- Sound/audio
- Pi 4: memory above the first GB, the USB-C port, USB Ethernet adapters.

## Legal

- This repository contains no Microsoft or Windows copyrighted material. 
- It is just a build toolchain plus own ARM ARC firmware emulator code. 
- Written from scratch by Dave Clanker based on U-Boot.
- Public domain.
