#!/usr/bin/env bash
# The programs that APPS names, for the system disk (sdcard/make-sd-image.sh): GitHub repositories,
# OWNER/NAME, separated by spaces. Of each, the newest release (no draft or pre-release) that has one
# file with pint35 in its name (build/release.pl), checked against the SHA-256 GitHub has of it, into
# obj/apps/NAME (NAME in upper case), for \APPS\NAME: a program (.exe) as NAME.EXE, or a .zip
# unpacked, without the one folder it may hold, with the names of its top level in upper case as on
# the rest of the disk. It is downloaded again when that release changes; when GitHub does not
# answer, the last one stays. A repository that PINT downloads already (an extra, extras/*.sh) is
# left out, and so is a NAME that PINT has in \APPS.
# obj/apps/list: a line NAME|ITEM=PROGRAM... for each, with an item in Program Manager for each
# program of Windows (not of the console) of its top level, named after the repository (the program
# of a release) or after the program (of a .zip). Without APPS, obj/apps goes.
#
# Usage:  APPS="OWNER/NAME ..." build/make-apps.sh
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
O=$R/obj/apps V=pint35
[ -n "${APPS:-}" ] || { rm -rf "$O"; exit 0; }
mkdir -p "$O"
trap 'rm -rf "$O"/*.new "$O"/*.tmp' EXIT

# gui EXE: whether EXE is a program of Windows (PE subsystem 2), not of the console
gui() {
    perl -e 'open F, "<", $ARGV[0] or exit 1; binmode F; read F, $h, 64;
             exit 1 if length $h < 64 || substr($h, 0, 2) ne "MZ";
             seek F, unpack("V", substr($h, 60, 4)), 0; read F, $p, 94;
             exit 1 if length $p < 94 || substr($p, 0, 4) ne "PE\0\0";
             exit(unpack("v", substr($p, 92, 2)) == 2 ? 0 : 1)' "$1"
}
okname() { [[ $1 =~ ^[A-Za-z0-9_.-]+$ ]]; }

# extra OWNER/NAME: the folder on the disk of the extra of that repository (extras/*.sh), which PINT downloads
# already (build/build-extras.sh)
extra() {
    awk -v a="$(echo "$1" | tr A-Z a-z)" 'FNR == 1 { r = s = "" }
        /^REPO=/ { r = tolower(substr($0, 6)) }
        /^DEST=/ { s = substr($0, 6) }
        r == a && s != "" { print s; exit }' "$R"/extras/*.sh
}

# get OWNER/NAME DIR: the file that $O/DIR.release.new names into $O/DIR, its items into $O/DIR.items
get() {
    local a=$1 d=$2 n=${1#*/} t=$O/$2.tmp u s f p b z
    read -r u s <<< "$(awk '$1 == "file" { print $3, $4 }' "$O/$d.release.new")"
    f=${u##*/}
    echo "   APPS: $a $(sed -n 's/^tag //p' "$O/$d.release.new"): $f"
    rm -rf "$t" && mkdir -p "$t/x" "$t/z"
    curl -fsSL --retry 3 -o "$t/f" "$u" || { echo "!! APPS: cannot download $u"; exit 1; }
    [ -z "$s" ] || [ "$(shasum -a 256 "$t/f" | cut -d" " -f1)" = "$s" ] || { echo "!! APPS: $u is not the file GitHub has (SHA-256)"; exit 1; }
    : > "$t/items"
    case $(echo "$f" | tr A-Z a-z) in
    *.exe)
        cp "$t/f" "$t/x/$d.EXE"
        if gui "$t/x/$d.EXE" && okname "$n"; then printf "%s " "$n=$d" > "$t/items"; fi ;;
    *.zip)
        unzip -qo "$t/f" -d "$t/z"
        z=$t/z; set -- "$t"/z/*; if [ $# = 1 ] && [ -d "$1" ]; then z=$1; fi
        for p in "$z"/*; do
            [ -e "$p" ] || continue
            b=${p##*/}
            case $(echo "$b" | tr A-Z a-z) in
            *.exe) if gui "$p" && okname "${b%.*}"; then printf "%s " "${b%.*}=${b%.*}" >> "$t/items"; fi ;;
            esac
            mv "$p" "$t/x/$(echo "$b" | tr a-z A-Z)"
        done ;;
    *)  echo "!! APPS: $f of $a is no program (.exe) or .zip"; exit 1 ;;
    esac
    rm -rf "$O/$d" && mv "$t/x" "$O/$d" && mv "$t/items" "$O/$d.items" && mv "$O/$d.release.new" "$O/$d.release"
    rm -rf "$t"
}

: > "$O/list.new"
keep=" "
for a in $APPS; do
    [[ $a =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || { echo "!! APPS: $a is no GitHub repository (OWNER/NAME)"; exit 1; }
    got=$(extra "$a")
    if [ -n "$got" ]; then echo "   APPS: $a: PINT has it already (\\$(echo "$got" | tr / '\\')), left out"; continue; fi
    d=$(echo "${a#*/}" | tr a-z A-Z)
    if [ -e "$R/sdcard/root/APPS/$d" ]; then echo "   APPS: $a: PINT has \\APPS\\$d already, left out"; continue; fi
    case $keep in *" $d "*) echo "!! APPS: two repositories named ${a#*/}"; exit 1 ;; esac
    keep="$keep$d "
    rc=0; GET="f=*$V*" perl "$R/build/release.pl" "$a" > "$O/$d.release.new" || rc=$?
    if [ $rc = 2 ]; then
        [ -f "$O/$d.items" ] || { echo "!! APPS: GitHub did not list releases of $a (no such repository, or no answer)"; exit 1; }
        echo "   APPS: GitHub did not answer, keeping $(sed -n 's/^tag //p' "$O/$d.release") of $a"
    elif [ $rc != 0 ]; then
        echo "!! APPS: no release of $a has one file with $V in its name"; exit 1
    elif ! { [ -f "$O/$d.items" ] && cmp -s "$O/$d.release.new" "$O/$d.release"; }; then
        get "$a" "$d"
    fi
    echo "$d|$(cat "$O/$d.items")" >> "$O/list.new"
done

# what APPS no longer names goes
for p in "$O"/*; do
    b=${p##*/}
    case $b in list|list.new) continue ;; *.release) b=${b%.release} ;; *.items) b=${b%.items} ;; esac
    case $keep in *" $b "*) ;; *) rm -rf "$p" ;; esac
done
mv "$O/list.new" "$O/list"
