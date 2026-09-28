#!/bin/sh
# Editor round-trip acceptance: drives a replay that places a link and a monster (and paints one
# terrain cell) in a COPY of the retail world, then byte-diffs the written .obl/.mon/.ter
# against extracted/ to prove the record formats.
#
# extracted/ is never modified: the whole data tree is copied under a temp dir first and the
# game is pointed at the copy with --data.
#
#   tests/editors_replay.sh            (uses build/ or $WOS_BUILD)
#   WOS_BUILD=/mnt/build/wos-X tests/editors_replay.sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=${WOS_BUILD:-"$root/build"}
data=${WOS_DATA:-"$root/extracted"}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

[ -x "$build/wos" ] || { cmake -S "$root" -B "$build" >/dev/null && cmake --build "$build" -j4 >/dev/null; }

cp -r "$data" "$work/data"
mkdir -p "$work/save"
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}

status=0
"$build/wos" --data "$work/data" --save "$work/save" --headless --seed 1 --max-frames 200000 \
    --replay "$root/tests/replay/editors.rpl" --log "$work/events.log" >"$work/stdout.log" 2>&1 || status=$?
cat "$work/events.log"
if [ "$status" -ne 0 ]; then
    echo "FAIL: wos exited with $status"
    cat "$work/stdout.log"
    exit 1
fi
for ev in editor_open editor_link_add editor_monster editor_terrain editor_save editor_screen; do
    if ! grep -q "^EVT $ev\b" "$work/events.log"; then
        echo "FAIL: missing EVT $ev"
        exit 1
    fi
done

# The link index the editor claimed and the monster index it claimed, straight from the log.
link_index=$(sed -n 's/^EVT editor_link_add index=\([0-9-]*\).*/\1/p' "$work/events.log" | head -n 1)
mon_index=$(sed -n 's/^EVT editor_monster index=\([0-9-]*\) .*/\1/p' "$work/events.log" | head -n 1)
echo "edited .obl record $link_index, .mon record $mon_index"

fail=0
python3 - "$data/worlds/Evergreen/maps" "$work/data/worlds/Evergreen/maps" "$link_index" "$mon_index" <<'PY' || fail=1
import sys
orig_dir, new_dir, link_index, mon_index = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

def check(name, record, edited):
    a = open("%s/evergreen.%s" % (orig_dir, name), "rb").read()
    b = open("%s/evergreen.%s" % (new_dir, name), "rb").read()
    if len(a) != len(b):
        print("FAIL: %s size %d -> %d" % (name, len(a), len(b))); return False
    if len(a) % record:
        print("FAIL: %s is not a multiple of %d" % (name, record)); return False
    changed = []
    for r in range(len(a) // record):
        if a[r*record:(r+1)*record] != b[r*record:(r+1)*record]:
            changed.append(r)
    print("%s: %d of %d records differ: %s" % (name, len(changed), len(a)//record, changed))
    if edited not in changed:
        print("FAIL: %s: the edited record %d is not in the changed set" % (name, edited))
        return False
    # Everything that changed outside the edited record must be the runtime scratch the original
    # also leaves in memory: the link cursor distance (0x190) and the monster instance pointer
    # (0x10), which FUN_00464461 zeroes on load.
    for r in changed:
        if r == edited:
            continue
        ra = a[r*record:(r+1)*record]; rb = b[r*record:(r+1)*record]
        for k in range(record):
            if ra[k] == rb[k]:
                continue
            if name == "obl" and 400 <= k < 404:
                continue
            if name == "mon" and 16 <= k < 20:
                continue
            print("FAIL: %s record %d byte %d changed outside the scratch fields" % (name, r, k))
            return False
    return True

ok = True
if not check("obl", 800, link_index): ok = False
if not check("mon", 276, mon_index): ok = False
# The terrain DIB: the header (54 bytes) and the 256-entry palette must be untouched, and only
# cell bytes inside the pixel array may move. The editor paints the whole brush square
# (FUN_004642c7 per cell, FUN_004643AA sizes 0x20/0x30/0x40), so more than one byte is expected.
import struct
a = open("%s/evergreen.ter" % orig_dir, "rb").read()
b = open("%s/evergreen.ter" % new_dir, "rb").read()
if len(a) != len(b):
    print("FAIL: ter size %d -> %d" % (len(a), len(b))); ok = False
else:
    off = struct.unpack("<I", a[10:14])[0]
    w = struct.unpack("<i", a[18:22])[0]
    h = struct.unpack("<i", a[22:26])[0]
    stride = (w + 3) & ~3
    cells = set()
    bad = 0
    for i in range(len(a)):
        if a[i] == b[i]:
            continue
        if i < off:
            print("FAIL: ter header byte %d changed" % i); bad = 1; break
        rel = i - off
        cells.add((rel % stride) + (h - 1 - rel // stride) * w)
    n = len(cells)
    print("ter: %d bytes differ, %d distinct cells of %dx%d (palette and header intact)"
          % (sum(1 for i in range(len(a)) if a[i] != b[i]), n, w, h))
    if bad or n == 0:
        ok = False
    else:
        xs = sorted(set(c % w for c in cells)); ys = sorted(set(c // w for c in cells))
        # the brush paints a solid square, so every cell of the bounding box must have moved
        want = (max(xs) - min(xs) + 1) * (max(ys) - min(ys) + 1)
        if n != want:
            print("FAIL: %d cells changed but the bounding box holds %d" % (n, want)); ok = False
sys.exit(0 if ok else 1)
PY
[ "$fail" -eq 0 ] || { echo "FAIL: byte diff"; exit 1; }
echo "PASS: editor round-trip touches only the edited records"
