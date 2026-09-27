#!/bin/sh
# Gap-closure acceptance: runs every tests/replay/gap_*.rpl headless with a fresh save dir.
# Each replay declares in its header:
#   # seed: N                      (default 1)
#   # requires: evt1 evt2 ...     (each must appear as "EVT <name>" in the log)
#   # requires-match: REGEX       (repeatable; some log line must match "^EVT REGEX", grep -E)
# A replay passes when wos exits 0 (all `expect`s met) and every required event was logged.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=${WOS_BUILD:-"$root/build"}
data=${WOS_DATA:-"$root/extracted"}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$root/tools/extract_installer.sh" "$data"
[ -x "$build/wos" ] || { cmake -S "$root" -B "$build" >/dev/null && cmake --build "$build" -j4 >/dev/null; }
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}

fail=0
count=0
for rpl in "$root"/tests/replay/gap_*.rpl; do
    [ -f "$rpl" ] || continue
    count=$((count + 1))
    name=$(basename "$rpl" .rpl)
    seed=$(sed -n 's/^# seed: *\([0-9][0-9]*\).*/\1/p' "$rpl" | head -n 1)
    requires=$(sed -n 's/^# requires: *//p' "$rpl" | head -n 1)
    mkdir -p "$work/$name/save"
    status=0
    "$build/wos" --data "$data" --save "$work/$name/save" --headless --seed "${seed:-1}" \
        --max-frames 200000 --replay "$rpl" --log "$work/$name/events.log" \
        >"$work/$name/stdout.log" 2>&1 || status=$?
    missing=
    for ev in $requires; do
        grep -q "^EVT $ev\b" "$work/$name/events.log" || missing="$missing $ev"
    done
    patterns=$(sed -n 's/^# requires-match: *//p' "$rpl")
    if [ -n "$patterns" ]; then
        while IFS= read -r pat; do
            grep -Eq "^EVT $pat" "$work/$name/events.log" || missing="$missing [$pat]"
        done <<EOF
$patterns
EOF
    fi
    if [ "$status" -ne 0 ] || [ -n "$missing" ]; then
        echo "FAIL $name: exit=$status missing:${missing:- none}"
        tail -n 5 "$work/$name/stdout.log"
        fail=1
    else
        echo "PASS $name: $requires $patterns" | tr '\n' ' '; echo
    fi
done
[ "$count" -gt 0 ] || { echo "FAIL: no gap replays"; exit 1; }
exit $fail
