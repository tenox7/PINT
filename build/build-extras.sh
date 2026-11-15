#!/usr/bin/env bash
# Build the extras: programs too big for this repository, kept in their own, built for PINT 3.5
# when their checkout, or a link to it, is in extras/ with the name of their recipe,
# extras/NAME.sh, in the environment of the NT build (build/msvc.sh): Microsoft C for ARM, the
# header farm, the libraries of obj/um and obj. A recipe sets
#   BUILD  the command that builds it (default nmake), in its build copy; INCLUDE names the
#          headers of user mode, DDKINC also those of the DDK (winddi.h), EXTRAS the extras/ of
#          the port; pe2img is at hand
#   COPY   what it takes of its checkout (default all)
#   DIR    the directory of its build copy in obj/extras (default NAME), beside the others
#   NEEDS  the extras it takes, built first (default none)
#   DEST   its directory in sdcard/root, and FILES, what goes there from the build copy, under
#          names in upper case as the rest of the disk (default none)
# An extra builds anew, in a copy without .git and without the objects, libraries and programs
# of other builds (those its git does not track), when what it takes of its checkout, its recipe
# (and its other files in extras/, NAME.*), an extra it needs or the libraries changed; then its
# files replace those in sdcard/root/DEST. A checkout that is no git work tree is copied as it
# is. Without its checkout, that directory goes.
# build-update.sh runs it before the system disk.
#
# Usage:  build/build-extras.sh [NAME]
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
E=$R/extras O=$R/obj/extras G=$R/sdcard/root
recipe() { BUILD=nmake COPY=. DIR=$1 NEEDS= DEST= FILES=; . $E/$1.sh; }

# one extra, the step of each
if [ $# -gt 0 ]; then
    recipe $1
    c=$(cd -P $E/$1 && pwd)
    rm -rf "${O:?}/${DIR:?}"
    mkdir -p $O/$DIR
    (cd "$c" && tar --exclude .git -cf - $COPY) | tar -C $O/$DIR -xf -
    if [ "$(git -C "$c" rev-parse --show-toplevel 2>/dev/null)" = "$c" ]; then
        shopt -s nocasematch
        git -C "$c" ls-files -o -z | while IFS= read -r -d '' f; do
            case $f in *.obj|*.exe|*.dll|*.lib|*.exp|*.res|*.pdb|*.ilk|*.map) rm -f "$O/$DIR/$f" ;; esac
        done
        shopt -u nocasematch
    fi
    NT35=$(cd $R/.. && pwd)
    docker run --rm -e DIR="$DIR" -e BUILD="$BUILD" -v "$NT35/msvc":/opt/msvc:ro -v "$NT35":/work \
        -v "$NT35/PRIVATE":/work/PRIVATE:ro -v "$NT35/PUBLIC":/work/PUBLIC:ro \
        -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -u
. ./msvc.sh
bash mkfarm.sh /tmp/f > /tmp/farm.log 2>&1 || { cat /tmp/farm.log; exit 1; }
# the stdarg.h of the port (that of the run-time has no ARM) and stcrt.h, ahead of the farm
mkdir -p /tmp/x && cp $K/inc/stdarg.h $K/inc/stcrt.h /tmp/x/
w() { printf "Z:%s" "$1" | tr / "\\\\"; }
case $(uname -m) in aarch64) h=arm64 ;; *) h=x64 ;; esac
b=$(ls -d /opt/msvc/VC/Tools/MSVC/*/bin/Host$h/arm | tail -1)
export WINEPATH="$(w $b);$(w $(ls -d /opt/msvc/kits/bin/*/$h | tail -1))"
v="$(w /tmp/f/pub);$(w /tmp/f/crt);$(w ${b%/bin/*}/include)"
export INCLUDE="$(w /tmp/x);$v" DDKINC="$(w /tmp/x);$(w /tmp/f/oak);$v"
export LIB="$(w /work/RASPI/obj/um);$(w /work/RASPI/obj)" EXTRAS=/work/RASPI/extras
pe2img() { bash /work/RASPI/tools/host.sh pe2img "$@"; }
cd /work/RASPI/obj/extras/$DIR
eval "$BUILD"'
    [ -n "$DEST" ] || exit 0
    rm -rf "$G/${DEST:?}"
    mkdir -p $G/$DEST
    cd $O/$DIR
    for f in $FILES; do cp -R $f "$G/$DEST/$(basename $f | tr a-z A-Z)"; done
    exit 0
fi

# the extras with a checkout, after those they need
. $R/build/stamp.sh
P=() N=()
for f in $E/*.sh; do
    [ -e "$f" ] || continue
    n=$(basename $f .sh)
    recipe $n
    if [ ! -d $E/$n ]; then
        rm -f $ST/extra_$n
        [ -z "$DEST" ] || rm -rf "$G/${DEST:?}"
        continue
    fi
    for d in $NEEDS; do [ -d $E/$d ] || { echo "!! extras/$n needs extras/$d"; exit 1; }; done
    [ -n "$NEEDS" ] && N+=($n) || P+=($n)
done
[ ${#P[@]} -gt 0 ] || [ ${#N[@]} -gt 0 ] || exit 0

extra() {
    local n=$1 c d x i=()
    recipe $n
    c=$(cd -P $E/$n && pwd)
    for x in $COPY; do i+=("$c/$x"); done
    for d in $NEEDS; do i+=($ST/extra_$d); done
    job extra/$n step extra/$n $E/$n.* "${i[@]}" $R/obj/um/*.lib $R/obj/*.lib -- $B/build-extras.sh $n
}
for n in ${P[@]+"${P[@]}"}; do extra $n; done
barrier
for n in ${N[@]+"${N[@]}"}; do extra $n; done
barrier
