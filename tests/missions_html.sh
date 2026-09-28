#!/bin/sh
# MISSION/MISSIONS + HTML viewer acceptance.
#
# Evergreen scene 120 (the Blue Cavern Tavern) and scene 2000 both use the mission opcode, but
# scene 120 is only reachable from map 2 "Macgyver Castle" and retail ships no castle1.mon, so
# map 2 cannot be entered; scene 2000 has no .obl link at all. The same opcode, the same VM and
# the same mission table are therefore exercised through tests/probeworld, a probe world that
# reuses Evergreen's tables and assets verbatim and puts the same script in scene 3, which IS
# map 0 link 0 ("Gateway of Dreams") - the link the Well spawn walks into. Its scene script is
# the shape of QuestScenes120.txt: MISSIONS 1,2,3 then HTML.
#
# Covers: the 0x4A opcode arming the scene button, the missions.ini table, the Qualify
# condition, the picker's control ids, accept + the per-hero .mis write, the 0x29 opcode
# suspending the scene, link following inside the world HTML folder, and closing.
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

"$build/wos" --data "$root/tests/probeworld" --save "$work/save" --headless --seed 1 \
    --max-frames 200000 --replay "$root/tests/replay/probe_missions.rpl" \
    --log "$work/events.log" >"$work/stdout.log" 2>&1
status=$?

fail=0
[ "$status" -eq 0 ] || { echo "FAIL probe_missions: exit=$status"; fail=1; }
for ev in missions_loaded missions_offer mission_status mission_accept \
          html_open html_link html_close missions_panel; do
    grep -q "^EVT $ev\b" "$work/events.log" || { echo "FAIL missing $ev"; fail=1; }
done
grep -q "^EVT scene_op .*op=MISSIONS\b" "$work/events.log" || { echo "FAIL MISSIONS opcode"; fail=1; }
grep -q "^EVT scene_op .*op=HTML\b" "$work/events.log"     || { echo "FAIL HTML opcode"; fail=1; }
# the mission log the original keeps beside the .wsh: <save>/<world>/savedHeroes/<name>.mis
grep -q "^\[1\]" "$work/save/Probe/savedHeroes/Probe.mis" || { echo "FAIL .mis section"; fail=1; }
grep -q "^Status=1" "$work/save/Probe/savedHeroes/Probe.mis" || { echo "FAIL .mis Status"; fail=1; }
for shot in html html2 missions accepted; do
    [ -s "$shots/wos-probe-$shot.bmp" ] || { echo "FAIL screenshot $shot"; fail=1; }
done
[ "$fail" -eq 0 ] && echo "PASS probe_missions: MISSION(S) offer, missions.ini, picker, accept, HTML open/link/close"
exit $fail
