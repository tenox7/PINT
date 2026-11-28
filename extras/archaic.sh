# Archaic, a web browser through WRP (the Web Rendering Proxy, https://github.com/tenox7/wrp),
# https://github.com/tenox7/archaic: its build for Windows NT on ARM, from the newest release that has it (built there
# with the PINT 3.5 SDK, for PINT 3.5 and 4.0), into \APPS\ARCHAIC, with its LICENSE. In Programs > Apps and on the
# desktop (syssetup.inf).
REPO=tenox7/archaic
GET="archaic.exe=archaic-ntarm.exe LICENSE=/LICENSE"
BUILD='perl -pi -e "s/\r*\$/\r/" LICENSE'
DEST=APPS/ARCHAIC
FILES="archaic.exe LICENSE"
