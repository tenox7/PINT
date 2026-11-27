# ScummVM, which plays classic adventure and role-playing games from their original data files, an unofficial port of
# ScummVM 2.9.1 to Windows NT 4.0 on ARM, https://github.com/danifunker/nt4ports (folder scummvm-2.9): the zip for ARM
# of the newest release that has one (built there with the PINT 4.0 SDK), a folder with ScummVM, its scummvm.ini (which
# keeps its settings, saved games and log beside it), scummvm-nosound.bat and the licences, into \GAMES\SCUMMVM (no
# games). The pattern takes ScummVM 2 only: the same release has 0.6.1 for ARM too. In Programs > Games
# (syssetup.inf), started in its folder.
REPO=danifunker/nt4ports
GET="scummvm-2.*-nt4-arm.zip"
DEST=GAMES/SCUMMVM
FILES="scummvm-2.*-nt4-arm/*"
