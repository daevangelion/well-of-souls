#!/bin/sh
# Unpack assets/WellOfSouls.exe (Clickteam Install Creator) into extracted/ using cicdec 3.0.1 under mono.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/extracted"}
[ -f "$out/Souls.exe" ] && exit 0
cic="$root/work/cicbin/cicdec.exe"
if [ ! -f "$cic" ]; then
    mkdir -p "$root/work/cicbin"
    curl -sSL -o "$root/work/cicdec.zip" https://github.com/Bioruebe/cicdec/releases/download/3.0.1/cicdec.zip
    (cd "$root/work/cicbin" && 7z x -y ../cicdec.zip >/dev/null)
fi
mono "$cic" "$root/assets/WellOfSouls.exe" "$out" >/dev/null
test -f "$out/Souls.exe"
