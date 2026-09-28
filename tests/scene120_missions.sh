#!/bin/sh
# Evergreen scene 120 (The Blue Cavern Tavern) MISSION(S) replay.
#
# The route is verified as far as the walk: boot -> Piano-User (START_LOCATION 2,13,1) -> map 2
# loads -> scene 221 -> back on map 2. It is BLOCKED on the walk to link 20, because the port has
# no pathfinding: click_walk()/walk_to() accept only a straight leg and refuse anything that
# crosses terrain with `EVT path_none`, and the tavern is five turns away from link 13. See the
# replay header for the decoded terrain and the BFS route. This runner exists so the replay has one
# command and so the blocker is reported rather than silent; it exits 2 until mapview.c can path,
# at which point rename the replay to tests/replay/gap_missions.rpl and replay_gaps.sh picks it up.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=${WOS_BUILD:-"$root/build"}
data=${WOS_DATA:-"$root/extracted"}
shots=${WOS_SHOTS:-"$root/work/shots"}

[ -x "$build/wos" ] || { cmake -S "$root" -B "$build" >/dev/null && cmake --build "$build" -j4 >/dev/null; }
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$shots" "$work/save"

"$build/wos" --data "$data" --save "$work/save" --headless --seed 1 \
    --max-frames 200000 --replay "$root/tests/replay/scene120_missions.rpl" \
    --log "$work/events.log" >"$work/stdout.log" 2>&1 || true

fail=0
if grep -q "^EVT map_error map=2 reason=load" "$work/events.log"; then
    echo "BLOCKED scene120: map 2 will not load"
    grep -h "cannot load map 2" "$work/stdout.log" || true
    exit 2
fi
if grep -q "^EVT scene_enter scene=120$" "$work/events.log"; then :; else
    echo "BLOCKED scene120: reached map 2 but not the tavern link - the walk needs pathfinding (see the replay header)"
    grep -E "^EVT (map_enter|scene_enter|path_none)" "$work/events.log" | tail -4
    exit 2
fi
for ev in map_enter scene_enter missions_loaded missions_offer missions_panel; do
    grep -q "^EVT $ev\b" "$work/events.log" || { echo "FAIL missing $ev"; fail=1; }
done
grep -q "^EVT scene_enter scene=120$" "$work/events.log" || { echo "FAIL scene 120 not entered"; fail=1; }
grep -q "^EVT scene_op .*op=MISSIONS\b" "$work/events.log" || { echo "FAIL MISSIONS opcode"; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS scene120: MISSIONS 1..20 armed the Mission button in The Blue Cavern Tavern"
exit $fail
