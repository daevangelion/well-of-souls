#!/usr/bin/env bash
# tests/diff_oracle.sh -- run every tests/diff/*.dsc on BOTH sides and diff per label.
#
#   original : tools/oracle/run.sh <dsc> <tmp>/oracle/<name>/
#   port     : $WOS_BUILD/wos --data <copy> --save <tmp>/save --headless \
#                --script <dsc> --dump <tmp>/port/<name>/dump
#
# For every `dump <label>` the oracle wrote, the port's dump for the same label is
# compared key by key with tools/oracle/cmp_dump.py, which masks the volatile byte ranges
# of the hero record (tools/oracle/hero_mask.txt) and reports the first differing keys.
# rng.* keys are compared first and reported first: if the LCG diverged, nothing else in
# the dump means anything.
#
# Exits non-zero if any label mismatches.  A label the port did not produce is a mismatch,
# not a skip: a silently missing dump is the failure mode that hides a regression.
#
# Environment:
#   WOS_BUILD   port build dir (default /mnt/build/wos-oracle-port)
#   WOS_ORACLE_TIMEOUT   seconds allowed per original run (default 180)
#   DIFF_KEEP=1  keep the per-run output directories
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BUILD="${WOS_BUILD:-/mnt/build/wos-oracle-port}"
PORT="$BUILD/wos"
TMP="${DIFF_TMP:-/mnt/build/wos-diff}"
MASK="$REPO/tools/oracle/hero_mask.txt"
CMP="$REPO/tools/oracle/cmp_dump.py"

if [ ! -x "$PORT" ]; then
    echo "diff_oracle: no port binary at $PORT" >&2
    echo "diff_oracle: build it, or set WOS_BUILD" >&2
    exit 2
fi
if [ ! -d "$REPO/tests/diff" ]; then
    echo "diff_oracle: tests/diff does not exist" >&2
    exit 2
fi

rm -rf "$TMP"
mkdir -p "$TMP"

# The port writes into its own data copy and its own save tree, never the repo's
# extracted/ and never the oracle's game directory: the two sides must not be able to
# see each other's saves.
PORT_DATA="$TMP/data"
mkdir -p "$PORT_DATA"
cp -a "$REPO/extracted/." "$PORT_DATA/"

total=0; ok=0; bad=0; skipped=0
declare -a SUMMARY=()

for dsc in "$REPO"/tests/diff/*.dsc; do
    [ -e "$dsc" ] || { echo "diff_oracle: no scripts in tests/diff" >&2; exit 2; }
    name=$(basename "$dsc" .dsc)
    printf '=== %s\n' "$name"

    odir="$TMP/oracle/$name"; pdir="$TMP/port/$name"
    mkdir -p "$odir" "$pdir" "$TMP/save/$name"

    if ! WOS_ORACLE_TIMEOUT="${WOS_ORACLE_TIMEOUT:-180}" \
            "$REPO/tools/oracle/run.sh" "$dsc" "$odir" >"$odir/run.log" 2>&1; then
        echo "  ORACLE FAILED (see $odir/run.log and ${WOS_LOG:-/tmp/wos-oracle-hook.log})"
        SUMMARY+=("$name: oracle-run FAILED")
        bad=$((bad+1)); total=$((total+1))
        continue
    fi

    # the port gets a pristine save tree per script
    rm -rf "$TMP/save/$name"; mkdir -p "$TMP/save/$name"
    ( cd "$PORT_DATA" && \
      SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      "$PORT" --data "$PORT_DATA" --save "$TMP/save/$name" --headless \
              --script "$dsc" --dump "$pdir/dump" ) >"$pdir/run.log" 2>&1
    prc=$?
    if [ $prc -ne 0 ]; then
        echo "  PORT EXIT $prc (see $pdir/run.log)"
    fi

    for of in "$odir"/*.txt; do
        [ -e "$of" ] || continue
        label=$(basename "$of" .txt)
        total=$((total+1))
        # the port writes <prefix>.<label>.txt; fall back to a single combined dump
        pf="$pdir/dump.$label.txt"
        [ -f "$pf" ] || pf="$pdir/dump"
        if [ ! -f "$pf" ]; then
            echo "  $label: MISSING (port wrote no dump; see $pdir/run.log)"
            SUMMARY+=("$name/$label: MISSING")
            bad=$((bad+1)); continue
        fi
        if python3 "$CMP" "$of" "$pf" "$MASK"; then
            ok=$((ok+1))
            SUMMARY+=("$name/$label: OK")
        else
            bad=$((bad+1))
            SUMMARY+=("$name/$label: MISMATCH")
        fi
    done
done

echo
echo "================= diff_oracle summary ================="
for s in "${SUMMARY[@]}"; do echo "  $s"; done
echo "--------------------------------------------------------"
printf 'labels=%d  equal=%d  mismatched=%d' "$total" "$ok" "$bad"
[ $skipped -gt 0 ] && printf '  skipped=%d' "$skipped"
echo
[ "${DIFF_KEEP:-0}" = 1 ] || true
echo "per-run output under $TMP (set DIFF_KEEP=1 to keep it after the run)"
[ "$bad" -eq 0 ]
