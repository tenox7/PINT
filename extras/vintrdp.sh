# vintrdp, a Remote Desktop client for NT 4.0 Terminal Server, Windows 2000 and Server 2003 (and, over TLS and NLA,
# Windows 10 and 11) that also connects to VNC servers, https://github.com/danifunker/vintrdp: its build for NT 4.0 on
# ARM, from the newest release that has it (built there with ruststdwinnt4 and the PINT 4.0 SDK), into \APPS\VINTRDP,
# with its LICENSE (GPL 3.0). In Programs > Apps (syssetup.inf), started in its folder.
REPO=danifunker/vintrdp
GET="vintrdp.exe=vintrdp-nt4-arm-*.exe LICENSE=/LICENSE"
BUILD='perl -pi -e "s/\r*\$/\r/" LICENSE'
DEST=APPS/VINTRDP
FILES="vintrdp.exe LICENSE"
