# Maps, movement and encounters (Souls.exe)

## Summary

Fully reverse-engineered the WoS world-map subsystem from `work/decomp/all.c`. Documented all four map file formats with exact byte layouts, the coordinate system, movement/pathfinding, and the random-encounter rule. Key findings: 1 low-res jpg pixel = 1 "map unit"; the X4 hi-res jpg is blitted 1:1 so 1 map unit = 4 screen px; `.ter` is a plain 8-bit BMP at (jpgW/4)x(jpgH/4) so one terrain cell = 4x4 map units, sharing the pathfinding grid; `.obl` = 256 raw 800-byte link records; `.mon` = 1000 raw 276-byte {monsterId,x,y,radius} records; walk speed is 3200/4800 units-per-ms (12.5/18.75 map units per second), doubled by equipment field 12; encounters are a per-frame Bernoulli roll `200*3/(bitlen(difficulty)+0)` per 10000 with a 5 s cooldown, resolved against `groups.txt` using the NEAREST link's signed difficulty level and the Manhattan distance to it.

## Architecture

The world-map subsystem is a self-contained MFC child window (the "map view") that owns: a low-res base map bitmap, an optional 4x hi-res bitmap, a 1/4-scale 8-bit terrain bitmap, a 256x800-byte link table, a 1000x276-byte monster-placement table and a 1000x48-byte object table (objects.obr). All world geometry is expressed in "map units" = one pixel of the LOW-RES mapName.jpg. Internal runtime positions are 24.8 fixed point (x<<8). The walk window is a fixed 256x256 screen-pixel rect; because the X4 hi-res image is blitted 1:1, one map unit = 4 screen pixels, and the visible area is 64x64 map units. The pathfinder works on a 4x4-map-unit grid. Camera is hard-centred on the hero. Random encounters are a per-timer-tick Bernoulli roll gated by the nearest link's difficulty level, resolved through groups.txt, plus a separate proximity roll for .mon-placed monsters.

# Well of Souls — World Maps & Movement (offline solo reimplementation reference)

All addresses are VAs from `work/decomp/all.c`. `DAT_xxxx` names are Ghidra's global labels. Struct offsets are byte offsets from the start of the record.

---

## 0. The one number you must internalise: map units

Everything on the world map — terrain cells, link positions, monster positions, path nodes, camera — is expressed in **"map units" = one pixel of the LOW-RES `mapName.jpg`**.

Runtime actor positions are **24.8 fixed point** (`x` stored as `mapunit << 8`); the `>>8` you see everywhere is the conversion back to map units. Proof: `FUN_004620f3` (0x4620f3) is handed `param_2 = DAT_00679e2c[..] << 8` and immediately does `FUN_004631c6(param_2 >> 8, param_3 >> 8)`; the click handler `FUN_0041f247` (0x41f247) does `... * 0x100` to build the fixed-point target.

The walk window is a hard-coded **256x256 screen-pixel** rect: `all.c:20960`
```c
SetRect((LPRECT)(param_1 + 0x3508), local_34.right + -0x100, local_34.top,
        local_34.right, local_34.top + 0x100);
```
The X4 hi-res image is blitted **1:1** (no scaling), so **1 map unit = 4 screen pixels** and the visible area is 64x64 map units.

---

## 1. `maps.txt` / the `+MAPS` table

Parsed by `FUN_00482c54` (0x482c54, all.c:95837). One record per map, **256 bytes (0x40 dwords)**, array base `DAT_00ce2b58`, indices 0..999 (`all.c:95870` rejects `id > 999`). Layout:

| off | field | notes |
|----|-------|-------|
| +0x00 | `used` | 1 = defined |
| +0x04 | `imageName[64]` | e.g. `"evergreen.jpg"` — `strncpy` 0x40 bytes (`all.c:95881`) |
| +0x45 | `rootName[64]` | e.g. `"evergreen"` — base for `.obl`/`.ter`/`.mon`/MIDI (`all.c:95883`) |
| +0x86 | `mapName[64]` | shown on map change + minimap (`all.c:95885`) |
| +0xF8 | `mapFlags` u32 | `strtoul(s,0,0)` → decimal **or `0x`-hex** (`all.c:95889-95891`) |
| +0xFC | `soundTheme` | 0..255 (`all.c:95893-95894`) |

Columns therefore are: `id, imageFile, rootName, "Map Name", flags, [theme]`. Quest doc is `extracted/worlds/Evergreen/quest.txt:218-295`; the data lives in `extracted/worlds/Evergreen/maps.txt` (18 rows, ids 0..17). Evergreen's row 0 is `0, evergreen.jpg, evergreen, "Evergreen", 131072` — flag `0x20000 = MAP_FLAG_NO_PKREZ`.

Duplicate detection: the parser cross-checks that no two maps share imageName / rootName / mapName (`all.c:95899-95928`).

### Map-file naming
`FUN_0041e421` (0x41e421, all.c:22999) is the map loader. It sets the search path to `worlds\<world>\maps\` (`s__s_worlds__s_maps_004e25c8`) and then:

```
root      = "<mapdir>" + map.rootName            // DAT_004e0ac8
sprintf(DAT_004e0ee8, "%s%s.mon", root)  -> FUN_00464461()   // .mon  (all.c:23295-23296)
sprintf(DAT_004e0de0, "%s%s.ter", root)  -> FUN_00486690()   // .ter  (all.c:23299)
sprintf(buf,         "%s%s.obl", root)  -> FUN_00463989()   // .obl  (all.c:23286)
sprintf(buf,         "%s%sX4.jpg",root)  -> _access() test   // all.c:23238
objects.bmp else objects.jpg -> FUN_00486690()               // all.c:23160-23166
objects.obr -> FUN_00463630()                                // all.c:23167
```
The OBL and TER must share the root name (quest.txt:229-232 says this explicitly).

`FUN_00486690` (0x486690) is the generic image loader: it sniffs the magic (`FUN_004894c0` → 1=BMP/RLE, 2=JPEG) and dispatches to `FUN_00488080` / `FUN_00488510`. So `.ter` really is a Windows BMP; nothing special about it.

---

## 2. `.jpg` / `X4.jpg` background images and their alignment

* `mapName.jpg` — low-res. **Its pixel dimensions define the map size.**
* `mapNameX4.jpg` — optional, must be *exactly* 4x each dimension (readme.txt). If `_access()` succeeds it is loaded and used 1:1; otherwise the low-res is stretched 4x as a fallback (`all.c:23238-23285`).

Map size extraction (`all.c:23221-23227`):
```c
if (DAT_004e09b8[1] == 0) DAT_005494b8 = 0; else DAT_005494b8 = *(int*)(DAT_004e09b8[1] + 4);
DAT_00549990 = FUN_004864c0();
DAT_004df8ac = (DAT_005494b8 + 3) / 4;   // terrain grid width  == jpgW/4
DAT_004df8b0 = (DAT_00549990 + 3) / 4;   // terrain grid height == jpgH/4
DAT_00539388  = DAT_005494b8 / 2;        // initial camera centre
DAT_00539384  = DAT_00549990 / 2;
```
`DAT_004e09b8` = low-res `CBitmap*`, `DAT_004e09bc` = hi-res `CBitmap*` (24 bytes apart, `all.c:23213/23218`). `FUN_004864c0` (0x4864c0) returns `abs(biHeight)` of a `BITMAPINFOHEADER`.

**Scale factor, stated plainly:**
```
1 jpg pixel      = 1 map unit
1 map unit       = 4 screen pixels (X4 hi-res drawn 1:1)
1 terrain cell   = 4 x 4 map units = 16 x 16 screen pixels
1 path node      = 4 x 4 map units (same grid as terrain, aligned)
```
Terrain grid and pathfinding grid are the **same** grid — see §5.

The low-res jpg is also the **minimap** source (upper-right MAP window); the walk view only uses it when no X4 file exists.

---

## 3. `.ter` — terrain bitmap

**Format:** plain 8-bit palettised Windows BMP, uncompressed, bottom-up.
**Dimensions:** `(jpgW+3)/4 x (jpgH+3)/4`.

Verification: `extracted/worlds/Evergreen/maps/evergreen.jpg` renders 768x768; `evergreen.ter` is 37942 bytes = `14 + 40 + 1024 (256*RGBQUAD) + 192*192`. 192 = 768/4. Exact.

The runtime never re-parses the BMP — `FUN_00486690` produces a `CBitmap` (`FUN_004864e0`, 0x4864e0, builds a `BITMAPINFOHEADER` with `biBitCount = 8` at +0xE and creates a DIB section) and the terrain pixels are read straight out of the DIB bits pointer at `DAT_00549b80`. Globals: `DAT_00549b7c` = the `BITMAPINFOHEADER*`, `DAT_00549b80` = pixel bits. Height comes from `FUN_004864c0`, stride from `(width+3)&~3`.

### Pixel accessor — `FUN_0046186f` (0x46186f, all.c:70089)
```c
undefined1 FUN_0046186f(int x, int y) {      // x,y in MAP UNITS
  x >>= 2; y >>= 2;                          // -> terrain cell
  if (x < 0 || y < 0 || x >= DAT_004df8ac || y >= DAT_004df8b0) return 9;   // off-grid == 9 "Impassable"
  stride = (DAT_00549b7c->biWidth + 3) & ~3u;
  return bits[(height - y - 1) * stride + x];   // BMP rows are bottom-up
}
```
So it is **one byte per 4x4-map-unit cell**, bottom-up rows, `9` returned for out-of-range (matching terrain id 9 = "Impassable").

### What the byte values mean
The palette + semantics come from the `+TERRAINS` section (`quest.txt:124-148`), parsed by `FUN_00481f8a` (0x481f8a, all.c:95215) into a 10-entry table at `DAT_005ea618` (stride 0x45 dwords = 276 B, same shape as a monster record):

| off | field |
|-----|-------|
| +0x00 | `used` |
| +0x04 | `name[0x104]` (editor palette label) |
| +0x108 (`&DAT_005ea720`) | token number (0 = none) |
| +0x10C (`&DAT_005ea724`) | damage (quest.txt says "not yet implemented") |
| +0x110 (`&DAT_005ea728`) | **required item / token id** |

Evergreen: `0 No Hindrance`, `1 Lava`, `2 Electricity`, `3 -`, `4 Deep Water`, `5 Clouds`, `6/7 -`, `8 Healing`, `9 Impassable`.

### Walkability — `FUN_004631c6` (0x4631c6, all.c:71300)
```c
if (x < 0 || y < 0 || x >= DAT_005494b8 || y >= DAT_00549990) return 0;   // off-map
int t = FUN_0046186f(x, y);                                               // 0..9 / 255
bool special = (t < 10 && DAT_005ea728[t*0x45] != 0
                && FUN_0044de18(DAT_005ea728[t*0x45]));                   // party owns that item
if (t != 0 && party[0].type != 2 && !FUN_00479fc4(t, &flags) && !special) return 0;
return 1;
```
* **terrain 0 is unconditionally walkable.**
* any other value needs either the `+TERRAINS` arg3 item, or an equipment "movement" capability.

`FUN_00479fc4` (0x479fc4, all.c:90043) scans 14 equipment slots of the party leader; slots 2..9 all read `party+0x6A4`. For each item it looks at `item+0x2E4` (item stride 0x30C) and compares against the terrain id:
* `== t` → can cross terrain t.
* `== 10` (0xA) → can cross everything except terrain 9.
* `== 11` (0xB) → sets out-flag **bit 2** → 10x lower random-encounter rate (flying/boat).
* `== 12` (0xC) → sets out-flag **bit 1** → **double walk speed**.
* `t == 0` or `t == -1` → always returns 1.

`FUN_0044de18` (0x44de18, all.c:55801) = "does the party own item id N" (searches `party+0x76C` bag, ids 1..0xFFF).

Rendering the terrain overlay (`FUN_00461684`, all.c:69997) draws each non-zero cell as a 16x16-screen-pixel `FillSolidRect` using the *current palette entry* for that index — this is a debug overlay only, gated on `DAT_004df8b4` which is 0 in normal play (see §9).

Editor painting is `FUN_004642c7` (0x4642c7, all.c:71998) — it writes `DAT_004f0ce0` (the brush terrain id, chosen from the `TerrainPalette` resource) into the same `x>>2, y>>2` grid.

---

## 4. `.obl` — link/object table

**Size:** 204800 bytes = **256 records x 800 bytes**. Confirmed by size arithmetic and by `FUN_00463989` (0x463989, all.c:71688):
```c
memset(param_2, 0, param_3 * 800);
sVar1 = fread(param_2, 800, param_3, _File);
```
Array base `DAT_00604808`, end `DAT_00636808`; loops run `while (piVar4 < &DAT_00636814)` with stride 200 dwords (= 800 B) — e.g. `all.c:71778-71820`. Save is the mirror `FUN_0046393f` (all.c:71670). So `.obl` is a **raw memory dump**: runtime-only fields are written to disk too.

### Record layout (offsets from record start)

| off | name | evidence |
|-----|------|----------|
| +0 | `used` | `(&DAT_00604808)[i*200]`, all.c:71259/71575 |
| +4 | `objectId` — index into objects.obr (0..1023) | `(&DAT_0060480c)`, bounds-checked `<0x400` at all.c:67025 |
| +8 | `x` (map units) | `(&DAT_00604810)`, all.c:71254 |
| +12 | `y` (map units) | `(&DAT_00604814)`, all.c:71256 |
| +16 | `kind` (0..4) | `(&DAT_00604818)`, all.c:71637 |
| +100 (0x64) | `targetId` (char/scene id; `lParam` of WM_0x475, `wParam` of WM_0x46A) | `(&DAT_0060486c)`, all.c:71647/71661 |
| +104 (0x68) | **signed monster difficulty level / group id** | `(&DAT_00604870)`, all.c:106407; editor exposes it as a combo + a "negative" checkbox (`local_18[0x1a]`, `abs()` into the combo, `SendMessageA(...,0xf1, local_18[0x1a] < 0)`) at all.c:67166-67176 |
| +108 (0x6C) | sound theme override (quest `theme N`) | `(&DAT_00604874)`, all.c:37516 |
| +112 (0x70) | secondary string (chat/label) | `&DAT_00604878 + i*800`, all.c:90270 |
| +192 (0xC0) | **display name** (`char*`) | `piVar6+0x2d` = 12+45*4, drawn by `FUN_0049c0b0` at all.c:71810; editor `SetWindowText(0x5dc, local_18+0x30)` all.c:67190 |
| +400 (0x190) | runtime: Manhattan distance hero→link (0 = hero standing on it) | `piVar4[0x61]` all.c:71602; zeroed on hit all.c:71586 |
| +404 (0x194) | destination map number (kind 2) / scene arg | `(&DAT_0060499c)` all.c:71643; editor sets the string from `&DAT_00ce2bde + val*0x100` all.c:67157 |
| +408 (0x198), +412 (0x19C) | two ints published to the script engine for the nearest link | all.c:90309/90321 |
| +416 (0x1A0) | `hasBeenUsed` (link name revealed after first use) | `piVar6[0x65]` = 12+404 all.c:71808; set all.c:70984; editor treats link 0 specially all.c:23472 |
| +420 (0x1A4) | required item id (link only active if the party owns it) | `(&DAT_006049ac)` all.c:71589; editor `local_18[0x69]` all.c:67165 |
| +424 (0x1A8) | extra numeric id, surfaced as `%d` | `local_18[0x6a]` all.c:67192; cached into `DAT_004f2248` all.c:71613 |
| +428..+799 | unused / editor workspace | — |

### `kind` semantics — `FUN_00463853` (0x463853, all.c:71634)
```c
k = link.kind;
if (k == 0) return 0;                       // pure decoration
if (k == 2 || k == 3) {
    SendMessageA(main, WM_0x46A /*scene enter*/, link.targetId, link.destMap);
    if (k == 2) { FUN_00436ffc(link.destMap, namebuf, 1);   // switch map
                   FUN_00456d2f(mapName[destMap]); }
    return 0;                                // k==3: run a scene, stay put
}
// k == 1 or k == 4:
SendMessageA(main, WM_0x475 /*PK/duel to character*/, self, link.targetId);
```
`FUN_004625ee` (all.c:70847) returns true only when the nearest link's `kind == 4` — i.e. **kind 4 is a "no random monsters here" link**. `FUN_0046259a` + `DAT_004f2228` produce the player message *"There are no monsters here - hunt"* (all.c:70837).

### Hit box — `FUN_004636d3` (0x4636d3, all.c:71546), called once per tick from `FUN_00462958`
```c
halfW = (obrRect.right - obrRect.left + 7) / 8;    // objects.obr rect / 8
halfH = (obrRect.bottom - obrRect.top + 7) / 8;
SetRect(&r, link.x - halfW, link.y - halfH, link.x + halfW, link.y + halfH);
```
It walks all 256 records, remembers the **nearest** link by Manhattan distance (`DAT_004f2240`) and, if the point is *inside* a link rect, sets `DAT_004f2244` to that link's index — but only if `kind >= 1` **and** the party owns the required item at +420. It `break`s on the first containing link.

### Drawing — `FUN_004639eb` (0x4639eb, all.c:71760)
```c
local_38 = (link.x - viewLeft) * 4;      // 4x: map units -> screen px
local_34 = (link.y - viewTop)  * 4;
StretchBlt(obrRect_from_obr, dest, SRCCOPY);
```
The sprite rect comes from `objects.obr` entry `objectId`; the name at +192 is drawn above it (`OffsetRect(0,-0x20); InflateRect(200,0)`) when `hasBeenUsed` is set.

### objects.obr — 1000 records x 48 bytes
`FUN_00463630` (all.c:71517) `fread(param_2, 0x30, param_3, f)`; load loop bound gives 1000 records (all.c:23168-23173); the editor validates ids `< 0x400`. Layout (base `DAT_005f8808`):
```
+0  used
+4  name[24]
+32 left, +36 top, +40 right, +44 bottom    // source rect inside objects.bmp/jpg
```

---

## 5. `.mon` — monster placement table

**Size:** 276000 bytes = **1000 records x 276 bytes (0x45 dwords)**. `FUN_00464461` (0x464461, all.c:72101):
```c
memset(param_2, 0, 0x43620);
sVar1 = fread(param_2, 1, 0x43620, _File);
bVar3 = sVar1 == 0x43620;                  // DAT_004f224c = "mon file loaded"
for (i = 1000; i; i--, param_2 += 0x45) {
    if (*param_2 > 0) DAT_004f2254++;      // count of live placements
    param_2[4] = 0;                        // clear cached sprite handle
}
```
Save is `FUN_00464421` (all.c:71656) writing 0x43620 bytes verbatim.

### Record layout (276 bytes)

| off | name | evidence |
|-----|------|----------|
| +0 | `monsterId` — 1-based index into `monsters.txt` (max 4096) | `0 < id < 0x1000` tested at all.c:72272/72517 |
| +4 | `x` (map units) | drawn at `(x - viewLeft)*4` |
| +8 | `y` (map units) | |
| +12 | `radius` (map units) | editor draws `InflateRect(±(r*4), ±(r*4))` + `Ellipse`, all.c:72302-72310 |
| +16 | runtime sprite handle (zeroed on load) | `piVar15[4]` all.c:72199 |
| +20..275 | unused | |

Editor click-picking is `FUN_00464b06` (all.c:72415): a hit box of `(x-4,y-4)-(x+4,y+4)` in map units, and while the hero is inside a zone the current zone is forced.

**In normal play these monsters are invisible**: the map paint only calls `FUN_004644ee` when `DAT_004df8b8 != 0` (all.c:69907), and that flag is initialised to 0 (all.c:24068) and only toggled from the debug menu string `"monsters"` (all.c:38659-38663). Same for the terrain overlay flag `DAT_004df8b4`. You meet these monsters only through the proximity roll in §8.

---

## 6. Pathfinding and movement

### Grid snapping — `FUN_00461b11` (0x461b11, all.c:70255)
```c
int FUN_00461b11(int v) { return ((v + 3) >> 2) * 4 + 2; }   // centre of the 4x4 cell
```
All path nodes are `4*k + 2` in map units — the *same* grid as the terrain cells, so terrain index and path index coincide.

### The line march — `FUN_00461b93` (0x461b93, all.c:70272)
Classic Bresenham-with-divisor: `steps = 2*max(|dx|,|dy|)` (min 1), then repeatedly `x = x0 + accx/steps; accx += dx;` and `FUN_004631c6(x,y)`; stops at the first blocked cell and snaps back to the last walkable point via `FUN_00461b11`. When `param_7 != 0` it additionally requires all four diagonal neighbours to be walkable (`FUN_00461b26`, all.c:70258) to avoid clipping corners.

### Path builder — `FUN_00461dcc` (0x461dcc, all.c:70413)
Stores a polyline in `DAT_00679e28` (24000 bytes = 2000 nodes x 3 ints: x, y, and a scratch word) with `DAT_004f2168` = node count. It marches, and on hitting a wall performs up to 10 sideways detours using the 8-direction table at `DAT_004f21a0`/`DAT_004f21c0` via `FUN_00461d41`, splicing the successful detour in and resuming. It also prunes the polyline with a "is there line of sight" test (`FUN_00461cd7` -> `FUN_00461b93` with `param_7=0`). `FUN_00461cf7` (all.c:70403) is a 3x3 direction table at `DAT_004f21e0` indexed `((dy+1)&3)<<2 | (dx+1)&3`.

### Click target validation — `FUN_0046206d` (0x46206d, all.c:70545)
```
start = snap(hero); goal = snap(click)
best = min over three runs of FUN_00461dcc(start, goal, preferFast in {1,0,1}) by node count
if (best.nodes < 2000 && best.nodes > 2) DAT_004f216c = 1;   // "target is reachable"
```
So an unreachable click is silently snapped to the closest reachable cell on the march line, and a click with essentially no path (`nodes <= 2`) is dropped.

### Starting a walk — `FUN_004620f3` (0x4620f3, all.c:70586)
```c
FUN_0046206d(heroX>>8, heroY>>8, targetX>>8, targetY>>8);
if (DAT_004f216c > 0) { targetX = DAT_00679e38 << 8; targetY = DAT_00679e3c << 8; }
if (param_5 /*player-initiated*/) FUN_0046206d(...)  // re-validate
if (FUN_004631c6(target>>8, target>>8) == 0) return;
if (param_4 < 1) param_4 = 1;                       // speed
rec->targetX = param_2; rec->targetY = param_3;     // +0x9C / +0xA0
rec->startX  = rec->x; rec->startY = rec->y;        // +0xBC / +0xC0
rec->stepX   = (speed * (tx - x)) / dist;           // +0xA4
rec->stepY   = (speed * (ty - y)) / dist;           // +0xA8
rec->speed   = speed;                               // +0xAC
rec->startTick = GetTickCount();                    // +0xB0
rec->duration  = (dist * 1000) / speed;             // +0xB4   <-- ms
```
`FUN_004622f4` (all.c:70659) is the thin wrapper `FUN_004620f3(..., 1)` used for player-initiated walks.

**Speed units:** `speed` is 24.8-fixed *map units per millisecond*; `duration_ms = distance_units * 1000 / speed`.

| constant | value | map units/s | screen px/s | where |
|----------|-------|--------------|-------------|-------|
| world map (`mapId == 0`) | `0xC80` = 3200 | 12.5 | 50 | all.c:23356 / 71047 |
| any other map | `0x12C0` = 4800 | 18.75 | 75 | all.c:23370 / 71052 |
| ×2 if any equipped item has `item+0x2E4 == 12` | | | | all.c:23377-23379 |

### The per-frame step — `FUN_0046230e` (0x46230e, all.c:70669)
```c
elapsed = GetTickCount() - rec->startTick;
if (rec->duration < elapsed) { FUN_00461948(hero, rec->targetX, rec->targetY, 1); }  // arrive
else {
   nx = rec->startX + (rec->stepX * elapsed) / 1000;
   ny = rec->startY + (rec->stepY * elapsed) / 1000;
   clamp both >= 0;
   facing = pack(dirX, dirY) into rec+0x88, remapped 5 -> 9 when both axes move;
   if (!FUN_004631c6(nx>>8, ny>>8)) FUN_00461948(hero, rec->x, rec->y, 1);  // blocked: hold
   else { rec->x = nx; rec->y = ny; }
}
return rec->speed > 0;   // true while still moving
```
Direction packing (`all.c:70705-70726`): each axis is `0` if the delta is < 0, `1` if |delta| < 0x14 (20), `2` if increasing; result is `dirY<<2 | dirX`, and diagonal `5` is stored as `9`.

`FUN_00461948` (all.c:70145) is the "advance the path" callback. For the hero it does `DAT_004f216c++` and re-issues `FUN_004620f3` toward `DAT_00679e2c[node]` (all.c:70163-70165) — i.e. the hero is walked **one polyline node at a time**, and each hop re-runs the collision-validated march for the next few cells. This is what makes the walk re-route if the terrain changes under it.

### Actor record (`DAT_0067fbf8`, stride 0x16CC, 44 records = party + nearby characters)
`+0x94/+0x98` = x/y (24.8), `+0x9C/+0xA0` = target, `+0xA4/+0xA8` = per-tick step, `+0xAC` = speed, `+0xB0` = start tick, `+0xB4` = duration, `+0xBC/+0xC0` = leg start, `+0x88` = facing, `+0x25/+0x26` (=+0x94/+0x98 aliases) = position, `+0x90` = map id, `+0x1C` = HP>0 flag, `+0x33` (=+0xCC) = "in scene" flag, `+0xA04` = encounter difficulty, `+0x6BC` = busy flag, `+0x6A4` = the mount/vehicle equipment slot.

### Player input
**There is no keyboard walking.** Movement is click-to-walk only:

* `FUN_00462b5d` (0x462b5d, OnLButtonDown, all.c:71012) with `DAT_004df8a4 == 6` (the live map mode) converts the click and calls `FUN_004622f4`.
* `FUN_0041f247` (0x41f247, all.c:23346) is the older/other click path:
  ```c
  if (!PtInRect(mapRect, pt)) return 0;
  tx = (pt.x - mapRect.left) + DAT_00539388 - 0x80;    // 0x80 = 128 = 256/2
  ty = (pt.y - mapRect.top)  + DAT_00539384 - 0x80;
  tx = max(0, tx << 8); ty = max(0, ty << 8);
  speed = (mapId != 0) ? 0x12C0 : 0xC80;
  if (FUN_00479fc4(-1, &flags) & 1) speed *= 2;
  PlaySound("walk.wav"); FUN_004622f4(0, tx, ty, speed);
  ```
* Mouse-move changes the cursor to a "no entry" cursor when `FUN_004631c6` says the cell is blocked (`FUN_00463258`, all.c:71323).
* Other movers: the quest-script `MOVE` command (all.c:34169, 34291 → `FUN_004622f4`) and the idle auto-wander below.

### Idle auto-wander
`FUN_004620f3` (`all.c:70637-70653`): after each walk leg, if the leg was short (`dist < 0x2800` = 40 map units) and started within 2 s of the previous one, remember the endpoint as a wander target and bump `_DAT_004f219c` (leg count). `FUN_004610d9` (all.c:69784) is called from the map paint and, if >2 s have elapsed, walks one more leg to the remembered point and clears it. This is what makes the hero shuffle about while idle — and it is a large fraction of how often the encounter roll gets evaluated.

### Camera
Hard-centred, no inertia. `FUN_00462958` (all.c:70947) each tick, for the hero record and when `DAT_004df8a4 == 6`:
```c
DAT_00539388 = (hero.x + (hero.x>>31 & 0xff)) >> 8;
DAT_00539384 = (hero.y + (hero.y>>31 & 0xff)) >> 8;
```
`FUN_00461138` (OnPaint, all.c:69861) then derives the view rect:
```c
viewW = (clientW + 3) / 4;                       // in map units
DAT_005394b0 = DAT_00539388 - (viewW - clientW/2)/1 ...   // effectively hero - viewW/2
SetRect(view, DAT_005394b0, DAT_005394b4, DAT_005394b0 + viewW, DAT_005394b4 + viewH);
```
Hero screen position = `(viewW/2) * 4` = `clientW/2` — dead centre. The main frame's timer (all.c:21284-21291) invalidates only the changed sub-rect when the camera moved. Editor modes 9/10 have mouse-drag panning via `FUN_0041f309` (all.c:23392).

Paint order (all.c:69863-69953):
1. black clear
2. `StretchBlt`/`BitBlt` the map: source rect `clientRect` offset by `(viewLeft<<2, viewTop<<2)` — i.e. **the X4 hi-res image sampled 1:1**, clamped against the image bounds (out-of-map is black)
3. debug terrain overlay (`DAT_004df8b4`)
4. debug monster sprites (`DAT_004df8b8`)
5. link objects + names (`FUN_004639eb`)
6. actors, with shadows
7. the walk polyline overlay (`FUN_00461a1e`, editor only), the linked-object rectangle (`FUN_004633ce`)
8. `FUN_004610d9()` — pump one wander leg

### Map edges and link transitions
There are **no** edge-scroll transitions. The hero is hard-clamped by `FUN_004631c6`'s bounds test (`x < 0 || x >= mapW || y >= mapH` -> blocked), so the camera can never leave the map. Transitions between maps happen **only** by walking onto a link:

`FUN_00462958` (all.c:70973-70996):
```c
nearest = FUN_004636d3(heroX, heroY);                 // DAT_004f2240
hit     = FUN_0046260e(heroX, heroY, &state, moving); // see below
if (nearest < 0 || hero[0x2B] != 0)      onLinkLatch = 0;
else if (!onLinkLatch && (link.kind != 4 || hit == 0 || hit == self)) {
    onLinkLatch = 1; activated = 1;
    log("M %d L %d", mapId, nearest);  PlaySound("link.wav");
    record[+0x1A0] = 1;                 // mark used
    FUN_00463853(nearest);              // -> WM_0x46A / map change / WM_0x475
}
if (hit == 0 || onLinkLatch) activated = 0;
else if (!activated) { FUN_00461a07(...); activated = 1; onLinkLatch = 1;
                       PostMessageA(main, WM_0x475, hit, state); }   // walk up to a player
```
So: *reaching* a link (`DAT_004f2244 == nearest`, distance 0) fires it once; *being near* another player walks the hero into them and sends a duel request.

`FUN_00436ffc` (0x436ffc, all.c:39134) is the actual map switch: it sets `DAT_004e70b4 = destMap`, picks the wandering MIDI via `music.ini` keys `numMidi` / `midi<N>`, then calls `FUN_00436fc1`. `FUN_0041e421` (the loader) is re-entered from the frame at all.c:37026. On exit the party record's map id is updated and `FUN_00461a07` re-seeds the hero position.

---

## 7. `+TERRAINS` vs. the `.ter` file — the full chain

```
.ter BMP byte 0..9
   -> FUN_0046186f()                        (0x46186f)  value at 4x4-cell
   -> FUN_004631c6()                        (0x4631c6)  pass / block
        |- DAT_005ea728[t] != 0 && FUN_0044de18(item)   -> special boots/token
        '- FUN_00479fc4(t, &flags)                      -> equipment capability
   -> FUN_0049099b()                        (0x49099b)  (not used for terrain)
```

---

## 8. Random encounters — the actual rule

The whole thing is in `FUN_0046260e` (0x46260e, all.c:70865), evaluated **once per party record per frame** from `FUN_00462958` (all.c:70981), i.e. once per animation tick, and *only* after the 44-record party loop has been exhausted (the `0x5B2FF` test at all.c:70906).

### 8a. The roll
```c
// preconditions, all must hold:
moving || FUN_0046259a(1000)                    // >1 s since the last battle ended
nearestLink >= 0
party[0].+0xCC == 0                              // not inside a scene
party[0].+0x70 > 0                               // hero alive
party[0].type != 2                               // not in a vehicle

if (link[+104] < 2 && (!monLoaded || monCount == 0 ||
                       (link[+104] != 0 || onLink != nearestLink)))
    DAT_004f2228 = 1;                            // -> "There are no monsters here - hunt"
else {
    base = 200;
    if (FUN_00479fc4(-1, &flags) & 2) base = 20; // flying/boat: 10x rarer
    lvl  = FUN_00436b15(party);                  // == bitlen(party[0].+0xA04) - 3, min 0
    threshold_A = (uint)(base * 3) / (lvl + 3);
    if ( (rand() % 10000 < threshold_A)
         && GetTickCount() - lastEncounterA > 5000 && moving )
     or ( (r = rand()) % 10000 < party[0].+0xA04
         && GetTickCount() - lastEncounterB > 5000
         && FUN_0046259a(0) )                    // any time since the last battle
    {
        lastEncounterA = lastEncounterB = GetTickCount();
        DAT_004e70ac = 0;
        *outState = 2;
        FUN_004624ef(self);                      // "I'm the one being attacked"
        PlaySound("fight.wav");
        return self;                             // -> PostMessage WM_0x475(self, 2)
    }
}
```
`FUN_00436b03` (all.c:38987) is `floor(log2(n))+1`; `FUN_00436b15` (all.c:39002) subtracts 3 and clamps at 0.

So the **per-tick probability** is `200*3 / (bitlen(difficulty) - 3 + 3) / 10000`, i.e. `0.06 / (bitlen(difficulty))` — with the party-difficulty field at offset `+0xA04` reused *raw* as a second `/10000` threshold. The rate is scaled by how many animation frames elapse, which is why the short idle-wander legs in §6 matter so much: **encounter rate is per frame, not per map unit walked.** A faithful port should pick a fixed tick rate (or convert to per-map-unit) rather than replicate the frame dependence.

`FUN_0046259a` (all.c:70851) is the post-battle grace period: 2 s, after which (if `DAT_004f2228`) it emits the "no monsters here" hint. `DAT_004e70a8` is the timestamp.

### 8b. Which monsters — `FUN_0049099b` (0x49099b, all.c:106381)
Triggered by the battle, uses `DAT_004f2240` = the **nearest** link (not necessarily the one you're standing on) and `DAT_004f2248`-adjacent fields:

```c
if (nearestLink >= 0) {
    group  = link[+104];                    // signed monster difficulty level
    dist   = link[+400];                    // Manhattan distance hero -> link, map units
    if (monLoaded && group == 0) return FUN_00464daf();      // .mon proximity roll instead
    pct = dist; if (pct > 80) pct = 80; if (pct < 20) pct = 20;   // 20..80, percent
    if (group < 0) { group = -group; pct = 100 - pct; hard = true; }
    if (group != 0 && !hard && group < 0x1000
        && groups[group].used && (n = groups[group].count) > 0) {
        for (i = 0; i < n; i++) {
            mid = groups[group].ids[i];
            if ((rand() % 100 < pct) || DAT_00502a38 /* group 0 flag */) {
                if (FUN_00480499(mid, 0, 1, 0, 0, 0) > 0) { spawn(mid); picked++; seen(mid); }
            }
        }
    }
}
if (picked == 0 && n > 0) spawn(groups[group].ids[rand() % n]);   // guaranteed fallback, one monster
```
Key consequences:
* The **group** is chosen by the nearest link's `+104` field, exactly as `groups.txt:1-40` documents.
* The **inclusion probability of each listed monster** is the clamped Manhattan distance from the hero to that link, 20%..80%. Positive level = easier near the link (towns), negative = `100 - pct` so it gets harder near the link (guarded areas).
* A **negative** level never rolls the list at all (`hard` short-circuits) — it only affects the sign convention in the UI.
* `groups.txt` group 0 sets `DAT_00502a38` (line `0, 1` in Evergreen) = "not random, all members appear".
* The last-resort `picked==0` branch guarantees at least one monster whenever the group is non-empty.

`groups.txt` parse: `FUN_00483686` (all.c:96270) into `DAT_008e5b58`, 4096 records x 44 bytes (`memset(..., 0x2C000)`):
```
+0  used | +4 count | +8 .. +43  up to 9 monster ids
```

### 8c. `.mon` proximity roll — `FUN_00464daf` (0x4644daf, all.c:72493)
Used instead of 8b when the map has a `.mon` file **and** the nearest link's level is 0. Iterates all 1000 `.mon` records; for each with a valid monster id, computes the **Euclidean** distance from the hero and:
```c
if (dist < radius) {
    if (rand() % 100 < 25)  spawn(mon);                       // 25%
    if (dist < radius * _DAT_004cd548) {                      // constant [UNVERIFIED]
        spawn(mon);
        if (rand() % 100 < 15) spawn(mon);                    // +15%
    }
    if (dist < radius * _DAT_004cd578) {                      // constant [UNVERIFIED]
        spawn(mon);
        if (rand() % 100 < 5) spawn(mon);                     // +5%
    }
}
```
So inside a zone you always get 1 monster (25% chance, rolled per check), up to 3 near the centre. `_DAT_004cd548` > `_DAT_004cd578`; the values are doubles in `.rdata` at 0x4CD548 / 0x4CD578 and I could not read the binary (`read` refuses PE paths in this environment) — 0.6 / 0.3 is a plausible guess but is **[UNVERIFIED]**. Note both branches are independent, so a monster can be spawned 2x or 3x from one check.

---

## 9. What is *not* drawn in normal play

`DAT_004df8b4` (terrain overlay) and `DAT_004df8b8` (monster sprites) are both set to 0 at world entry (all.c:24068-24069) and only flipped by the `Debug` menu strings `"terrain"` / `"monsters"` (all.c:38650-38664) or by the Link Editor (all.c:66934/66939/67823). **In a shipping solo build the hero walks over an invisible terrain map and never sees the `.mon` monsters until they attack.** Do not port the overlays as gameplay features.

Also editor-only (mode `DAT_004df8a4` 9 = Link Editor, 10 = object placement, 7 = story): `FUN_00461684`, `FUN_004644ee`, `FUN_00461a1e` (path polyline), `FUN_0045d65f`/`FUN_0045d5be`/`FUN_0040f849`/`FUN_0040f8fd`/`FUN_0045dba0` (link/object/mon editor init), `FUN_004642c7` (terrain brush), `FUN_00464b06` (monster placement), `FUN_0046393f`/`FUN_00464421` (OBL/MON writers).

Mode 6 is the live walking map. `FUN_0041b891(6)` / `FUN_0041b891(7)` select it (all.c:37026, 21277).

---

## 10. Minimal data model for a C port

```c
typedef struct {            /* 800 bytes == one .obl record */
    int32_t used, object_id, x, y, kind;          /*  0,  4,  8, 12, 16 */
    int32_t target_id;                            /* 100 */
    int32_t monster_level;                        /* 104  signed */
    int32_t theme;                                /* 108 */
    char    label2[?];                            /* 112 */
    /* ... */
    char   *name;                                 /* 192 */
    /* ... */
    int32_t dist;                                 /* 400 runtime */
    int32_t dest_map;                             /* 404 */
    int32_t var_a, var_b;                         /* 408, 412 */
    int32_t has_been_used;                        /* 416 */
    int32_t required_item;                        /* 420 */
    int32_t aux_id;                               /* 424 */
} Link;                        /* must stay 800 bytes for byte-compatible I/O */

typedef struct {            /* 276 bytes == one .mon record */
    int32_t monster_id, x, y, radius;             /*  0, 4, 8, 12 */
    void   *sprite;                               /* 16 runtime */
} MonPlace;                  /* 1000 entries == 276000 bytes */

typedef struct {            /* 48 bytes == one objects.obr record */
    int32_t used;                /*  0 */
    char    name[24];            /*  4 */
    int32_t l, t, r, b;          /* 32, 36, 40, 44 */
} ObjRec;                     /* 1000 entries */
```

Load order for a map: parse `maps.txt` -> open `root.jpg` (record W,H) -> `root.ter` as 8bpp `(W+3)/4 x (H+3)/4` -> `root.obl` (256x800) -> optional `rootX4.jpg` -> `objects.bmp|jpg` + `objects.obr`.

---

## 11. Quick reference — VAs

| VA | name | role |
|----|------|------|
| 0x41E421 | `FUN_0041e421` | map load: `.obl`, `.ter`, `.mon`, `X4.jpg`, objects |
| 0x486690 | `FUN_00486690` | generic BMP/JPEG image loader (returns a `CBitmap*`) |
| 0x4864E0 | `FUN_004864e0` | create an 8bpp DIB section |
| 0x4864C0 | `FUN_004864c0` | `abs(biHeight)` of a bitmap |
| 0x482C54 | `FUN_00482c54` | `+MAPS` / `maps.txt` parser |
| 0x481F8A | `FUN_00481f8a` | `+TERRAINS` parser |
| 0x483686 | `FUN_00483686` | `+GROUPS` parser |
| 0x463989 | `FUN_00463989` | `.obl` reader (256 x 800) |
| 0x46393F | `FUN_0046393F` | `.obl` writer |
| 0x464461 | `FUN_00464461` | `.mon` reader (1000 x 276) |
| 0x464421 | `FUN_00464421` | `.mon` writer |
| 0x463630 | `FUN_00463630` | `.obr` reader (1000 x 48) |
| 0x4642C7 | `FUN_004642c7` | terrain brush (editor) |
| 0x461684 | `FUN_00461684` | terrain overlay draw (debug) |
| 0x46186F | `FUN_0046186f` | **terrain byte lookup** `x>>2, y>>2` |
| 0x4631C6 | `FUN_004631c6` | **can-walk-here test** |
| 0x479FC4 | `FUN_00479fc4` | equipment terrain/fly/speed capabilities |
| 0x44DE18 | `FUN_0044de18` | party owns item id |
| 0x461B11 | `FUN_00461b11` | snap to 4-unit cell centre |
| 0x461B26 | `FUN_00461b26` | all-4-diagonals-walkable |
| 0x461B93 | `FUN_00461b93` | Bresenham line march |
| 0x461CF7 | `FUN_00461cf7` | 3x3 direction table |
| 0x461D41 | `FUN_00461d41` | sideways detour step |
| 0x461DCC | `FUN_00461dcc` | path builder (<=2000 nodes) |
| 0x46206D | `FUN_0046206d` | validate/repair a click target |
| 0x4620F3 | `FUN_004620f3` | start a walk leg |
| 0x4622F4 | `FUN_004622f4` | player-initiated walk |
| 0x46230E | `FUN_0046230e` | per-tick position + facing |
| 0x461948 | `FUN_00461948` | arrive / advance path node |
| 0x461138 | `FUN_00461138` | map OnPaint (camera, blit, overlays) |
| 0x4610D9 | `FUN_004610D9` | idle auto-wander pump |
| 0x462958 | `FUN_00462958` | map tick: camera, nearest link, link activation, encounters |
| 0x46260E | `FUN_0046260e` | **random-encounter roll** + nearby-player scan |
| 0x49099B | `FUN_0049099b` | **PickRandomMonster** (groups.txt) |
| 0x464DAF | `FUN_00464daf` | **PickRandomMonster** (`.mon` proximity) |
| 0x46259A | `FUN_0046259a` | 2 s post-battle grace / "no monsters" hint |
| 0x4625EE | `FUN_004625ee` | nearest link is kind 4 (safe) |
| 0x4636D3 | `FUN_004636d3` | nearest link + link-under-hero |
| 0x463853 | `FUN_00463853` | activate a link (dispatch by kind) |
| 0x4639EB | `FUN_004639eb` | draw link sprites + names |
| 0x4644EE | `FUN_004644ee` | draw `.mon` sprites (debug) |
| 0x464B06 | `FUN_00464b06` | `.mon` editor click |
| 0x436FFC | `FUN_00436ffc` | switch map + start map music |
| 0x41F247 | `FUN_0041f247` | click-to-walk (older path) |
| 0x41F309 | `FUN_0041f309` | editor camera drag |
| 0x436B03/0x436B15 | | `bitlen`, and `bitlen-3` |
| 0x481F8A, 0x483686, 0x482C54 | | quest-table parsers |

---

## 12. Open items / to verify

1. `_DAT_004cd548` and `_DAT_004cd578` (the two inner-radius fractions in `FUN_00464daf`) — doubles at 0x4CD548/0x4CD578; the PE reader is blocked in this environment. Read them out of `extracted/Souls.exe` directly.
2. The exact semantics of the OBL +112 / +408 / +412 / +424 fields (chat label, two engine variables, one numeric id). Low impact for solo play.
3. Whether `objects.bmp` cells are authored at 2x the on-map size. The click box is `w/8` half-width while the sprite is blitted at 4x, so the hit box is much smaller than the sprite by design; the authoring scale is worth eyeballing against `maps/objects.bmp`.
4. `FUN_00480499` (spawn a monster into the fight) and the surrounding battle plumbing — out of scope here but it is the consumer of the group list.


## Sources

- `work/decomp/all.c`: Full decompilation of Souls.exe. Map subsystem lives at lines ~69690-73400 (VA 0x460000-0x465000) and the map loader at lines 22994-23324 (VA 0x41e421).
- `work/decomp/functions.tsv`: VA list + sizes, used to confirm FUN_00463989 (OBL reader), FUN_00464461 (MON reader), FUN_0049099b (PickRandomMonster) exist as separate functions.
- `extracted/worlds/Evergreen/quest.txt`: Authoritative spec for the 6 CSV columns (id, image file, OBL/TER/MON/MIDI root, map name, map flags bitmask, sound theme) and the full map-flag table.
- `extracted/worlds/Evergreen/quest.txt`: Terrain table: 10 entries, id / name / damage / token. Terrain 0 = 'No Hindrance' (always walkable), 9 = 'Impassable'. arg3 = token number that permits crossing.
- `extracted/worlds/Evergreen/groups.txt`: Monster group table. Documents that the NEAREST LINK's signed 'difficulty level' selects the group, that the per-monster inclusion probability rises with Manhattan distance from that link, and the sign convention (positive = easier near the link, negative = harder near it).
- `extracted/worlds/Evergreen/maps.txt`: Map table itself: 18 rows, columns `id, imageFile, rootName, "Map Name", flags[, theme]`.
- `extracted/worlds/Evergreen/maps/readme.txt`: Authoritative prose on mapName.JPG / mapNameX4.JPG (must be exactly 4x linear in each dimension), mapName.OBL, mapName.TER, mapName.MON, OBJECTS.BMP/JPG and OBJECTS.OBR.
- `extracted/worlds/Evergreen/maps/evergreen.jpg`: Main world map image. Renders 768x768 -> confirms .ter is 192x192 = jpg/4 (file is 37942 B = 1078 B header/palette + 192*192).
- `extracted/worlds/Evergreen/maps/evergreen.ter`: 8-bit indexed BMP, 37.1 KB = 14 (BITMAPFILEHEADER) + 40 (BITMAPINFOHEADER) + 1024 (256*RGBQUAD palette) + 192*192 pixel bytes. Bottom-up, stride = (192+3)&~3 = 192.
- `extracted/worlds/Evergreen/maps/evergreenX4.jpg`: Hi-res map image, must be exactly 4x evergreen.jpg in each dimension (3072x3072). Drawn 1:1 into the 256x256 walk window.
- `extracted/worlds/Evergreen/maps/evergreen.obl`: 204800 bytes = 256 records x 800 bytes. Raw dump of the in-memory link array.
- `extracted/worlds/Evergreen/maps/evergreen.mon`: 276000 bytes = 1000 records x 276 bytes. Raw dump of the in-memory monster-placement array.
- `extracted/worlds/Evergreen/maps/objects.obr`: Object name table: 1000 records x 48 bytes = 48000 B. {used, name[24], pad, left, top, right, bottom}. Gives each link its sprite rect and click box.
- `extracted/worlds/Evergreen/maps/objects.bmp`: 256-colour RLE BMP of link sprites; preferred over objects.jpg when present. Source rects for the .obr entries.
