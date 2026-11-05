#!/usr/bin/env bash
# OLE Automation (CAIROLE\OLEAUTO), oleaut32.dll, as its build\win32mak.mak builds it for a RISC
# machine (make.bat r2mipsnd: TARGAPI WIN32, DEBUG R, LOCALBUILD FALSE), with Microsoft C for ARM
# and the object lists of the makefile:
# - ole2di32.lib, OLEDISP_OBJS of src\oledisp with DISPCLFLAGS (those of RISC, _ARM_ for _MIPS_)
#   and DISPINCFLAGS: the oledisp.cpp of WIN32, the oleconva.cpp of RISC (one file for MIPS, Alpha
#   and PowerPC) and the invoke of the CPU, that of ARM the port's (invoke.asm);
# - oleaut32.dll, the typelib of src\typelib (MISCLIB_OLEOBJ, CLOBJ_OLE, tlibpch) with OLECLFLAGS,
#   OLEINCS and the precompiled header of tlibpch.cxx, the .def of the C preprocessor and
#   bin\win32def.awk, oleaut32.rc, linked as the makefile does with OLE2ANSI: the entry of the
#   multithreaded C run-time (_DllMainCRTStartup, libcmt.lib), no base, the libraries of
#   TYPELIBDLL_LIBS but offansi.lib (of the VBA project, not in the tree: its ANSI forms of the
#   OLE calls go unused with FV_UNICODE_OLE), then GDI, USER, the C run-time, KERNEL, ADVAPI, MPR.
# The version.hxx of a retail RISC build is that of R2MIPSND (the Win32 one with OE_RISC).
#
# Usage:  cd RASPI/build && ./make-oleaut.sh          # obj/OLEAUT32.DLL
#         INSTALL=1 ./make-oleaut.sh                  # into System32
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NT35="$(cd ../.. && pwd)"

docker run --rm -e CJ="${CJ:-4}" -e INSTALL="${INSTALL:-0}" \
    -v "$NT35/msvc":/opt/msvc -v "$NT35":/work -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -u
. ./msvc.sh
O=/work/RASPI/obj
U=$O/um
A=/work/PRIVATE/CAIROLE/OLEAUTO
S=$A/SRC/OLEDISP
T=$A/SRC/TYPELIB
SUBSYS=CAIROLE bash mkfarm.sh /tmp/f
DIR=/tmp/oleaut; rm -rf $DIR; mkdir -p $DIR/d $DIR/t $DIR/pch $U

# the objects of a list of win32mak.mak, in a retail build
objs() { SRCMACROS="DEBUG=R" bash srcmacro.sh $A/BUILD/WIN32MAK.MAK $1 | tr " " "\n" | tr "\\\\" / | sed -n "s,.*/\([^/]*\)\.obj$,\1,p"; }
# a file of a directory of the tree, by its name in any case
tf() { local f; f=$(ls $1 | grep -ix "$2") && echo $1/$f; }
# the source of an object of a directory: the version of the port, else that of the tree
src() { local p=$(bash portdir.sh $1) f; f=$(ls $p 2>/dev/null | grep -ix -E "$2\.(c|cpp|cxx|asm)") && { echo $p/$f; return; }; f=$(ls $1 | grep -ix -E "$2\.(c|cpp|cxx)") && echo $1/$f; }

# DISPCLFLAGS with DISPINCFLAGS: INC\OLEDISP, then the SDK
DDEFS="-DWIN32 -DINC_OLE2 -D_MT -DOA_UNICODE -DUNICODE -D_ARM_=1 -DVBA2=1"
DINCS="-I$K/inc -I/tmp/f/ovr $(bash incdirs.sh $S ../../INC/OLEDISP) -I/tmp/f/oak -I/tmp/f/pub -I/tmp/f/csr -I/tmp/f/crt"
# OLECLFLAGS with VBAJETSTR and OLEINCS: DESTDIR (version.hxx), INC\TYPELIB, then the SDK
TDEFS="-DOE_JET=1 -DFV_UNICODE_OLE=1 -D_ARM_=1 -DWIN32 -DConst=const -DOSDEBUG"
TINCS="-I$K/inc -I/tmp/f/ovr $(bash incdirs.sh $T "../../R2MIPSND;../../INC/TYPELIB") -I/tmp/f/oak -I/tmp/f/pub -I/tmp/f/csr -I/tmp/f/crt"

# A source of the typelib as Microsoft C compiled it with the precompiled header of tlibpch.cxx
# (PCHOLE, -Yu): the state of tlibpch.cxx at its #pragma hdrstop, then the source after its own
# (the compiler skipped what comes before it).
PCH=$(src $T tlibpch)
pch() {
  local n
  n=$(tr -d "\r\032" < $1 | grep -n -m1 "^#pragma[[:space:]]*hdrstop" | cut -d: -f1)
  [ -n "$n" ] || { echo "   no hdrstop in $(basename $1)"; return 1; }
  { tr -d "\r\032" < $PCH | sed -n "/^#pragma[[:space:]]*hdrstop/q;p"; echo "#line $((n + 1)) \"$1\""; tr -d "\r\032" < $1 | sed "1,${n}d"; } > $2
}
disp() {
  local f=$1 b=$(basename "${1%.*}" | tr A-Z a-z) e=c
  case $f in
  *.asm) ntas $DIR/d/$b.obj $f -D_ARM_=1 -D_LANGUAGE_ASSEMBLY; return ;;
  *.[Cc][Pp][Pp]) e=cpp ;;
  esac
  ntsrc $f $DIR/d/$b.$e && ntcc $DIR/d/$b.obj $DIR/d/$b.$e $DDEFS $DINCS
}
# (OLEBLD for all but dassert.c)
tlib() {
  local f=$1 b=$(basename "${1%.*}" | tr A-Z a-z) d="$TDEFS -DOLEBLD"
  case $f in
  *.[Cc][Xx][Xx]) pch $f $DIR/pch/$b.cxx && ntsrc $DIR/pch/$b.cxx $DIR/t/$b.cxx && ntcc $DIR/t/$b.obj $DIR/t/$b.cxx $d $TINCS ;;
  *) [ $b != dassert ] || d=$TDEFS
     ntsrc $f $DIR/t/$b.c && ntcc $DIR/t/$b.obj $DIR/t/$b.c $d $TINCS ;;
  esac
}

DOBJS=$(objs OLEDISP_OBJS)
TOBJS="$(objs MISCLIB_OLEOBJ) $(objs CLOBJ_OLE) tlibpch"
echo ">> compiling CAIROLE/OLEAUTO (ole2di32.lib, oleaut32.dll)"
for b in $DOBJS; do
  case $b in
  oledisp) f=$(src $S/WIN32 $b) ;;
  invoke) f=$(src $S/WIN32/ARM $b) ;;
  oleconva) f=$(src $S/WIN32/MIPS $b) ;;
  *) f=$(src $S $b) ;;
  esac
  [ -n "$f" ] || { echo "   no source of $b"; exit 1; }
  par disp $f
done
for b in $TOBJS; do
  f=$(src $T $b) || { echo "   no source of $b"; exit 1; }
  par tlib $f
done
pwait
echo "   $PAROK compiled, $PARFAIL failed"
[ $PARFAIL -eq 0 ] || exit 1
ntlib $DIR/ole2di32.lib $(for b in $DOBJS; do echo $DIR/d/$b.obj; done) || exit 1

# the .def as the makefile makes it (no UNICODE_OLE): the C preprocessor, then win32def.awk
tr -d "\r\032" < $A/BIN/WIN32DEF.AWK > $DIR/win32def.awk
ntsrc $(tf $T oleaut32.def) $DIR/oleaut32.src
cl -nologo -EP -DOE_JET=1 $TINCS -Tc $DIR/oleaut32.src 2>/dev/null | awk -f $DIR/win32def.awk | sed -f deffix.sed > $DIR/oleaut32.def
# RCFLAGS with the DBCS of make.bat (CHARSIZE D)
RES=$(bash rc.sh $T $(tf $T oleaut32.rc) -DLANGTOK=VAEN232 -DFV_DBCS $TINCS) || exit 1
LIBS=""
for l in ole32 ole2di32 uuid gdi32 user32 libcmt kernel32 advapi32 mpr; do
  a=$U/$l.lib; [ $l != ole2di32 ] || a=$DIR/$l.lib
  [ -f $a ] || { echo "   missing library $l (build it first)"; exit 1; }
  LIBS="$LIBS $a"
done
ntcc $DIR/newarray.obj /work/RASPI/crt/cxx/newarray.cxx && ntlib $DIR/newarray.lib $DIR/newarray.obj || exit 1
link -nologo -machine:arm -release -dll -entry:_DllMainCRTStartup -subsystem:windows -nodefaultlib \
     -debug -pdb:$DIR/oleaut32.pdb -pdbaltpath:"Z:$(echo $O/oleaut32.pdb | tr / "\\\\")" -map:$DIR/oleaut32.map \
     -def:$DIR/oleaut32.def -implib:$DIR/oleaut32.lib -out:$DIR/OLEAUT32.DLL \
     $(for b in $TOBJS; do echo $DIR/t/$b.obj; done) $RES $LIBS $DIR/newarray.lib > $DIR/link.err 2>&1 \
  || { echo ">> LINK FAILED"; grep -E "error|unresolved" $DIR/link.err | sort | uniq -c | sort -rn | head -40; exit 1; }
cp $DIR/oleaut32.pdb $DIR/oleaut32.map $O/
bash /work/RASPI/tools/host.sh pe2img -version 3.50 $DIR/OLEAUT32.DLL || exit 1
ntimplib $DIR/oleaut32.def $DIR/oleaut32.lib $U/oleaut32.lib
OUT=$O; [ "$INSTALL" = 1 ] && OUT=/work/RASPI/sdcard/root/WINNT/System32 && mkdir -p $OUT
cp $DIR/OLEAUT32.DLL $OUT/OLEAUT32.DLL
echo "   wrote $OUT/OLEAUT32.DLL"
'
