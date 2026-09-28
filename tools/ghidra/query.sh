#!/bin/sh
# Query the analysed Souls.exe Ghidra project headlessly (no GUI, no omp ghidra device).
#   tools/ghidra/query.sh refs    0x5078a4 0x40   # every reference into a range
#   tools/ghidra/query.sh writes  0x5078a4 0x3c60 # WRITE references only
#   tools/ghidra/query.sh callers FUN_0046fcc4
#   tools/ghidra/query.sh decomp  0x494fcd
#   tools/ghidra/query.sh disasm  FUN_0046fcc4
#   tools/ghidra/query.sh func    0x46fd10
# Each call copies the project to a private dir on /mnt/build, so parallel callers never
# fight over Ghidra's project lock. Takes about 20-40 s (JVM start + program open).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
ghidra=${GHIDRA_HOME:-/opt/ghidra_12.1.3_PUBLIC}
proj="$root/work/ghidra_proj"
[ -f "$proj/souls.gpr" ] || { echo "no project at $proj; run tools/ghidra/DumpDecomp first" >&2; exit 1; }
tmp=$(mktemp -d /mnt/build/ghidra-q.XXXXXX)
trap 'rm -rf "$tmp"' EXIT INT TERM
cp -a "$proj/." "$tmp/"
"$ghidra/support/analyzeHeadless" "$tmp" souls -process Souls.exe -noanalysis -readOnly \
    -scriptPath "$root/tools/ghidra" -postScript Query.java "$@" 2>&1 \
    | sed -n 's/^.*Q| //p'
