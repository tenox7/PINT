# ntrdps, an RDP server for the console of Windows NT, https://github.com/tenox7/ntrdps: built in the
# environment of the NT build (its drivers need the headers of kernel mode) with its own makefiles
# and extras/NTRDPS.mak, into \NTRDPS as its installer, ntrdpset.exe, with the files of NT 4.0 in it
# (setup.rc without the layered driver of NT 3.5). It is not installed: the user runs the installer.
ENV=nt
COPY="src LICENSE README.md"
BUILD='cp $EXTRAS/NTRDPS.mak src/arm.mak && cp $EXTRAS/NTRDPS.mak src/drivers/arm.mak &&
    (cd src && nmake -nologo -f arm.mak) &&
    (cd src/drivers && INCLUDE=$KMINC nmake -nologo -f arm.mak DRIVERS=1) &&
    for f in src/ntrdps.exe src/ntrdpshk.dll src/drivers/ntrdpsmr.dll src/drivers/ntrdpsm0.dll src/drivers/ntrdpsmr.sys; do ntver $f || exit 1; done &&
    for f in src/drivers/ntrdpsmr.dll src/drivers/ntrdpsm0.dll src/drivers/ntrdpsmr.sys; do pe2img -align 0x20 $f || exit 1; done &&
    mkdir -p src/setup/nt35 src/setup/nt40 && cp src/ntrdps.exe src/ntrdpshk.dll src/setup/nt35/ &&
    cp src/drivers/ntrdpsmr.dll src/drivers/ntrdpsm0.dll src/drivers/ntrdpsmr.sys src/setup/nt40/ &&
    sed "/NTRDPSFILE.*ntrdpsly/d" src/setup/setup.rc > src/setup/arm.rc &&
    (cd src && nmake -nologo -f arm.mak ntrdpset.exe) && ntver src/ntrdpset.exe &&
    cp src/tls/bearssl/LICENSE.txt bearssl.txt && sed -i "s/\r*$/\r/" LICENSE README.md bearssl.txt'
DEST=NTRDPS
FILES="src/ntrdpset.exe LICENSE README.md bearssl.txt"
