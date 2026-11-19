# TenoxVNC, a VNC viewer, https://github.com/tenox7/tenoxvnc: its build for PINT 4.0, from the newest release that has
# it (built there with the PINT 4.0 SDK), into \APPS\TENOXVNC, with the licenses of that release: TenoxVNC's (GPL 2.0)
# and those of its zlib and IJG JPEG decoders. In Programs > Apps and on the desktop (syssetup.inf).
REPO=tenox7/tenoxvnc
GET="tenoxvnc.exe=tenoxvnc-pint40-arm.exe LICENCE.TXT=/LICENCE.TXT LICENSE.zlib=/LICENSE.zlib README.jpeg=/README.jpeg"
BUILD='perl -pi -e "s/\r*\$/\r/" LICENCE.TXT LICENSE.zlib README.jpeg'
DEST=APPS/TENOXVNC
FILES="tenoxvnc.exe LICENCE.TXT LICENSE.zlib README.jpeg"
