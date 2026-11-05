Windows NT 3.5 for the Raspberry Pi 2 (PINT) on QEMU's raspi2b

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and GTK 3) or run.cmd (Windows 10+ x64).
NT is in the window: click it to give NT the mouse, Ctrl+Alt+G releases it.
QEMU monitor: Ctrl+Alt+2 in the window (or the View menu); Ctrl+Alt+1 goes back to NT. The terminal has the serial
console and the monitor: Ctrl-A C switches between them, Ctrl-A X quits. Monitor commands:
  sendkey ctrl-alt-delete    Ctrl+Alt+Del for NT
  info block                 the disks: sd0 is the SD card, NT's only disk
  change sd0 other.img raw   another SD card image: shut NT down first, then system_reset
A new disk first runs NT's developer setup (three boots, on its own). Then NT logs on as Administrator by itself
(password: password; hold Shift while it logs on to get the Logon box). Extra QEMU options are passed through,
e.g. -display vnc=:1.
Networking: a USB Ethernet adapter on QEMU user mode networking (NAT, DHCP).
Resolution: RES=1280x1024 ./run.sh (run.cmd: set RES=1280x1024 first), default 1920x1080.

QEMU @V@ source: https://download.qemu.org/qemu-@V@.tar.xz (GPLv2, see bin/COPYING).
