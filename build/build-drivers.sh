#!/usr/bin/env bash
# Build the kernel-mode drivers into sdcard/root/WINNT/System32/Drivers: storage (with the
# crash dump driver, diskdump),
# file systems, display, keyboard and mouse, the pipe, mailslot, null, beep and file
# system recognizer drivers, and the network. Needs the kernel (make-execlink.sh) and
# the HAL.
# Each step runs when its inputs changed (build/stamp.sh). A driver and the ones that link
# it are a job (stamp.sh), J at once.
#
# Usage:  build/build-drivers.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/stamp.sh"
N=/work/PRIVATE/NTOS D=/work/RASPI/drivers
drv() { INSTALL=1 cstep drv $N $1 link -- $B/make-driver.sh $1; }
bcm() { INSTALL=1 ROOT=$D cstep drv $D $1 link -- $B/make-driver.sh $1; }

lib() { cstep lib $N $1 -- $B/make-lib.sh $1; }
storage() { drv DD/SCSIPORT; lib DD/CLASS; drv DD/SCSIDISK; bcm bcmemmc; drv DD/NTFT; drv DD/DISKDUMP; }
video() { drv VIDEO/PORT; bcm bcmvideo; }
input() { drv DD/KBDCLASS; drv DD/MOUCLASS; bcm bcmusb; }
job storage storage
for d in FASTFAT FSREC NPFS MAILSLOT DD/NULL DD/BEEP; do job drv/$d drv $d; done
job video video; job input input
# the sound driver, on NT's sound library
sound() { lib DD/SOUND/SOUNDLIB; bcm bcmsound; }
job sound sound
# networking: the NDIS wrapper, the netcard detection driver of setup, the Ethernet miniport of
# the board (a function of bcmusb), the TDI wrapper and TCP/IP with NetBIOS over TCP/IP, the
# NetBIOS interface, Windows Sockets (afd), and the redirector, server, browser and multiple UNC
# provider of LAN Manager (bcmeth links bcmusb and the NDIS wrapper)
network() {
    drv NDIS/WRAPPER; drv NDIS/DETECT/DRIVER
    drv TDI/WRAPPER; lib TDI/TCPIP/IP; drv TDI/TCPIP/TCP; lib NBT/NBT; drv NBT/NT; drv NETBIOS; drv AFD
    lib SMBTRSUP; lib BOWSER; drv MUP/DAYTONA; drv RDR/DAYTONA; drv SRV/DAYTONA
}
job network network; barrier
# the miniport of the on-board wireless chip links the NDIS wrapper too
job bcmwlan bcm bcmwlan
bcm bcmeth
barrier
