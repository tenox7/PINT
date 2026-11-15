# ntrdps, an RDP server for the console of Windows NT, https://github.com/tenox7/ntrdps: built in the
# environment of the NT build with its own makefiles and extras/NTRDPS.mak, into \NTRDPS as its
# installer, ntrdpset.exe, with the files of NT 3.5 in it (setup.rc without those of NT 4.0). It is
# not installed: the user runs the installer, which also asks for the account clients log on with.
COPY="src LICENSE README.md"
BUILD='cp $EXTRAS/NTRDPS.mak src/arm.mak && cp $EXTRAS/NTRDPS.mak src/drivers/arm.mak &&
    (cd src && nmake -nologo -f arm.mak) &&
    (cd src/drivers && INCLUDE=$DDKINC nmake -nologo -f arm.mak DRIVERS=1 ntrdpsly.dll) &&
    for f in src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsly.dll; do pe2img -version 3.50 $f || exit 1; done &&
    mkdir -p src/setup/nt35 && cp src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsly.dll src/setup/nt35/ &&
    sed "/NTRDPSFILE.*nt40/d" src/setup/setup.rc > src/setup/arm.rc &&
    (cd src && nmake -nologo -f arm.mak ntrdpset.exe) && pe2img -version 3.50 src/ntrdpset.exe &&
    cp src/tls/bearssl/LICENSE.txt bearssl.txt && sed -i "s/\r*$/\r/" LICENSE README.md bearssl.txt'
DEST=NTRDPS
FILES="src/ntrdpset.exe LICENSE README.md bearssl.txt"
