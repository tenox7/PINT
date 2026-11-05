#!/usr/bin/env bash
# The OLE types (PRIVATE\TYPES) as their makefiles build them for NT 3.5 (OPSYS NT1X, with
# TYPES\makefile.inc), with Microsoft C for ARM, from what pass 1 made of the interfaces
# (tools/midl/gen-midl.sh): pass 2 (OLEPRX32\DAYTONA\pass2.mk), uuid.lib, the GUIDs of the
# types (cguid_i.c and their _i.c), and pass 3 (pass3.mk), oleprx32.dll, the proxies and stubs
# of the proxy types (their _p.c, call_as.c, dlldata.c, transmit.cxx): no entry, no C
# run-time, the code in .orpc. The types are those of the types.mk of COMPOBJ, NEW_OLE and
# OLEAUTO. uuid.lib goes to obj/um as the SDK's (LIBCOPY public\sdk\lib), with the import
# library of oleprx32.
#
# Usage:  cd RASPI/build && ./make-types.sh uuid            # uuid.lib
#         INSTALL=1 ./make-types.sh oleprx32               # oleprx32.dll (after ole32.lib), into System32
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NT35="$(cd ../.. && pwd)"
PASS=${1:?usage: make-types.sh uuid|oleprx32}

docker run --rm -e CJ="${CJ:-4}" -e PASS="$PASS" -e INSTALL="${INSTALL:-0}" \
    -v "$NT35/msvc":/opt/msvc -v "$NT35":/work -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -u
. ./msvc.sh
O=/work/RASPI/obj
U=$O/um
T=/work/PRIVATE/TYPES
D=$T/OLEPRX32/DAYTONA
SUBSYS=TYPES bash mkfarm.sh /tmp/f
DIR=/tmp/types; rm -rf $DIR; mkdir -p $DIR/obj $DIR/src $U

# the types a list of the types.mk names (PROXYTYPES, TYPES)
types() { local m; for m in COMPOBJ:COM NEW_OLE:OLE OLEAUTO:OLEAUTO; do bash srcmacro.sh $T/${m%:*}/TYPES.MK ${m#*:}_$1; done | tr " " "\n" | sed -n "s/^\.\\\\\(.*\)\.type$/\1/p"; }
# a source: the version of the port, else that of the tree, else the one of pass 1
src() { local f; [ -f $(bash portdir.sh $D)/$1 ] && { echo $(bash portdir.sh $D)/$1; return; }; f=$(ls $D | grep -ix "$1") && { echo $D/$f; return; }; echo /tmp/f/gen/TYPES/OLEPRX32/DAYTONA/$1; }
# The C flags of TYPES\makefile.inc for NT1X: CFLAGS, the CDEBUGFLAGS of a free build (NTDEBUG
# retail: BUILDTYPE RELEASE), CPLATFORM with ARM, the C run-time DLL (USE_CRTDLL) and the
# warnings of cinc\warning.h; CINC: the directory, CINC, COMPOBJ and the SDK, pass 3 CAIROLE\IH first.
DEFS="-D_WINDOWS -DUNICODE -D_UNICODE -DDBG=0 -DDEVL=0 -DINC_OLE2 -DSTRICT -D_ARM_=1 -DFLAT -DWIN32=100 -D_NT1X_=100 -D_MT -D_DLL"
ntsrc /work/PRIVATE/CINC/WARNING.H $DIR/src/warning.h
CINC=".;../../../cinc;../../compobj"; [ $PASS = uuid ] || CINC="../../../cairole/ih;$CINC"
INCS="-I$K/inc -I/tmp/f/ovr $(bash incdirs.sh $D "$CINC") -I/tmp/f/oak -I/tmp/f/pub -I/tmp/f/csr -I/tmp/f/crt"
one() {
  local f=$1 b=$(basename "${1%.*}" | tr A-Z a-z) e=c
  case $f in *.[Cc][Xx][Xx]) e=cxx ;; esac
  ntsrc $f $DIR/src/$b.$e && ntcc $DIR/obj/$b.obj $DIR/src/$b.$e $DEFS -FI$DIR/src/warning.h $INCS
}
if [ $PASS = uuid ]; then
  srcs="$(src cguid_i.c) $(for t in $(types TYPES); do src ${t}_i.c; done)"
else
  srcs="$(for t in $(types PROXYTYPES); do src ${t}_p.c; done) $(src call_as.c) $(src dlldata.c) $(src transmit.cxx)"
fi
echo ">> compiling TYPES ($PASS)"
for f in $srcs; do par one $f; done
pwait
echo "   $PAROK compiled, $PARFAIL failed"
[ $PARFAIL -eq 0 ] || exit 1
if [ $PASS = uuid ]; then
  ntlib $DIR/uuid.lib $DIR/obj/*.obj || exit 1
  cp $DIR/uuid.lib $U/uuid.lib
  echo "   wrote ${U#/work/RASPI/}/uuid.lib"
  exit 0
fi

# The .def as the makefile.inc preprocesses it (CPLATFORM, CDEBUGFLAGS, CINC); the libraries of
# pass3.mk (LIBS: gdi32p, uuid, RPCLIBS, ole32) and OSLIBDLL; the base of COFFBASE oleprx32;
# LFLAGS /MERGE:.text=.orpc with those of the makefile.inc for a DLL (-NODEFAULTLIB -OPT:REF
# -RELEASE, SUBSYSTEM WINDOWS); NOENTRY; the VERSION_RC.
ntsrc $(src oleprx32.def) $DIR/oleprx32.src
cl -nologo -EP $DEFS $INCS -Tc $DIR/oleprx32.src 2>/dev/null | sed -f deffix.sed > $DIR/oleprx32.def
LIBS=""
for l in gdi32p uuid rpcrt4 ntdll ole32 gdi32 kernel32 user32 advapi32; do
  [ -f $U/$l.lib ] || { echo "   missing library $l (build it first)"; exit 1; }
  LIBS="$LIBS $U/$l.lib"
done
BASE=$(tr -d "\r" < /work/PUBLIC/SDK/LIB/COFFBASE.TXT | awk "tolower(\$1) == \"oleprx32\" {print \$2; exit}")
RES=$(bash rc.sh $D $D/OLEPRX32.RC $DEFS $INCS) || exit 1
link -nologo -machine:arm -release -dll -noentry -merge:.text=.orpc -opt:ref,noicf -base:$BASE -subsystem:windows -nodefaultlib \
     -debug -pdb:$DIR/oleprx32.pdb -pdbaltpath:"Z:$(echo $O/oleprx32.pdb | tr / "\\\\")" -map:$DIR/oleprx32.map \
     -def:$DIR/oleprx32.def -implib:$DIR/oleprx32.lib -out:$DIR/OLEPRX32.DLL $DIR/obj/*.obj $LIBS $RES > $DIR/link.err 2>&1 \
  || { echo ">> LINK FAILED"; grep -E "error|unresolved" $DIR/link.err | sort | uniq -c | sort -rn | head -40; exit 1; }
cp $DIR/oleprx32.pdb $DIR/oleprx32.map $O/
bash /work/RASPI/tools/host.sh pe2img -version 3.50 $DIR/OLEPRX32.DLL || exit 1
ntimplib $DIR/oleprx32.def $DIR/oleprx32.lib $U/oleprx32.lib
OUT=$O; [ "$INSTALL" = 1 ] && OUT=/work/RASPI/sdcard/root/WINNT/System32 && mkdir -p $OUT
cp $DIR/OLEPRX32.DLL $OUT/OLEPRX32.DLL
echo "   wrote $OUT/OLEPRX32.DLL"
'
