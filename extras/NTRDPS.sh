# ntrdps, an RDP server for the console of Windows NT, https://github.com/tenox7/ntrdps: built in the
# environment of the NT build (its drivers need the headers of kernel mode) with its own makefiles
# and extras/NTRDPS.mak, into \NTRDPS. The first run (winlogon.cmd) installs it as its installer
# does: the service (automatic, LocalSystem) and the mirror driver, both running from the restart
# that ends it. Clients log on with an account of the Administrators (Administrator, password).
ENV=nt
COPY="src LICENSE README.md"
BUILD='cp $EXTRAS/NTRDPS.mak src/arm.mak && cp $EXTRAS/NTRDPS.mak src/drivers/arm.mak &&
    (cd src && nmake -nologo -f arm.mak) &&
    (cd src/drivers && INCLUDE=$KMINC nmake -nologo -f arm.mak DRIVERS=1 ntrdpsm0.dll ntrdpsmr.sys) &&
    for f in src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsm0.dll src/drivers/ntrdpsmr.sys; do ntver $f || exit 1; done &&
    for f in src/drivers/ntrdpsm0.dll src/drivers/ntrdpsmr.sys; do pe2img -align 0x20 $f || exit 1; done &&
    cp src/tls/bearssl/LICENSE.txt bearssl.txt && sed -i "s/\r*$/\r/" LICENSE README.md bearssl.txt'
DEST=NTRDPS
FILES="src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsm0.dll src/drivers/ntrdpsmr.sys LICENSE README.md bearssl.txt"
FIRSTRUN='C:\NTRDPS\ntrdps -install
C:\NTRDPS\ntrdps -installmirror'
