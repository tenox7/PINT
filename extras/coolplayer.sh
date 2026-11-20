# CoolPlayer, an MP3 and Ogg Vorbis player, https://github.com/tenox7/coolplayer: its build for PINT 4.0, from the
# newest release that has it (built there with the PINT 4.0 SDK), into \APPS\COOLPLAYER, with the licenses of that
# release: CoolPlayer's (GPL 2.0) and those of its Ogg and Vorbis decoders. In Programs > Apps and on the desktop
# (syssetup.inf).
REPO=tenox7/coolplayer
GET="coolplayer.exe=coolplayer-arm-pint40.exe COPYING=/COPYING COPYING.ogg=/ogg/COPYING COPYING.vorbis=/vorbis/COPYING"
BUILD='perl -pi -e "s/\r*\$/\r/" COPYING COPYING.ogg COPYING.vorbis'
DEST=APPS/COOLPLAYER
FILES="coolplayer.exe COPYING COPYING.ogg COPYING.vorbis"
