#!/bin/sh
# Acceptance test: headless scripted play-through of offline solo play.
#   boot -> main menu -> create hero -> incarnate on map 0 -> walk -> random encounter -> win fight.
# Every step is asserted by `expect` in the replay (exit 2 on timeout) and re-checked in the log.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=${WOS_BUILD:-"$root/build"}
data=${WOS_DATA:-"$root/extracted"}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$root/tools/extract_installer.sh" "$data"
[ -x "$build/wos" ] || { cmake -S "$root" -B "$build" >/dev/null && cmake --build "$build" -j4 >/dev/null; }

mkdir -p "$work/save"
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
status=0
"$build/wos" --data "$data" --save "$work/save" --headless --seed 1 --max-frames 200000 \
    --replay "$root/tests/replay/offline.rpl" --log "$work/events.log" || status=$?
cat "$work/events.log"
if [ "$status" -ne 0 ]; then
    echo "FAIL: wos exited with $status"
    exit 1
fi
for ev in boot_menu hero_ready map_enter hero_move battle_start battle_won; do
    if ! grep -q "^EVT $ev\b" "$work/events.log"; then
        echo "FAIL: missing EVT $ev"
        exit 1
    fi
done
echo "PASS: offline play-through"
