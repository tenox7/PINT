# WiNTown, a port of Micropolis, https://github.com/tenox7/wintown: its build for PINT 4.0, from the newest release that
# has it (built there with the PINT 4.0 SDK), into \GAMES\WINTOWN, with the LICENSE of that release (GPL 3.0 with
# Electronic Arts' additional terms). In Programs > Games (syssetup.inf).
REPO=tenox7/wintown
GET="wintown.exe=wintown-arm-pint40.exe LICENSE=/LICENSE"
BUILD='perl -pi -e "s/\r*\$/\r/" LICENSE'
DEST=GAMES/WINTOWN
FILES="wintown.exe LICENSE"
