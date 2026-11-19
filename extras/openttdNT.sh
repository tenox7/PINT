# OpenTTD, https://github.com/tenox7/openttdNT: its build for PINT 3.5 and the data (release.zip, the folder release
# with the builds for other CPUs, left out), from the newest release that has both (the exe built there with the PINT
# 3.5 SDK), into \GAMES\OPENTTD. In the Games group (sdcard/make-sd-image.sh). NT 3.5 changes no display modes: the
# game runs in a window.
REPO=tenox7/openttdNT
GET="ttd.exe=ttd-pint35-arm.exe release.zip"
DEST=GAMES/OPENTTD
FILES="ttd.exe release/*.txt release/*.lng release/*.grf release/*.cat release/*.dat release/*.cfg release/*.sav
    release/MIDI"
