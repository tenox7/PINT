# ntrdps, an RDP server for the console of Windows NT, https://github.com/tenox7/ntrdps: built in the
# environment of the NT build with its own makefiles and extras/NTRDPS.mak, into \NTRDPS. The first
# run (winlogon.cmd) installs it as its installer does: the service (automatic, LocalSystem), the
# layered display driver (System32\layrdisp.dll, which GDI loads at boot), both running from the
# restart that ends it, and the account clients log on with (NT 3.5 has no account check it can
# use): Administrator, password; "C:\NTRDPS\ntrdps -setlogin" changes it. Its two lines go to
# -setlogin from a file: cmd runs a block of commands piped, "(echo A& echo B)| ...", in a cmd
# of its own, rebuilt with spaces around the &, and the user name would end with one.
COPY="src LICENSE README.md"
BUILD='cp $EXTRAS/NTRDPS.mak src/arm.mak && cp $EXTRAS/NTRDPS.mak src/drivers/arm.mak &&
    (cd src && nmake -nologo -f arm.mak) &&
    (cd src/drivers && INCLUDE=$DDKINC nmake -nologo -f arm.mak DRIVERS=1 ntrdpsly.dll) &&
    for f in src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsly.dll; do pe2img -version 3.50 $f || exit 1; done &&
    cp src/tls/bearssl/LICENSE.txt bearssl.txt && sed -i "s/\r*$/\r/" LICENSE README.md bearssl.txt'
DEST=NTRDPS
FILES="src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsly.dll LICENSE README.md bearssl.txt"
FIRSTRUN='C:\NTRDPS\ntrdps -install
C:\NTRDPS\ntrdps -installlayer
echo Administrator> C:\NTRDPS\login.txt
echo password>> C:\NTRDPS\login.txt
C:\NTRDPS\ntrdps -setlogin < C:\NTRDPS\login.txt
erase C:\NTRDPS\login.txt'
