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
# --script must deliver input to the screen update. A regression here made every
# front@* label in the diff suite report as 22 front-end mismatches when it was
# one broken input path: the script branch applied the op and then continued past
# screen->update(), so input_begin() cleared the edges on the next iteration.
si_log="$work/script_input.log"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$build/wos" --data "$data" --save "$work/save" \
    --headless --seed 1 --time 1234567890 --script "$root/tests/replay/script_input.dsc" \
    --log "$si_log" >/dev/null 2>&1
# A DEADLINE, not "eventually": the original leaves the title 230 ms after the click,
# so an assertion that merely waits would also pass a fix that delivers the input a
# frame late. The t=400 dump must already read front_state 1.
sed -n '/EVT front_state front_state=/p' "$si_log" >"$work/si_states" 2>/dev/null || true
if ! awk '/front@deadline/{seen=1} seen && /front_state=1/{ok=1} END{exit !ok}' "$si_log"; then
    echo "FAIL: --script input: front_state had not reached 1 by the t=400 deadline"
    echo "      (the original leaves the title 230 ms after the click)"
    grep -E 'front_state' "$si_log" || true
    exit 1
fi

echo "PASS: offline play-through"
