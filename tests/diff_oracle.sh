#!/usr/bin/env bash
# tests/diff_oracle.sh -- run every tests/diff/*.dsc on BOTH sides and diff per label.
#
#   original : tools/oracle/run.sh <dsc> <tmp>/oracle/<name>/
#              one <label>.txt per `dump <label>`, in the port's key vocabulary
#   port     : $WOS_BUILD/wos --data <copy> --save <tmp>/save --headless
#                --script <dsc> --log <tmp>/port/<name>/log
#              the port answers a `dump <label>` into its EVENT LOG, not into a
#              file, so this script splits the log back into <label>.txt with
#              tools/oracle/split_port_log.py -- one file per label, same shape
#              as the oracle's, which is the only way the two are comparable.
#
# For every label the oracle wrote, cmp_dump.py compares the keys both sides
# have, masks the volatile byte ranges of the hero record
# (tools/oracle/hero_mask.txt), and prints the first differing keys.  rng.* is
# reported first: if the LCG diverged, nothing else in the dump means anything.
#
# A label the port did not produce is a MISMATCH, not a skip: a silently missing
# dump is the failure mode that hides a regression.
#
# Environment:
#   WOS_BUILD            port build dir (default /mnt/build/wos-main)
#   WOS_ORACLE_TIMEOUT   seconds allowed per original run (default 180)
#   DIFF_ONLY=<name>     run just one script
#   DIFF_PORT_DUMP       refused: see the --dump note at the port invocation. A --dump
#                        file is end-of-run state, not per-op, and comparing it would
#                        report agreement for labels that never held it.
#   DIFF_TMP=<dir>       scratch (default /mnt/build/wos-diff)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BUILD="${WOS_BUILD:-/mnt/build/wos-main}"
PORT="$BUILD/wos"
TMP="${DIFF_TMP:-/mnt/build/wos-diff}"
MASK="$REPO/tools/oracle/hero_mask.txt"
CMP="$REPO/tools/oracle/cmp_dump.py"
SPLIT="$REPO/tools/oracle/split_port_log.py"

if [ ! -x "$PORT" ]; then
    echo "diff_oracle: no port binary at $PORT" >&2
    echo "diff_oracle: build it, or set WOS_BUILD" >&2
    exit 2
fi
if [ ! -d "$REPO/tests/diff" ] || ! ls "$REPO"/tests/diff/*.dsc >/dev/null 2>&1; then
    echo "diff_oracle: no scripts in tests/diff" >&2
    exit 2
fi

rm -rf "$TMP"
mkdir -p "$TMP"

# The port writes into its own data copy and its own save tree, never the repo's
# extracted/ and never the oracle's game directory: the two sides must not be
# able to see each other's saves.
PORT_DATA="$TMP/data"
mkdir -p "$PORT_DATA"
cp -a "$REPO/extracted/." "$PORT_DATA/"

total=0; ok=0; bad=0
declare -a SUMMARY=()

for dsc in "$REPO"/tests/diff/*.dsc; do
    name=$(basename "$dsc" .dsc)
    if [ -n "${DIFF_ONLY:-}" ] && [ "$DIFF_ONLY" != "$name" ]; then continue; fi
    printf '=== %s\n' "$name"

    odir="$TMP/oracle/$name"; pdir="$TMP/port/$name"
    mkdir -p "$odir" "$pdir" "$TMP/save/$name"

    if ! WOS_ORACLE_TIMEOUT="${WOS_ORACLE_TIMEOUT:-180}" \
            "$REPO/tools/oracle/run.sh" "$dsc" "$odir" >"$odir/run.log" 2>&1; then
        echo "  ORACLE FAILED (see $odir/run.log)"
        tail -5 "$odir/run.log" | sed 's/^/    /'
        SUMMARY+=("$name: oracle-run FAILED")
        bad=$((bad+1))
        continue
    fi

    # the port gets a pristine save tree per script
    rm -rf "$TMP/save/$name"; mkdir -p "$TMP/save/$name"
    # --time is PINNED, not defaulted. The incarnation time goes into the hero record
    # and therefore into the .her checksum, so a port run at the host's wall clock can
    # never match the original even when every rule agrees. The oracle's epoch is
    # VIRTUAL_EPOCH = 1234567890 (docs/re/oracle.md section 2), and the two must be the
    # same number or the comparison is not a comparison.
    ( cd "$PORT_DATA" && \
      SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      "$PORT" --data "$PORT_DATA" --save "$TMP/save/$name" --headless \
              --script "$dsc" --log "$pdir/log" --time 1234567890 ) >"$pdir/run.log" 2>&1
    # --dump IS DELIBERATELY ABSENT, and that matters more than it looks.
    # src/game_main.c's run_dumps() writes the whole dump table ONCE, at exit, so a
    # --dump FILE holds the END-OF-RUN state for every label in it: a per-label
    # comparison against it would silently compare the final state N times and report
    # agreement for labels that never held it. The per-op values come from the event
    # log instead -- dump_one() emits each key through log_emit as `EVT <key> <key>=<v>`
    # preceded by `EVT dump label=<name>`, and split_port_log.py reassembles them. So the
    # numbers in a diff are per-op by construction, and a run that passed --dump would
    # make every clock.* and rng.* value identical across labels. Guarded, not assumed.
    prc=$?
    [ $prc -ne 0 ] && echo "  PORT EXIT $prc (see $pdir/run.log)"
    if [ -n "${DIFF_PORT_DUMP:-}" ]; then
        echo "  FATAL: DIFF_PORT_DUMP is set; this suite compares per-op LOG values, and a"
        echo "  --dump file holds end-of-run state for every label. Unset it."
        exit 2
    fi
    python3 "$SPLIT" "$pdir/log" "$pdir" || echo "  PORT LOG SPLIT FAILED"

    for of in "$odir"/*.txt; do
        [ -e "$of" ] || continue
        label=$(basename "$of" .txt)
        total=$((total+1))
        pf="$pdir/$label.txt"
        if [ ! -f "$pf" ]; then
            echo "  $label: MISSING (the port emitted no dump with this label)"
            SUMMARY+=("$name/$label: MISSING")
            bad=$((bad+1))
            continue
        fi
        if python3 "$CMP" "$of" "$pf" "$MASK"; then
            ok=$((ok+1))
            SUMMARY+=("$name/$label: OK")
        else
            bad=$((bad+1))
            SUMMARY+=("$name/$label: MISMATCH")
        fi
    done

    # A label the ORACLE produced but the port did not is caught above (MISSING). The
    # reverse is not: a dump the port emitted and the oracle never reached would be
    # invisible, and an unreachable target is exactly the failure this suite exists to
    # catch. So the two label sets are compared both ways, explicitly.
    for pf in "$pdir"/*.txt; do
        [ -e "$pf" ] || continue
        label=$(basename "$pf" .txt)
        if [ ! -f "$odir/$label.txt" ]; then
            echo "  $label: PORT-ONLY (the original never produced this label)"
            SUMMARY+=("$name/$label: PORT-ONLY")
            bad=$((bad+1))
        fi
    done
done

echo
echo "================= diff_oracle summary ================="
for s in "${SUMMARY[@]}"; do echo "  $s"; done
echo "--------------------------------------------------------"
printf 'labels=%d  equal=%d  mismatched=%d\n' "$total" "$ok" "$bad"
echo "per-run output under $TMP"
[ "$bad" -eq 0 ]
