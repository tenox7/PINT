#!/usr/bin/env bash
# Build stdole32.tlb, the type library of the standard types of OLE Automation (GUID,
# DISPPARAMS, EXCEPINFO, IUnknown, IDispatch, IEnumVARIANT), which NT 3.5 installs in System32
# (bom.txt: STDOLE32.TLB, "OLE TWO") but whose source its tree lacks: it comes from NT 4.0
# (../nt40) as oleauto\build\mktyplib.mak makes it for WIN32. mktyplib (private\oleauto\src\
# mktyplib, copied into obj/mktyplib as it is, with the port's SOURCES: apps/mktyplib) is
# built as a program and run as NT's build ran it on each CPU: on the ARM Wine of the build
# image, with our ole32.dll and oleaut32.dll (NT 3.5's: the library is in their format), over
# the C preprocessor's stdole.odl; then stdole32.rc (the library as resource 1 TYPELIB, and the
# version) is linked into a DLL of resources alone: no entry, no default libraries, heap and
# stack 0. NT 3.5's LoadTypeLib reads a library from the resources of an image
# (typelib\gtlibole.cxx: GetOffsetOfTypeLibResource).
#
# Usage:  cd RASPI/build && ./make-stdole.sh       # obj/STDOLE32.TLB
#         INSTALL=1 ./make-stdole.sh              # also into System32
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NT35="$(cd ../.. && pwd)"
M=$NT35/../nt40/private/oleauto/src
D=$NT35/RASPI/obj/mktyplib
[ -d $M/mktyplib ] || { echo "   no NT 4.0 tree ($M)"; exit 1; }
mkdir -p $D
for f in lexer.c intlstr.c mktyplib.c parser.c hout.c typout.cpp dimalloc.cxx mktyplib.rc mktyplib.ico \
         errors.h fileinfo.h intlstr.h mktyplib.h parser.h tokens.h typelib.err stdole.odl stdole32.rc; do
    cp -f $M/mktyplib/$f $D/ || exit 1
done
cp -f $M/inc/verstamp.h ../apps/mktyplib/SOURCES $D/ || exit 1
INSTALL=0 ROOT=/work/RASPI/obj ./make-exe.sh mktyplib mktyplib || exit 1

docker run --rm -e INSTALL="${INSTALL:-0}" -v "$NT35/msvc":/opt/msvc -v "$NT35":/work -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -u
. ./msvc.sh
O=/work/RASPI/obj
S=/work/RASPI/sdcard/root/WINNT/System32
M=$O/mktyplib
SUBSYS=mktyplib bash mkfarm.sh /tmp/f
W=/tmp/stdole; rm -rf /tmp/stdole; mkdir -p $W
for f in OLE32.DLL OLEAUT32.DLL; do [ -f $S/$f ] || { echo "   no $f in System32 (build it first)"; exit 1; }; cp $S/$f $W/; done
cp $O/MKTYPLIB.EXE $W/ && cp $M/stdole.odl $W/stdole32.odl
cd $W
cl -nologo -EP stdole32.odl > stdole32.pre 2> cpp.err || { cat cpp.err; exit 1; }
# OleInitialize makes a window for DDE (ole232\base\ole2.cpp), and Wine makes windows only with
# a display driver: here the null one, set in the prefix of this container before Wine starts
# (with none, mktyplib stops with "out of memory"). A time limit: a hang fails the build.
{ echo; echo "[Software\\\\Wine\\\\Drivers]"; echo "\"Graphics\"=\"null\""; } >> /opt/wine32/user.reg
(export WINEPREFIX=/opt/wine32; /usr/lib/wine/wineserver32 -p 2>/dev/null
 WINEDLLOVERRIDES="ole32,oleaut32=n,b" timeout 300 /usr/lib/wine/wine MKTYPLIB.EXE /h /o stdole32.log /tlb stdole32.tmp /nocpp stdole32.pre) > mktyplib.out 2>&1
[ -s stdole32.tmp ] || { echo ">> MKTYPLIB FAILED"; tr -d "\r" < mktyplib.out; exit 1; }
tr -d "\r" < stdole32.log
res=$(bash $B/rc.sh $M $M/stdole32.rc -DWIN32 -I$W -I/tmp/f/pub -I/tmp/f/crt) || exit 1
link -nologo -machine:arm -dll -noentry -nodefaultlib -debug:none -heap:0,0 -stack:0,0 -map:$W/stdole32.map \
     -out:$W/STDOLE32.TLB $res > link.err 2>&1 || { echo ">> LINK FAILED"; tr -d "\r" < link.err; exit 1; }
bash /work/RASPI/tools/host.sh pe2img -version 3.50 $W/STDOLE32.TLB || exit 1
cp $W/STDOLE32.TLB $W/stdole32.map $O/
OUT=$O; [ "$INSTALL" = 1 ] && OUT=$S && cp $W/STDOLE32.TLB $S/
echo "   wrote $OUT/STDOLE32.TLB"
'
