#!/usr/bin/env bash
# The pass 0 of the NT build: run the midl (and wcshdr) recipes of the components' MAKEFIL0
# (with build/nmk.awk) on a lower case mirror of their directories, with
# MIDL (obj/host, tools/make-tools.sh, on the 32-bit Wine: tools/host.sh) and the ARM
# target: MIDL_CPP is the cl -E of the target (obj/host/MIDLCPP.EXE, midlcpp),
# MIDL_FLAGS -D_ARM_ -D_WCHAR_T_DEFINED. The stubs and headers it makes land in
# obj/gen/midl/<directory>, where mkfarm.sh gives them to the builds.
#
# Usage:  cd RASPI/tools/midl && ./gen-midl.sh          # all
#         ./gen-midl.sh NET/SVCDLLS LSA            # the recipes of those makefiles only
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NT35="$(cd ../../.. && pwd)"
[ -f "$NT35/RASPI/obj/host/MIDL.EXE" ] || { echo "gen-midl: build the tools first (tools/make-tools.sh)"; exit 1; }

docker run --rm -e ONLY="$*" -v "$NT35/msvc":/opt/msvc -v "$NT35":/work -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -eu
B=/work/RASPI/build
P=/work/PRIVATE
W=/tmp/mw
G=/work/RASPI/obj/gen/midl
bash mkfarm.sh /tmp/f > /dev/null
host() { bash /work/RASPI/tools/host.sh "$@"; }
dos() { printf "Z:%s" "$1" | tr / "\\\\"; }

lower() { printf "%s" "$1" | tr A-Z a-z; }
# The real directory of a path relative to the current one, case insensitively.
real() {
    local p=$CUR c n
    for c in $(echo "$1" | tr "/" " "); do
        case $c in
        ..) p=$(dirname $p) ;;
        .) ;;
        *) n=$(ls $p 2>/dev/null | grep -ix "$c" | head -1); [ -n "$n" ] || return 1; p=$p/$n ;;
        esac
    done
    echo $p
}
# Mirror a directory of the tree, lower case, with all its subdirectories.
mirror() {
    local m=$W/$(lower "${1#$P/}") f
    [ -e $m/.mirrored ] && return
    mkdir -p $m
    (cd "$1" && find . -type d) | while read -r f; do mkdir -p "$m/$(lower "$f")"; done
    (cd "$1" && find . -type f) | while read -r f; do
        tr -d "\032\r" < "$1/$f" | sed -E -f /work/RASPI/tools/midl/incpath.sed > "$m/$(lower "$f")"
    done
    find $m -type f -exec touch -d @0 {} +
    find $m -type d | while read -r f; do : > "$f/.mirrored"; done
}
# A path of a recipe: the SDK and OAK include directories are the farm of the ARM
# build (the ARM headers ahead of the SDK ones); a directory of the tree is mirrored.
path() {
    case $1 in
    BASEDIR/public/sdk/inc) echo "/work/RASPI/kernel/inc -I/tmp/f/pub" ;;
    BASEDIR/public/sdk/inc/crt) echo /tmp/f/crt ;;
    BASEDIR/public/oak/inc) echo /tmp/f/oak ;;
    BASEDIR/private/*) local r; r=$(CUR=/work real "${1#BASEDIR/}") && mirror $r && echo $W/$(lower "${r#$P/}") ;;
    nul|NUL) echo nul ;;
    *) local r d=$1
       [ -d "$(real "$d")" 2>/dev/null ] || d=$(dirname "$1")
       r=$(real "$d") && mirror $r
       lower "$1" ;;
    esac
}
arg() {
    case $1 in
    -I*) local i; for i in $(printf "%s" "${1#-I}" | tr ";" " "); do echo "-I$(path "$i")"; done ;;
    -*) echo "$1" ;;
    *) path "$1" ;;
    esac
}
nmk_midl() {
    local a=() x o
    while [ $# -gt 0 ]; do
        case $1 in
        -cpp_cmd) a+=("$1" "$2"); shift 2 ;;
        -cpp_opt) o=""; for x in $2; do for x in $(arg $x); do case $x in -I/*) x=-I$(dos ${x#-I}) ;; esac; o="$o $x"; done; done; a+=(-cpp_opt "$o"); shift 2 ;;
        -prefix) a+=("$1" "$2" "$3"); shift 3 ;;
        -acf|-header|-cstub|-sstub|-saux|-caux) a+=("$1" "$(path "$2")"); shift 2 ;;
        -I*) for x in $(arg "$1"); do a+=("$x"); done; shift ;;
        -*) a+=("$1"); shift ;;
        *) a+=("$(path "$1")"); shift ;;
        esac
    done
    local s=0
    host midl "${a[@]}" > /tmp/midl.out 2>&1 || s=$?
    grep -v "^Processing\\|^Microsoft\\|^Copyright\\|MIDL2169" /tmp/midl.out || true
    return $s
}
nmk_exists() { [ -e "$(lower "$1")" ]; }
nmk_copy() { cp "$(path "$1")" "$(path "$2")"; }
nmk_del() { local f; for f in "$@"; do rm -f "$(lower "$f")"; done; }
nmk_wcshdr() { host wcshdr < "$(path "$1")" > "$(path "$2")"; }

recipe() {
    local mk=$1; shift
    want $mk || return 0
    CUR=$(dirname $P/$mk)
    # the component: the includes reach its sibling directories (NOMIRROR: only the
    # directories the recipe names)
    if [ -n "${NOMIRROR:-}" ]; then mkdir -p $W/$(lower "${CUR#$P/}"); elif [ "$(dirname $CUR)" = $P ]; then mirror $CUR; else mirror $(dirname $CUR); fi
    : > /tmp/mw.stamp
    sleep 1
    for t in "$@"; do
        NMK_MACROS="MIDL_CPP=/work/RASPI/obj/host/MIDLCPP.EXE
MIDL_FLAGS=-D_ARM_ -D_WCHAR_T_DEFINED
${MACROS:-}" NMK_KEEP=BASEDIR NMK_TARGET="$t" awk -f $B/nmk.awk $P/$mk > /tmp/nmk.sh
        (cd $W/$(lower "${CUR#$P/}") && . /tmp/nmk.sh)
    done
    keep
}
# what the recipes made, in the directories of the tree
keep() {
    find $W -type f -newer /tmp/mw.stamp ! -name .mirrored | while read -r f; do
        d=$(dirname "$f"); r=$P
        for c in $(echo "${d#$W/}" | tr "/" " "); do r=$r/$(ls $r | grep -ix "$c" | head -1); done
        mkdir -p $G/${r#$P/}
        tr -d "\r" < "$f" > $G/${r#$P/}/$(basename "$f")
        echo "   ${r#$P/}/$(basename "$f")"
    done
}
# (MACROS: more macros of a recipe.) A makefile.inc recipe with the macros of the SOURCES
# beside it, as makefile.def has them when it reads the makefile.inc (INCLUDES, C_DEFINES:
# the ARM MIDL_FLAGS last), TARGET_CPP and MIDL.
srcrecipe() {
    want $1 || return 0
    local m; m=$(bash $B/srcmacro.sh $P/$(dirname $1)/SOURCES "*" | sed -e "s/\$(BASEDIR)/BASEDIR/g" -e "s/\$([A-Za-z_0-9]*)//g")
    MACROS="$m
C_DEFINES=$(printf "%s\n" "$m" | sed -n "s/^C_DEFINES=//p") -D_ARM_ -D_WCHAR_T_DEFINED
TARGET_CPP=/work/RASPI/obj/host/MIDLCPP.EXE
MIDL=midl" recipe "$@"
}
# The moves of allidl after its MIDL (mv, which nmk.awk has not): the stubs to the components
# that compile them (com\class\daytona, scm\daytona).
allidl() {
    want $1 || return 0
    local d=$(dirname $1) t f to r
    t=$(tr -d "\032\r" < $P/$1 | sed -n "s/^DEST_TREE=//p")
    tr -d "\032\r" < $P/$1 | awk "/^allidl:/ { on = 1; next } /^[^ \t#]/ { on = 0 } on && \$1 == \"mv\" { print \$2, \$3 }" |
        while read -r f to; do
            to=$(printf "%s" "$to" | sed "s/\$(DEST_TREE)/$t/" | tr "\\\\" /)
            r=$(CUR=$P/$d real "${to%/.}") || { echo "gen-midl: allidl: no $to"; exit 1; }
            mkdir -p $G/${r#$P/} && mv $G/$d/$f $G/${r#$P/}/ && echo "   ${r#$P/}/$f (allidl)"
        done
}
# The interfaces of OLE by the .idl rule of TYPES\makefile.inc, as the pass1.mk of COMPOBJ,
# NEW_OLE and OLEAUTO run it (OPSYS NT1X): MIDL with IDLMIDLFLAGS (-import defined_single -Zp8 -Oi)
# and CINC (., OLEPRX32\DAYTONA, CINC, COMPOBJ, the SDK), its preprocessor with CPLATFORM, that
# of ARM; the proxies, GUIDs and aux routines to OLEPRX32\DAYTONA (MIDL_OUT_DIR), for oleprx32
# and uuid.lib. A header of the SDK (MIDL_HDR_OUT_DIR public\sdk\inc) is the one of the tree,
# which NT made: the one made here is not kept, nor the dlldata.c MIDL writes beside each
# interface (OLEPRX32\DAYTONA has its own).
types() {
    want TYPES || return 0
    local f d n inc="-I. -I../oleprx32/daytona -IBASEDIR/private/cinc -IBASEDIR/private/types/compobj -IBASEDIR/public/sdk/inc/crt -IBASEDIR/public/sdk/inc"
    CUR=$P/TYPES; mirror $CUR; mkdir -p /tmp/mwh
    : > /tmp/mw.stamp
    sleep 1
    for f in COMPOBJ/COM COMPOBJ/WTYPES COMPOBJ/UNKNWN NEW_OLE/OLE2X OLEAUTO/OLEAUT; do
        d=$(lower ${f%/*}); n=$(lower ${f#*/}); CUR=$P/TYPES/${f%/*}
        (cd $W/types/$d && nmk_midl -import defined_single -Zp8 -Oi $inc -no_warn -char unsigned -mode c_port \
            -proxy ../oleprx32/daytona/${n}_p.c -iid ../oleprx32/daytona/${n}_i.c -header /tmp/mwh/$n.h \
            -caux ../oleprx32/daytona/${n}_x.c -saux ../oleprx32/daytona/${n}_y.c -cpp_cmd /work/RASPI/obj/host/MIDLCPP.EXE \
            -cpp_opt "-nologo -DMIDL_PASS -D_ARM_ -D_WCHAR_T_DEFINED -DFLAT -DWIN32=100 -D_NT1X_=100 $inc -E -Tc" $n.idl)
        rm -f $W/types/$d/dlldata.c
    done
    keep
}

# the recipes named (by the start of their makefile path), else all of them afresh
want() { local o; [ -z "$ONLY" ] && return 0; for o in $ONLY; do case $1 in $o*) return 0 ;; esac; done; return 1; }
[ -n "$ONLY" ] || rm -rf $G
mkdir -p $G
recipe WINDOWS/SCREG/WINREG/MAKEFIL0 "\$(TARGETS)"
recipe WINDOWS/SCREG/SC/MAKEFIL0 "\$(TARGETS)"
recipe LSA/MAKEFIL0 "\$(CLIENT_TARGETS)" "\$(SERVER_TARGETS)"
recipe NEWSAM/MAKEFIL0 "\$(CLIENT_TARGETS)" "\$(SERVER_TARGETS)"
recipe EVENTLOG/MAKEFIL0 "elfclnt\\\$(ELFIDL)_c.c elfclnt\\\$(ELFIDL)_x.c server\\\$(ELFIDL)_y.c server\\\$(ELFIDL)_s.c \$(ELFIDL).h"
recipe RPC/RUNTIME/RTIFS/MAKEFIL0 "\$(MGMT_HDR) \$(MGMT_CSTUB) \$(MGMT_SSTUB)" "\$(CONV_HDR) \$(CONV_CSTUB) \$(CONV_SSTUB)" "\$(EPMP_HDR) \$(EPMP_CSTUB) \$(EPMP_SSTUB)"
recipe WINDOWS/SPOOLER/SPOOLSS/MAKEFIL0 "\$(TARGETS)"
recipe WINDOWS/NETDDE/SRC/NDEAPI/MAKEFIL0 "\$(TARGETS)"
NOMIRROR=1 recipe NET/MAKEFIL0 "inc\\dosprint.h" "inc\\rxprint.h"
for s in SRVSVC WKSSVC BROWSER REPL MSGSVC AT; do recipe NET/SVCDLLS/$s/MAKEFIL0 "\$(TARGETS)"; done
for s in LOGONSRV RPL; do recipe NET/SVCDLLS/$s/MAKEFIL0 "\$(CLIENT_TARGETS)" "\$(SERVER_TARGETS)"; done
recipe NET/SOCKETS/TCPCMD/FTPSVC/MAKEFIL0 "\$(TARGETS)"
# (the name service interfaces: the client stubs of rpcns4, the server stubs of the locator)
recipe RPC/LOCATOR/MAKEFIL0 nsicom.h nsisvr.h nsiclt.h nsimgm.h loctoloc.h
# (OLE: the interfaces of compobj and its service control manager, the NTTARGETFILE0 allidl of
# its makefile.inc; the interfaces of OLE, the proxies of oleprx32 and the GUIDs of uuid.lib)
srcrecipe CAIROLE/COM/IDL/DAYTONA/MAKEFILE.INC drot.h getif.h ichnl.h iface.h objsrv.h osrot.h scm.h
allidl CAIROLE/COM/IDL/DAYTONA/MAKEFILE.INC
types
'
