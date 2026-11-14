#!/usr/bin/env bash
# Build the extras: programs too big for this repository, kept in their own, built with the
# PINT 4.0 SDK (build/make-sdk.sh) when their checkout, or a link to it, is in extras/ with the
# name of their recipe, extras/NAME.sh. A recipe sets
#   BUILD  the command that builds it in the SDK's environment (default nmake)
#   DIR    the directory of its build copy in obj/extras (default NAME), beside the others
#   NEEDS  the extras it takes, built first (default none)
#   GAME   its directory in \GAMES, and FILES, what goes there from the build copy (default none)
# An extra builds anew, in a copy without .git and without the objects, libraries and programs
# of other builds (those its git does not track: a file of the repository with such a name is
# kept, as Sabre's 3D models lib/*.OBJ), when its checkout, its recipe, an extra it needs or
# the SDK changed; then its files replace those in sdcard/root/GAMES/GAME. A checkout that is
# no git work tree is copied as it is. Without its checkout, that directory goes.
# build-update.sh runs it before the system disk.
#
# Usage:  build/build-extras.sh [NAME]
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
E=$R/extras O=$R/obj/extras G=$R/sdcard/root/GAMES
recipe() { BUILD=nmake DIR=$1 NEEDS= GAME= FILES=; . $E/$1.sh; }

# one extra, the step of each
if [ $# -gt 0 ]; then
    recipe $1
    c=$(cd -P $E/$1 && pwd)
    rm -rf "${O:?}/${DIR:?}"
    mkdir -p $O/$DIR
    tar -C "$c" --exclude .git -cf - . | tar -C $O/$DIR -xf -
    if [ "$(git -C "$c" rev-parse --show-toplevel 2>/dev/null)" = "$c" ]; then
        shopt -s nocasematch
        git -C "$c" ls-files -o -z | while IFS= read -r -d '' f; do
            case $f in *.obj|*.exe|*.dll|*.lib|*.exp|*.res|*.pdb|*.ilk|*.map) rm -f "$O/$DIR/$f" ;; esac
        done
        shopt -u nocasematch
    fi
    docker run --rm -v "$(cd $R/../../nt35/msvc && pwd)":/opt/msvc:ro -v $R/obj/sdk/pint40sdk:/opt/pint40sdk:ro \
        -v $R/sdk/docker/pint-env:/usr/local/bin/pint-env:ro -v $R/sdk/docker/wine-sdk:/usr/local/bin/wine-sdk:ro \
        -v $R/sdk/docker/cl:/usr/local/bin/cl:ro -v $R/sdk/docker/link:/usr/local/bin/link:ro -v $O:/src -w /src/$DIR \
        --entrypoint sh tenox7/msvc-arm32 -c ". /usr/local/bin/pint-env && $BUILD"
    [ -n "$GAME" ] || exit 0
    rm -rf "$G/${GAME:?}"
    mkdir -p $G/$GAME
    cd $O/$DIR
    cp -R $FILES $G/$GAME/
    exit 0
fi

# the extras with a checkout, after the SDK and those they need
. $R/build/stamp.sh
P=() N=()
for f in $E/*.sh; do
    n=$(basename $f .sh)
    recipe $n
    if [ ! -d $E/$n ]; then
        rm -f $ST/extra_$n
        [ -z "$GAME" ] || rm -rf "$G/${GAME:?}"
        continue
    fi
    for d in $NEEDS; do [ -d $E/$d ] || { echo "!! extras/$n needs extras/$d"; exit 1; }; done
    [ -n "$NEEDS" ] && N+=($n) || P+=($n)
done
[ ${#P[@]} -gt 0 ] || [ ${#N[@]} -gt 0 ] || exit 0

step pint40sdk $R/sdk $R/obj/um $R/crt/cxx/newarray.cxx -- $B/make-sdk.sh
extra() {
    local n=$1 d i=()
    recipe $n
    for d in $NEEDS; do i+=($ST/extra_$d); done
    job extra/$n step extra/$n "$(cd -P $E/$n && pwd)" $E/$n.sh $ST/pint40sdk ${i[@]+"${i[@]}"} -- $B/build-extras.sh $n
}
for n in ${P[@]+"${P[@]}"}; do extra $n; done
barrier
for n in ${N[@]+"${N[@]}"}; do extra $n; done
barrier
