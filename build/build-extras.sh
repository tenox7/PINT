#!/usr/bin/env bash
# Build the extras: programs too big for this repository, kept in their own, built for PINT 4.0
# when their checkout, or a link to it, is in extras/ with the name of their recipe,
# extras/NAME.sh. A recipe sets
#   BUILD  the command that builds it (default nmake), in its build copy: in the environment of
#          the PINT 4.0 SDK (build/make-sdk.sh), or with ENV=nt in that of the NT build
#          (build/msvc.sh): Microsoft C for ARM, the header farm, the libraries of obj/um and
#          obj; INCLUDE names the headers of user mode, KMINC also those of kernel mode
#          (winddi.h, miniport.h), EXTRAS the extras/ of the port; pe2img and ntver are at hand
#   COPY   what it takes of its checkout (default all)
#   DIR    the directory of its build copy in obj/extras (default NAME), beside the others
#   NEEDS  the extras it takes, built first (default none)
#   DEST   its directory in sdcard/root, and FILES, what goes there from the build copy (default
#          none)
#   REPO   instead of a checkout: its GitHub repository (OWNER/NAME), and GET, what it takes of
#          the newest release there that has it all (no draft or pre-release): NAME=PATTERN, the
#          file of the release whose name matches PATTERN (* and ?), as NAME; PATTERN, a .zip of
#          the release, unpacked; NAME=/PATH, the file PATH of the repository at that release.
#          Each file of the release must have the SHA-256 GitHub has of it. Then BUILD (default
#          none) runs in the build copy, on the host
# An extra builds anew, in a copy without .git and without the objects, libraries and programs
# of other builds (those its git does not track: a file of the repository with such a name is
# kept, as Sabre's 3D models lib/*.OBJ), when what it takes of its checkout, its recipe (and its
# other files in extras/, NAME.*), an extra it needs, the SDK or (ENV=nt) the libraries changed;
# then its files replace those in sdcard/root/DEST. A checkout that is no git work tree is copied
# as it is. Without its checkout, that directory goes. An extra with REPO asks GitHub for its
# release at each build (build/release.pl; GITHUB_TOKEN raises GitHub's limit of 60 requests an
# hour) and is made anew when that release changes; when GitHub does not answer, it keeps the one
# it has.
# build-update.sh runs it before the system disk.
#
# Usage:  build/build-extras.sh [NAME]
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
E=$R/extras O=$R/obj/extras G=$R/sdcard/root
recipe() { BUILD= ENV=sdk COPY=. DIR=$1 NEEDS= DEST= FILES= REPO= GET=; . $E/$1.sh; [ -n "$BUILD$REPO" ] || BUILD=nmake; }

# release NAME: the release of an extra with REPO (build/release.pl) in obj/extras/NAME.release, written anew only
# when it changes, as an input of its step
release() {
    local rc=0
    mkdir -p $O
    GET="$GET" perl $R/build/release.pl $REPO > $O/$1.release.new || rc=$?
    if [ $rc = 0 ]; then
        cmp -s $O/$1.release.new $O/$1.release || mv $O/$1.release.new $O/$1.release
        rm -f $O/$1.release.new
    elif [ $rc = 2 ] && [ -f $O/$1.release ]; then
        rm -f $O/$1.release.new
        echo "   extras/$1: GitHub did not answer, keeping $(sed -n 's/^tag //p' $O/$1.release) of $REPO"
    else
        rm -f $O/$1.release.new
        echo "!! extras/$1: no release of $REPO with what extras/$1.sh takes"; exit 1
    fi
}
# fetch NAME URL FILE SHA256: URL as FILE, which must have that SHA-256 (none: unchecked)
fetch() {
    echo "   download $2"
    curl -fsSL --retry 3 -o "$3" "$2" || { echo "!! extras/$1: cannot download $2"; exit 1; }
    [ -z "$4" ] || [ "$(shasum -a 256 "$3" | cut -d" " -f1)" = "$4" ] || { echo "!! extras/$1: $2 is not the file GitHub has (SHA-256)"; exit 1; }
}

# one extra, the step of each
if [ $# -gt 0 ]; then
    recipe $1
    rm -rf "${O:?}/${DIR:?}"
    mkdir -p $O/$DIR
    if [ -n "$REPO" ]; then
        [ -f $O/$1.release ] || release $1
        while read -r k a b c; do
            case $k in
            tag)  echo "   $REPO $a" ;;
            file) fetch $1 "$b" "$O/$DIR/$a" "$c" ;;
            zip)  fetch $1 "$a" "$O/$DIR/.zip" "$b"; unzip -qo "$O/$DIR/.zip" -d "$O/$DIR"; rm "$O/$DIR/.zip" ;;
            repo) fetch $1 "$b" "$O/$DIR/$a" "" ;;
            esac
        done < $O/$1.release
        (cd $O/$DIR && eval "$BUILD")
    else
        c=$(cd -P $E/$1 && pwd)
        (cd "$c" && tar --exclude .git -cf - $COPY) | tar -C $O/$DIR -xf -
        if [ "$(git -C "$c" rev-parse --show-toplevel 2>/dev/null)" = "$c" ]; then
            shopt -s nocasematch
            git -C "$c" ls-files -o -z | while IFS= read -r -d '' f; do
                case $f in *.obj|*.exe|*.dll|*.lib|*.exp|*.res|*.pdb|*.ilk|*.map) rm -f "$O/$DIR/$f" ;; esac
            done
            shopt -u nocasematch
        fi
        if [ $ENV = nt ]; then
            NT40=$(cd $R/.. && pwd)
            docker run --rm -e DIR="$DIR" -e BUILD="$BUILD" -v "$NT40/../nt35/msvc":/opt/msvc:ro -v "$NT40/../nt35":/nt35:ro \
                -v "$NT40/../XPSP1":/XPSP1:ro -v "$NT40":/work -v "$NT40/private":/work/private:ro -v "$NT40/public":/work/public:ro \
                -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -u
. ./msvc.sh
bash mkfarm.sh /tmp/f > /tmp/farm.log 2>&1 || { cat /tmp/farm.log; exit 1; }
# the stdarg.h of the port (that of the run-time has no ARM) and stcrt.h, ahead of the farm, as the SDK has them
mkdir -p /tmp/x && cp $K/inc/stdarg.h $K/inc/stcrt.h /tmp/x/
w() { printf "Z:%s" "$1" | tr / "\\\\"; }
case $(uname -m) in aarch64) h=arm64 ;; *) h=x64 ;; esac
b=$(ls -d /opt/msvc/VC/Tools/MSVC/*/bin/Host$h/arm | tail -1)
export WINEPATH="$(w $b);$(w $(ls -d /opt/msvc/kits/bin/*/$h | tail -1))"
v="$(w /tmp/f/pub);$(w /tmp/f/crt);$(w ${b%/bin/*}/include)"
export INCLUDE="$(w /tmp/x);$v" KMINC="$(w /tmp/x);$(w /tmp/f/priv);$(w /tmp/f/pinc);$(w /tmp/f/oak);$v"
export LIB="$(w /work/RASPI/obj/um);$(w /work/RASPI/obj)" EXTRAS=/work/RASPI/extras
pe2img() { bash /work/RASPI/tools/host.sh pe2img "$@"; }
cd /work/RASPI/obj/extras/$DIR
eval "$BUILD"'
        else
            docker run --rm -v "$(cd $R/../../nt35/msvc && pwd)":/opt/msvc:ro -v $R/obj/sdk/pint40sdk:/opt/pint40sdk:ro \
                -v $R/sdk/docker/pint-env:/usr/local/bin/pint-env:ro -v $R/sdk/docker/wine-sdk:/usr/local/bin/wine-sdk:ro \
                -v $R/sdk/docker/cl:/usr/local/bin/cl:ro -v $R/sdk/docker/link:/usr/local/bin/link:ro -v $O:/src -w /src/$DIR \
                --entrypoint sh tenox7/msvc-arm32 -c ". /usr/local/bin/pint-env && $BUILD"
        fi
    fi
    [ -n "$DEST" ] || exit 0
    rm -rf "$G/${DEST:?}"
    mkdir -p $G/$DEST
    cd $O/$DIR
    cp -R $FILES $G/$DEST/
    exit 0
fi

# the extras with a checkout or REPO, after the SDK and those they need
. $R/build/stamp.sh
P=() N=() SDK=
for f in $E/*.sh; do
    n=$(basename $f .sh)
    recipe $n
    if [ -z "$REPO" ] && [ ! -d $E/$n ]; then
        rm -f $ST/extra_$n
        [ -z "$DEST" ] || rm -rf "$G/${DEST:?}"
        continue
    fi
    for d in $NEEDS; do [ -d $E/$d ] || { echo "!! extras/$n needs extras/$d"; exit 1; }; done
    [ -n "$NEEDS" ] && N+=($n) || P+=($n)
    [ -n "$REPO" ] || [ $ENV = nt ] || SDK=1
done
[ ${#P[@]} -gt 0 ] || [ ${#N[@]} -gt 0 ] || exit 0

[ -z "$SDK" ] || step pint40sdk $R/sdk $R/obj/um $R/crt/cxx/newarray.cxx $R/obj/ntoskrnl.lib $R/obj/hal.lib \
    $R/obj/videoprt.lib $R/obj/win32k.lib $R/obj/ndis.lib $R/obj/scsiport.lib $R/obj/libcntpr.lib -- $B/make-sdk.sh
extra() {
    local n=$1 c d x i=()
    recipe $n
    if [ -n "$REPO" ]; then
        release $n
        i+=($O/$n.release)
    else
        c=$(cd -P $E/$n && pwd)
        for x in $COPY; do i+=("$c/$x"); done
        if [ $ENV = nt ]; then i+=($R/obj/um/*.lib $R/obj/*.lib); else i+=($ST/pint40sdk); fi
    fi
    for d in $NEEDS; do i+=($ST/extra_$d); done
    job extra/$n step extra/$n $E/$n.* ${i[@]+"${i[@]}"} -- $B/build-extras.sh $n
}
for n in ${P[@]+"${P[@]}"}; do extra $n; done
barrier
for n in ${N[@]+"${N[@]}"}; do extra $n; done
barrier
