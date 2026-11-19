# OpenTTD, https://github.com/tenox7/openttdNT: its build for PINT 4.0 and the data (release.zip, the folder release
# with the builds for other CPUs, left out), from the newest release that has both (the exe built there with the PINT
# 4.0 SDK), into \GAMES\OPENTTD. In Programs > Games (syssetup.inf).
REPO=tenox7/openttdNT
GET="ttd.exe=ttd-pint40-arm.exe release.zip"
DEST=GAMES/OPENTTD
FILES="ttd.exe release/*.txt release/*.lng release/*.grf release/*.cat release/*.dat release/*.cfg release/*.sav
    release/MIDI"
