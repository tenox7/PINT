# WiNTown, a port of Micropolis, https://github.com/tenox7/wintown: its build for PINT 3.5, from the newest release that
# has it (built there with the PINT 3.5 SDK), into \GAMES\WINTOWN, with the LICENSE of that release (GPL 3.0 with
# Electronic Arts' additional terms). In the Games group (sdcard/make-sd-image.sh).
REPO=tenox7/wintown
GET="wintown.exe=wintown-arm-pint35.exe LICENSE=/LICENSE"
BUILD='perl -pi -e "s/\r*\$/\r/" LICENSE'
DEST=GAMES/WINTOWN
FILES="wintown.exe LICENSE"
