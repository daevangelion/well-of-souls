# Art formats (Souls.exe)

## Summary

Complete map of WoS A96 art: all art flows through one loader (FUN_00486690 -> FUN_00488080 BMP / FUN_00488510 JPG) into a 0x18-byte "Picture" (BITMAPINFO* + DIB bits) created as an 8bpp BI_RGB DIB section; transparency is a colour-key equal to the palette index of the bitmap's bottom-left pixel, applied in the software blitter FUN_00486d20 (`if (*_Src != key) *_Dst = *_Src`), and every art file is a horizontal "filmstrip" of square cells whose side equals the bitmap height (cell count = width/height, stored at record+0x484). Skins (skins\*.bmp) are 5 squares of side h: [MAP][READY][ATTACK][WEAK][CHAT], MAP subdivided 3x3 into h/3 map sprites (8 directions + centre for camping), h>35 enforced by validator FUN_0048dd7d, lower 1/6 of the four views is shadow; FUN_0048dad9 paints all 1px cell borders and the top 2 rows of the shadow band with the key colour; pose defaults to cell 2 (cell 6 if the sheet has >6 cells); a 64KB table DAT_005394b8[256][256] at DAT_005394b8 implements recolouring. Monsters use the same filmstrip, resolved worlds\W\monsters\ then root\monsters\, indexed by the ACTOR "pose" argument. UI: button*.bmp are horizontal strips of 48x48 cells, cell = state (1 up, 2 down/active, 3 mouse-over, 4/5 blink) laid out 10-wide at right edge, pitch 51px; buttonBar*.bmp is the bar background; bk*.jpg/itemTable.jpg/quadris.jpg are 256x256 dialog backgrounds blitted through a 0x100x0x100 scratch DIB; item sheets follow the items.txt class->bmp table (48x48, or 48x64 for armor/shields/staffs/swords) with arg3 = cell index (0 = empty-handed) and up to 9 "_N" extensions; attack/effects sheets are 8x10 grids of 48x48 (image N -> file N/10, strip N%10, 8 frames). Theme override order is world -> themes\<T> -> root (FUN_0042445e). Font is "Tempus Sans ITC" (tempsitc.ttf, AddFontResourceA at 0x408A6E), 110pt in most dialogs. NOTE: I could NOT measure BMP pixel dimensions - my tool set has no shell/python, read() corrupts binary, and grep skips binaries; section 9 of the report contains the exact script to generate the table.

## Architecture

All WoS art funnels through one pipeline: a path is built as a string, sniffed for type, and loaded into a uniform 0x18-byte Picture object that owns an 8bpp top-down DIB section. Every consumer then does source-rect math on a "horizontal filmstrip of square cells" whose cell size is derived from the bitmap height (or a hardcoded 0x30 = 48). Transparency is a single palette index read from the bitmap's bottom-left pixel. The root art\ folder holds UI chrome; worlds/<W>/art/ holds per-world item/attack sheets that override it; skins/ holds one filmstrip per hero; monsters/ (and worlds/<W>/monsters/) hold one filmstrip per monster/villager.

# WoS A96 art formats — reference for a portable C99 reimplementation

Every claim is cited as `VA (symbol)` in `work/decomp/all.c`, or as `file:line` for the shipped docs/data.
Things I could not verify are marked **[UNVERIFIED]**.

---

## 0. Measurement caveat (read this first)

The assignment asked for BMP header dimensions measured with python. **I could not run any code**: my tool set has no shell/python, `read` on a `.bmp` drops NUL/control bytes (character count ≠ byte count), `grep` silently skips binary files, and the PE reader refuses `.exe` paths (`Path element starting with '.' is not permitted`). So **no pixel dimensions in this report were measured** — every size below is either (a) taken from the author's own readme, (b) a hardcoded constant in the code, or (c) derived from code. Section 9 gives the exact one-liner to generate the full table.

What *is* fully pinned down: the container format, the cell/filmstrip arithmetic, the transparency rule, the state machines, and the index→file mappings. Those are what the C port actually needs.

---

## 1. The one loader: everything is an 8bpp filmstrip

### 1.1 Entry point
`FUN_00486690 (0x486690)` — "load a picture from a path".
```
CFile::Open(path, 0xa0 /*typeOpenDefault*/)  ->  FUN_00486770  ->  FUN_00488080 (BMP) | FUN_00488510 (JPG)
```
* Sniffer `FUN_00489570`: if `buf[0..1] == 0x4D42` ("BM") and ≥14 bytes → **type 1 (BMP)**; else memcmp against a 4-byte marker at `DAT_004fda38` (JPEG SOI) anywhere in the first N bytes → **type 2 (JPG)**; else 0.
* Returns 1 on success, 0 on failure. Callers test the return and pop `"Unable to load button bar image"` etc. (0x4782xx).

### 1.2 BMP loader `FUN_00488080 (0x488080)` — exact requirements
```c
read 0x0e bytes; if (n != 14 || word != 0x4D42) fail;            // BITMAPFILEHEADER
read 0x28 bytes; if (n != 0x28) fail;                           // BITMAPINFOHEADER only
if (hdr[0] != 0x28 || hdr.planes != 1) fail;                    // biSize==40, biPlanes==1
nClr = FUN_00489140(hdr);        // 0x489140
if (nClr*4 > 12 && hdr.biCompression == 3) { vga fixup; nClr = nClr*4 - 12; }
if (nClr > 0x3ff) nClr = 0x400;
read nClr*4 bytes of palette into bmiColors (0x400 zeroed first)
FUN_00489370(hdr, hdc, &palette)  ;   // 0x489370  LOGPALETTE 0x300, entries from file palette
                                       //   (B,G,R from +2,+1,+0 of each 4-byte entry)
                                       //   or GetSystemPaletteEntries if the bitmap has <=1 colour
FUN_004864e0(hdr.biWidth, hdr.biHeight, 0, dc)   // 0x4864e0 -> CreateDIBSection, 8bpp, BI_RGB(0),
                                                 //   palette filled by FUN_004891a0 (system colours, RGB order),
                                                 //   SetDIBColorTable(dc, 0, 0x100, bmiColors+0x28)
n = min(biSizeImage, scratch_space);
ReadFile(pixels, n);  SetDIBits(memDC, hbm, 0, biHeight, pixels, hdr, 0)
```
**Consequence for a C port:** the engine wants an **8bpp, 1-plane, uncompressed (BI_RGB), bottom-up** DIB with its own 256-entry palette. The authors' readmes insist the files are RLE8; this loader does **not** decompress — it `ReadFile`s the pixel area and hands it to `SetDIBits`, which does not accept BI_RLE8. In practice GDI/SetDIBits on a `BI_RLE8` header is undefined, so treat the shipped files as "whatever `ReadFile` yields" and simply load the bytes verbatim; that is exactly what the game does. **[UNVERIFIED]** whether the shipped files are truly RLE-compressed (I could not read the headers).

### 1.3 The `Picture` object (0x18 bytes)
`FUN_004863d0 (0x4863d0)` ctor, `FUN_00486460` dtor. Layout:

| off | meaning |
|---|---|
| +0x00 | vtable (`PTR_LAB_004d02a8`) |
| +0x04 | `BITMAPINFO*` (0x428 bytes) |
| +0x08 | `void*` DIB bits |
| +0x0C | `HBITMAP` |
| +0x10 | `HDC` (memory DC) |
| +0x14 | previous `HGDIOBJ` selected into it |

Virtuals used: `+0x04` destroy, `+0x18` colour-keyed draw, `+0x20` BitBlt, `+0x24` stretch/fill-from-DC.
Helpers: `FUN_004864c0 (0x4864c0)` = **bitmap height** (`labs(bmi->biHeight)`), `FUN_004864e0 (0x4864e0)` = create, `FUN_00486640` = attach palette, `FUN_00487ad0` = stretch blit.

### 1.4 Transparency = one palette index, read from the bitmap's own bottom-left pixel
Software blitter `FUN_00486d20 (0x486d20)`, inner loop:
```c
cVar7 = (char)param_6;                 // the key
...
if (*_Src != cVar7) *_Dst = *_Src;     // 0x486d20, ~line 99157
```
Callers always compute the key the same way — *byte at (0, height-1)*:
* sprites: `FUN_0048dad9 (0x48dad9)`: `uVar1 = *(undefined1 *)((iVar5 + -1) * uVar8 + *piVar3);` where `uVar8 = (bmi->biWidth + 3) & ~3` (row stride).
* buttons (0x4784xx): `local_40 = (uint)*(byte *)((iVar4 + -1) * uVar8 + *(int *)(local_18 + 4));`
* objects: same pattern at 0x4a4xxx, 0x4d1xxx.

So: **key = the palette index of pixel (0, h-1)** of the sheet. Every art file must have its background in that pixel. (Also index 0 is special: `FUN_004224e5` never recolours index 0 — `bVar3 != 0` guard.)

Effect modes of `FUN_00486d20` (`param_7`, folded into `local_15` at entry):
| arg | effect |
|---|---|
| `param_6 < 0` | raw `memcpy` rows, no key |
| 0 | keyed copy (`local_15 = 0x01`) |
| 1 | silhouette: fill dest with `local_15` where source ≠ key |
| 2 | `local_15 = 0x10` |
| 3 | horizontal flip (`local_15 = -1`) |
| 5 | vertical flip (`local_15 = 0x27`) |
last arg = mirror-x; the function clips the source rect against the dest bitmap on all four sides.

### 1.5 The universal "filmstrip" rule
Every character/item sheet is a **horizontal strip of square cells of side h = bitmap height**; cell *i* is at `x = i*h`, `y = 0`, `h x h`. Count = `width / height` (stored in the record at `+0x484`):
```c
*(int *)(param_1 + 0x484) = iVar5 / iVar6;     // 0x48df3c:104636  (width / height)
```
Author's own words: *"The internal layout of a skin file is a horizontal strip of square images. However many pixels tall the BMP file is determines the size of the square."* (`extracted/skins/readme.txt`)
And quest.txt:851: *"you specified both an image file name, and an offset (pose) to a particular image of that file's filmstrip."*

---

## 2. Hero / player sprites (`skins\*.bmp`)

### 2.1 Path
`"%s\skins\%s.bmp"` (string `s__s_skins__s_bmp_004df614`), root = `DAT_005384d0`.
Built at 0x48df3c, 0x4A25xx (skin picker), 0x48de8d. Special case: the skin named `"mirror"` resolves to the local player's own skin name (`s_mirror_00502ce8`, 0x48de8d / 0x4A…104533).
Per-skin INI also exists: `"%s\skins\%s.ini"` (`s__s__skins__s_ini_005075e4`, 0x4A…118826) with a `[skins]` section (censorship flags) — **not** art.

### 2.2 Layout — 5 squares, width = 5h
```
+--------+--------+--------+--------+--------+
|  MAP   | READY  | ATTACK |  WEAK  |  CHAT  |      each square is h x h,  h = BMP height
| 3x3    |        |        |        |        |
+--------+--------+--------+--------+--------+
  0..h      h..2h     2h..3h    3h..4h   4h..5h
```
* The first square is subdivided **3x3 into (h/3)x(h/3)** map sprites: 8 walking directions + the centre image used while camping/fighting. The other four are full `h x h` scene views.
* Cell indices in h-units: `0,1,2` = the three map sub-columns, `1,2,3,4` = READY/ATTACK/WEAK/CHAT. (The code treats the map block as one h-wide square whose *left* part it never validates — see 2.4.)
* Validator `FUN_0048dd7d (0x48dd7d)`: requires `h > 0x23` (35) and that the band of rows `h-(h-h/6)+4 … 5h-4` is entirely the key colour (more than 1000 mismatches ⇒ reject the skin and fall back to `josh1`).
* h must be > 35 ⇒ 48 is the natural authoring value. **[UNVERIFIED]** I could not measure the shipped `h`.
* **Direction order inside the 3x3 is [UNVERIFIED]** — the decompiled map renderer I found (0x464xxx) selects a single h-cell with a 1-px jitter, and the field that would hold the direction (`+0x1b0`, clamped to 8) belongs to the *object* editor window. Row-major 0..8 (0=NW, 4=centre) is the only reading consistent with the readme, but treat it as unproven.

### 2.3 Loading / record layout — `FUN_0048df3c (0x48df3c)`
```
if (rec->type(+4) < 1) path = "<root>\worlds\<W>\monsters\<name>.bmp" else "<root>\monsters\<name>.bmp"  (FUN_0048de8d)
else                  path = "<root>\skins\<name>.bmp"
load; if (!ok || blacklisted || !FUN_0048dd7d) retry with "josh1" (s_josh1_00502cf0) from the monsters folder
FUN_0048dad9(picture, rec->type)          // strip borders / clear shadow band
rec->cellCount(+0x484) = width / height
if (rec->type == -1 && (s = rec->scale(+0x448)) && s < 9)  picture = FUN_0048d8f4(picture, s)
FUN_004224e5(picture, recolor)             // +0 if 0, else global direction/recolor
```
Record is **0x6e0 bytes** (`FUN_0048e16e`), name string at `+0x29`, `+0x11C` = source bitmap, `+0x120` = Picture, `+0x124` = pose, `+0x448` = size tweak, `+0x484` = cell count, `+0x48C/+0x4A0` = two CStrings (name1/name2 used to build the skin path, 0x48df3c:104615).

### 2.4 `FUN_0048dad9 (0x48dad9)` — border/shadow normalisation
Runs once after load, in place:
1. `key = pixel(0, h-1)`.
2. Paint the **bottom row of the whole sheet** with `key`; paint the **top row of every cell** (x = k*h, k = 0..cellCount) with `key`; paint the **left column of every cell** (y = 0..h) with `key`. → the 1-px cell separators become transparent.
3. If `type == -4`: force cell size to **48** (`local_1c = width/0x30; local_20 = 0x30`) regardless of height.
4. If `type > 0`: `y = h - (h - h/6)`; clear the two rows `y-1` and `y` for `x = h … 5h-1` — i.e. **the top two rows of the lower-1/6 band of the four view squares** (the game draws its own oval shadow there).
5. If `cellCount > 6`: clear that same row for `x = 6h … width-1` (extra frames of monster sheets; note cell 5 is skipped — off-by-one in the original, or a reserved "corpse" frame).

### 2.5 Recolor table — `FUN_004224e5 (0x4224e5)`
```c
bVar2 = pixel(0, h-1);                                  // key
for every pixel: if (px != key && px != 0) {
    if (param/1000 == 0 || ((row ^ col) & 1) == 0) {    // 50% checkerboard dither
        if (table[color*0x100 + px] == 6) *p = 0x6b; else *p = table[color*0x100 + px];
    } else *p = key;
}
```
* `color = param % 1000` (0..255), `param/1000` = dither flag.
* `DAT_005394b8` is a **64 KiB lookup table indexed `[target*256 + source]`** → 256 possible full-palette remaps. This is how "flashing red when hit", "black when dead", ghost/invisible, and elemental tinting are done without touching the file.
* Called with the global `DAT_004e18b8` (0/1) and per-attacker values (0x4B…102039, 102049, 0x48df3c:104647).

### 2.6 Scene pose
```c
rec->pose(+0x124) = 2;
if (hero present && rec->cellCount(+0x484) > 6) rec->pose = 6;      // 0x48…106063
```
Object/monster editor draws pose 0..3 side by side: `SetRect(&r,1,1,cell,cell); OffsetRect(&r, pose*cell, 0)` (0x49E911, ~line 116433).

### 2.7 Size tweak — `FUN_0048d8f4 (0x48d8f4)`
`n = rec->+0x448` in `-1..8`: if `n<1` scale = `(n+10)*10 %`, else `100 + n/4 %`; applied to both w and h; height clamped to ≥ 6 (then w=30).

### 2.8 There is **no** per-hero body/head composition at render time
A hero is one BMP. The only layered art is the "customise soul" preview: `suits.bmp`, drawn by `FUN_0040aec1 (0x40AEC1)`:
```c
iVar1 = height;                                  // cell = square of side h
SetRect(&src, 0,0,h,h); OffsetRect(&src, (n % 13) * h, 0);   // 13 per row
FUN_00487ad0(dc, &dst, &src);                    // stretch into a rounded, 3D-framed box
```
Indices 9..12 print a label instead of art (`DAT_004de054/58/5c/60`).

---

## 3. Monster / villager sheets (`monsters\*.bmp`, `worlds\W\monsters\*.bmp`)

* Resolution order `FUN_0048de8d (0x48de8d)`: `<root>\worlds\<W>\monsters\<name>.bmp` → `<root>\monsters\<name>.bmp`; `"mirror"` → the hero's own skin. (Author doc: `extracted/worlds/Evergreen/Monsters/readme.txt`.)
* Format is identical to skins: horizontal filmstrip of `h x h` cells, index = pose. quest.txt:802-804: *"These 'villager' skin files are stored in the world's MONSTER folder and are long filmstrips with many different villagers in a single file."*; quest.txt:1432 uses `ACTOR 1,"The Blind Sage", joshMiscellaneous, 32, 0, 60` → **cell 32**.
* The same loader/validator/colour pipeline is used (`FUN_0048df3c` with `rec->type < 1`).
* `monsters.txt` records: stride **0x248**, bmp name at `+0x25` (`FUN_004066a9 (0x4066A9)`), `+0` = valid flag, `+0x10` = 1 for live.
* Actor positions in a scene are **0..100 in x and y**, normalised to the window (quest.txt:806).

---

## 4. Map and overlay art

| file | role | code |
|---|---|---|
| `maps\<map>.jpg` | the map image, shown in the mini-map and stretched **×4** in the walking view; mandatory | 0x41F2xx |
| `maps\<map>X4.jpg` | optional hi-res, **exactly 4× w and 4× h**; used if present | `s__s__sX4_jpg_004e22c4`, ~23238 |
| `maps\<map>.obl` | link placement (LINK editor) | `s__s__s_obl_004e2278` |
| `maps\<map>.ter` | terrain / boot requirements | `s__s__s_ter_004e2250` |
| `maps\<map>.mon` | monster placement | `s__s__s_mon_004e225c` |
| `maps\OBJECTS.JPG` or `OBJECTS.BMP` | overlay objects (trees, towns, doors) pasted on the map; **BMP wins if both exist** | `s__s__objects_bmp_004e2324` / `_jpg_004e2314`, ~23161 |
| `maps\OBJECTS.OBR` | names rectangles of the object sheet | `s__s__objects_obr_004e2304` |

Object rendering (0x464xxx, the big map paint):
```c
SetRect(&dst, (objx - camx)*4 - 0x10, (objy - camy)*4 - 0x10,
              (objx - camx)*4 + 0x10, (objy - camy)*4 + 0x10);   // 32x32 dest = 4x zoom of one cell
FillSolidRect(..., 0xff / 0x80);                                 // backing, flashes on tick&0x100
... vtable+0x20 -> BitBlt
InflateRect(-0x10,-0x10); InflateRect(size<<2, size<<2);
SelectObject(GetStockObject(5 /*NULL_BRUSH*/)); Ellipse(...);      // the oval shadow, pen 0xff/0x80
```
Source cell: `SetRect(&src,0,0,h-1,h-1); OffsetRect(&src, h, (objid + GetTickCount()/500 & 3) == 0)` — i.e. one h-cell, with a 1-px vertical jitter that walks a quarter of the objects every 500 ms. **[UNVERIFIED]** the exact cell-index source for a given object (it comes out of the `.OBL`/`.OBR` data through `FUN_00463630`).
Per-map object instance table: 16-byte stride records at `DAT_00636808 … DAT_00679E28` = `[id, x, y, size, Picture*]`.
Map scaling constants computed at load: `DAT_004df8ac = w/4`, `DAT_004df8b0 = h/4`, `DAT_00539388 = w/2`, `DAT_00539384 = h/2` (~23220).
`extracted/worlds/Evergreen/maps/` also holds `objects.bmp`, `Image1.bmp` and every `*X4.jpg` hi-res pair.
Scenes: `scenes\<name>.jpg`, **360 × 256** (`extracted/worlds/Evergreen/scenes/readme.txt`); resolution prefers `worlds\<W>\scenes\` over root (`s__s_worlds__s_scenes__s_jpg_004f1c38` / `s__s_scenes__s_jpg_004f1c24`, 0x4F1C38 region).

---

## 5. UI art in `art\`

### 5.1 Path resolution (this governs every `s_*_bmp_*` symbol)
`FUN_00424526 (0x424526)`: `sprintf(buf, "art\\%s", name)` then `FUN_0042445e`.
`FUN_0042445e (0x42445E)`, in order:
1. `<root>\worlds\<W>\<rel>` if the world name is set and the file exists;
2. `<root>\themes\<THEME>\<rel>` if a theme is selected and the file exists;
3. otherwise `<root>\<rel>`.
So the string `s_buttonBar_bmp_004f95c8` = `buttonBar.bmp` under those rules.
`FUN_0041e1aa (0x41E1AA)` is the same idea for world files: tries `<root>\worlds\<W>\<rel>`, returns true/false, and leaves `<root>\<rel>` in the buffer as the fallback (used for `attack%02d.bmp`, `effects%02d.bmp`, `slots.bmp`, `slotCover.bmp`, `items*.bmp`).

### 5.2 Buttons — horizontal strip of **48×48 cells**
Paint loop 0x4782xx / 0x4783xx (the `FUN_0047844D` family):
```c
iVar4 = <state>;                                   // 1..5
SetRect(&src,0,0,0x30,0x30); OffsetRect(&src, iVar4 * 0x30, 0);
FUN_004787b2(&btnRect, index);                     // 0x4787B2, screen rect of button i
key = pixel(0, h-1);                               // local_40
(**(this+0x18))(dc, btn.left, btn.top, &src, key); // colour-keyed draw
```
State machine (verbatim logic):
```c
if (btn->state == 3) { iVar4 = 5;                                  // "temporarily disabled"
    if (btn->ts == 0 && GetTickCount() - btn->ts2 > 500) btn->state = 2; }
else if (mouseOver == i) { iVar4 = 3;                               // highlight
    uVar8 = (GetTickCount() & 0x300) >> 8;                         // 0..3, 256 ms period
    if (uVar8 == 2) iVar4 = 4; else if (uVar8 == 3) iVar4 = 5; }   // blink A / B
else if (btn->state == 2) iVar4 = 2 - (blinkOn);                   // 2 or 1
else iVar4 = 1;                                                    // up
```
| cell | meaning |
|---|---|
| 0 | unused (no code path selects it) |
| 1 | up (normal) |
| 2 | down / active (the button's `defaultState`, set at registration) |
| 3 | mouse-over highlight |
| 4,5 | blink phases A/B (also the "disabled" look) |

Registration `FUN_00478673 (0x478673)`: `(index 0..9, "buttonX.bmp", defaultState, flag, id, cmdId)`; stores the name at `this + index*0x105 + 0x1A4`, the bitmap at `this + 0xB8 + index*0x18`, the state word at `this + 0xC00 + index*4`. Empty name ⇒ invisible button ⇒ the painter draws a 3-D placeholder (`FillSolidRect 0x8000` + `Draw3dRect 0/0xC0C0C0`).

Button layout `FUN_004787b2 (0x4787B2)`: 10 slots, laid right-to-left from the client rect:
```
left = client.right - 48 - 3 - index*0x33      (0x33 = 51 = 48 + 3 px gap)
top  = client.top + 8,  bottom = client.top + 0x38   (48 tall)
```
Hit-testing `FUN_0047876B (0x47876B)` = `PtInRect` over all 10.

Which buttons exist (all registered in `FUN_0047…`, ~88860-89070): `buttonIncarnate 0x486, buttonHaunt 0x48F, buttonNew 0x489, buttonRestore 0x488 (defaultState 3), buttonMap 0x487/0x484, buttonItems 0x47E, buttonSpells 0x47D, buttonEquip 0x47C, buttonStats 0x485, buttonCamp 0x482, buttonExit 0x480, buttonCurse 0x490, buttonBless 0x491, buttonWell 0x48B, buttonShop 0x48A, buttonMission 0x53C, buttonHunt 0x4CB, buttonGame 0x493, buttonFlee 0x47F`, plus `buttonBar.bmp` (bar background, drawn through vtable `+0x24` at (0,0)) and `buttonBarLeather.bmp`.

### 5.3 Dialog backgrounds — `bk*.jpg` are 256×256 tiles
Every dialog does: load `bk*.jpg`, create a **0x100 × 0x100** scratch DIB, blit/pattern the background through it:
```c
FUN_00424526(buf, s_bkEquip_jpg_004de214);  bg = FUN_00486690(buf, dc);
FUN_004864e0(0x100, 0x100, 0, dc);          // 256x256 scratch
```
Mapping (all cite-verified): `bkEquip` 0x40C371 & 0x4x, `bkSpell` 0x5x…, `bkDemon` 0x4F85A0, `bkBook` 0x4F2D00, `bkBeg` 0x4DDB4, `itemTable.jpg` (item/shop list) 0x4F1C74, `quadris.jpg` 0x4F6EC8, `petPen.jpg` 0x4DEE30, `pickSoulLabel.jpg` 0x4DF600, `createSoul.jpg` 0x504E28, `splash.jpg`, `title.jpg`, `beg.jpg`, `where.jpg`, `chapter.jpg`, `death.jpg`, `earth.jpg` (title-screen sequence, case 0..9 at 0x4B…21081-21170).
Window borders: `shopBorder256.bmp` (5 call sites), `shopSquareBorder256.bmp`, `missionBorder256.bmp`, `mapBorder256.bmp` — all loaded through the `art\` resolver, i.e. theme-overridable.

### 5.4 Item icon sheets (the `worlds\W\art\` filmstrips)
Author's table (`extracted/worlds/Evergreen/items.txt:77-91`), which is exactly what `FUN_00482431 (0x482431)` implements (cases 0..13 on `itemClass-10`, default string `DAT_004f1c6c`):

| class | sheet | cell (readme) |
|---|---|---|
| 0 POTION, 1 ANTIDOTE, 2 SPECIAL, 3 EXIT, 4 TRAVEL, 5 THROWABLE\* | `items.bmp` / `darts.bmp` | 48×48 |
| 10 EQUIP_HELMET | `helmets.bmp` | 48×48 |
| 11 EQUIP_ARMOR | `armor.bmp` | 48×64 |
| 12 EQUIP_RIGHT_1 | `swords.bmp` | 48×64 |
| 13 EQUIP_RIGHT_2 | `staffs.bmp` | 48×64 |
| 14 EQUIP_RIGHT_3 | `bows.bmp` | 48×48 |
| 15 EQUIP_RIGHT_4 | `music.bmp` | 48×48 |
| 16 EQUIP_RIGHT_5 | `right5.bmp` | 48×48 |
| 17 EQUIP_RIGHT_6 | `right6.bmp` | 48×48 |
| 18 EQUIP_RIGHT_7 | `right7.bmp` | 48×48 |
| 19 EQUIP_RIGHT_8 | `right8.bmp` | 48×48 |
| 20 EQUIP_BOOTS | `boots.bmp` | 48×48 |
| 21 EQUIP_SHIELD | `shields.bmp` | 48×64 |
| 22 EQUIP_RING | `rings.bmp` | 48×48 |
| 23 EQUIP_AMULET | `amulets.bmp` | 48×48 |
(\* class 5 is documented as `darts.bmp`; classes 2/3/4 shown as `items.bmp`.) **In code the cell width is the constant 0x30 = 48** (`FUN_00466931`, `SetRect(&r,0,0,0x30,0x30)`), so the readme's "48×64" sheets are drawn as 48×48 rows — a faithful port should keep `48 x rowHeight` with rowHeight taken from the file, or 48/48 if matching the original.

**Index mapping** (`items.txt:115-134`, arg 3 = `imageNumber.extensionNumber`):
* `imageNumber` (documented 0..63) = the cell index in the class filmstrip, counted from the left; **index 0 is the "empty-handed" placeholder** and is not a legal item image.
* `extensionNumber` (1..9, since A77) → the file is renamed `"%s_%d.bmp"`; code at ~22907:
  ```c
  if (ext == 0) sprintf(path, "%s.bmp", base); else sprintf(path, "%s_%d.bmp", base, ext);
  ext++;   // try the next extension as a fallback
  ```
* Max practical sheet width 4096 px (≈85 cells at 48 px) — the author's own guidance, note the doc says "each item is 64 pixels wide" while the code uses 48.
* The list control shows `"%s%d"` = `<sheet name><image number>` (0x4F…68663-68665) so the player sees e.g. `swords3`.

**The 10-slot paper-doll grid** — `FUN_00466931 (0x466931)`:
```c
for (slot = 0; slot < 10; slot++) {                 // dst rects stride 0x10
    img = slotItemNumber[slot];
    for (k = 0; k < 4; k++) {                       // 4 stacked 48x48 cells
        SetRect(&src, 0,0,0x30,0x30);
        row = DAT_00683c08[slot*400/4 + ((img/0x30 + k) % 100)];   // 10 x 100 int table
        if (row >= 0 && row < 8) { OffsetRect(&src, 0, row*0x30); ...draw... }
    }
}
```
i.e. each item sheet is also a **grid: 8 sprite rows of 48 px, 48 columns**, and a per-slot 100-entry table (`DAT_00683c08`, 400-byte stride, `% 100`) picks the row for each of the 4 stacked variants. The rows are the 8 map directions. **[UNVERIFIED]** the contents of that table (data section).

### 5.5 attack / effect animation sheets
`items.txt:358-363` is the spec, and the code matches it exactly:
> *"Each image is an eight-frame animation stored in a file named attackNN.bmp. Each file contains 10 such filmstrips where the topmost filmstrip is the '0' index of that file. SO, image 143 would be index '3' in the file attack14.bmp. The first one hundred images (files 00-09) are reserved for [the author], while the rest (attack10.bmp to attack19.bmp) are reserved for world designers (and go in your world's ART folder)."*

Loader `FUN_004a38ec (0x4A38EC)`: loops `sprintf(name,"attack%02d.bmp", i)` and `"effects%02d.bmp"`, i = 0..19, into two tables of **20 `Picture`s with 0x18 stride** at `DAT_00d76fe8` (attack) and `DAT_00d66dd8` (effects) — only 00..09 exist in the root `art\`, so slots 10..19 stay empty until a world supplies them.
Consumer (0x48D…104058-104213):
```c
rows   = height / 0x30;                       // 10 for a full sheet
picIdx = image / 10;  strip = image % 10;     // which Picture
col    = 7 - (framecount & 7) ...             // frame 0..7, counted DOWN
row    = strip % rows;
SetRect(&src,1,1,0x30,0x30); OffsetRect(&src, col*0x30, row*0x30);
FUN_00486d20(pic, x, y, &src, key= pixel(0,h-1), effect, mirror);
```
So: **cell 48×48, 8 columns × 10 rows per file, frame index runs right-to-left (col = 7-n)**. Special effect codes 0x1E/0x1F/0x20/0x21 (walk-attack, sin-bob, shake) pick which `Picture` to use.
`arg16` of items.txt also carries `flags`: bit 2 = "each of the 8 frames consumes 1/8 of the attack" instead of looping, bit 3 = don't mirror horizontally; and `attackPath` 1..7 = LUNGE/JUMP/LEAP/STAB/DBL-STAB/TRAMPLE/HOP, 30..33 = ARROW/STONE/LOB/Swoop (items.txt:336-357).

### 5.6 Other `art\` files and their jobs
| file | how it is used | cite |
|---|---|---|
| `buttonBar.bmp` / `buttonBarLeather.bmp` | bar background, vtable `+0x24` at (0,0) | 0x478350 |
| `bk*.jpg` | 256×256 dialog backgrounds | §5.3 |
| `*Border256.bmp` | 9-scaled window frames (`Draw3dRect`-style stretches) | 0xDC5E8, 0xDF668, 0xF132C, 0xE1ED8 |
| `equipIcons.bmp` | `LoadImageA(NULL, path, 0,0,0, LR_LOADFROMFILE)` → `CImageList::Create(142,16,1,0x808000)` and `(176,16,…)` | 0x40C371:9115, 0x55923:55923, 0x69137 |
| `slots.bmp` + `slotCover.bmp` | equip-screen slot frames, world-first then root | 0x4F2440, 0x4F242C (73746) |
| `misc32.bmp` | 256×256 scratch/compose sheet for small UI icons | 0x4E1CB8, 0x4F1D00 |
| `tree.bmp` | the wood-grain filler of the info pane, drawn 3× at `+0x64`,`+0x74`,`+0x84` | `FUN_004245d1 (0x4245D1)` |
| `suits.bmp` | soul-customisation preview, 13 per row, cell = height | 0x40AEC1 |
| `spells.bmp`, `disease.bmp`, `trophy*.bmp`, `music.bmp`, `petButtons.bmp`, `redButtons.bmp` | per-screen art, all via the `art\` resolver | 0x4E1D…, 0x4F8CFC |
| `souls.pal` | the **authoring** palette only — the game never reads it; it comes from each BMP's own palette | grep: no `.pal` reference in the exe |
| `questdiary.gif/.jpg`, `picksoullabel.jpg`, `itemTable.jpg`, `quadris.jpg`, `petPen.jpg`, `createSoul.jpg` | static labels/backgrounds | as cited |

---

## 6. `themes\` and `skins\` folders

* `skins\` (root, 82 files) — one hero filmstrip per file; heroes are world-independent so they live at the root. Author rationale + full layout spec: `extracted/skins/readme.txt`. Many files ship as `Name.bmp` + `Name2.bmp` pairs (the "2" variant is the recoloured/alternate one, selected by the same skin name resolution).
* `skins\<name>.ini` — per-skin metadata (`[skins]` section, censorship flags), read/written with `Get/WritePrivateProfileString` (0x4A…118826/118925).
* `themes\<Name>\` — user UI themes. `themes/readme.txt`: *"add a folder … name it the name of your theme … inside make three folders ART (bitmap files), SFX (WAV), MIDI (MID). These mirror the folders of the same name in your WoS directory. To override some art or sound asset just place an equivalent file of the same name into your theme's appropriate sub-folder. … Be sure to keep bitmap files the same size as the file they are replacing."*
  * Theme resolution is step 2 of `FUN_0042445e (0x42445E)`; the theme name lives at `DAT_004e18d0`, the world name at `DAT_004e0bd0`, the root at `DAT_005384d0`.
  * Shipped example `themes/Classic/`: `Art/` (11 files — `buttonStats/Well/Map/New/Restore/Shop/Spells/Hunt/Incarnate.bmp`, `itemTable.jpg`, `createsoul.jpg`, `bkDemon.jpg`) and `sfx/` (`MessageBox.wav`, `OpenDialog.wav`, `ButtonClick.wav`, `CloseDialog.wav`). No MIDI folder.
  * Note the art resolver is case-sensitive path building with `_access` probing, and the readme warns the replacement must match the original's byte size (the game never verifies it, but MFC resource/dialog sizing may).

---

## 7. Fonts

* Face name string `s_Tempus_Sans_ITC_004dc5b0` = **"Tempus Sans ITC"**; shipped as `extracted/tempsitc.ttf`.
* Registration `FUN_00408a6e (0x408A6E)`:
  ```c
  EnumFontFamiliesA(dc, "Tempus Sans ITC", &cb, &found);
  if (found == 0) { sprintf(buf, "%s\\%s", dir, "tempsitc.ttf"); AddFontResourceA(buf); }
  ```
  i.e. the TTF is private-loaded only when the family is not already installed.
* Creation is always `CFont::CreatePointFont(n, "Tempus Sans ITC", NULL)`; sizes used: **110 (0x6E)** for nearly every dialog, 120 (0x78), 130 (0x82), 100, 200, and 90 (0x5A) paired with `"Arial"`. Also `s_Arial_004dc894`, `s_Verdana_004ed690`, `s_Verdana_bold_004dd0a0`.
* Decorative text writer `FUN_0049c0b0 (0x49C0B0)` = `(dc, rect, string, colour, face, pointSize, bold, flags)` with `flags` typically `0x900`; used for every label.

---

## 8. Minimal C99 data model implied by all of the above

```c
typedef struct { int w, h; uint8_t px[]; } Sheet;      /* 8bpp, key = px[(h-1)*stride] */

typedef struct {                   /* "Picture", 0x18 bytes in the original */
    Sheet *sheet;                  /* +0x04 header, +0x08 bits               */
} Picture;

/* cell arithmetic used everywhere */
static int cell_of(Sheet *s)            { return s->w / s->h; }              /* == h-sized cells */
static void cell_rect(int i, int h, int *r) { r[0]=i*h; r[1]=0; r[2]=i*h+h-1; r[3]=h-1; }
static uint8_t key_of(Sheet *s)         { return s->px[(s->h-1)*((s->w+3)&~3)]; }

/* skins: 5 squares of side h; square 0 is the 3x3 map block */
#define SKIN_W(h)  (5*(h))
#define SKIN_MAP_SUB(h) ((h)/3)
#define SKIN_READY  (1*h)  /* 2*h ATTACK  3*h WEAK  4*h CHAT, each h x h      */

/* items: horizontal filmstrip, cell 48 wide; 9 extensions "<base>_<n>.bmp"   */
/* attacks: 8 cols x 10 rows of 48x48 in attackNN.bmp; image N -> (N/10, N%10) */
```

---

## 9. How to generate the measured table (what I could not run)

```bash
cd /home/jshield/src/games/WoS
python3 - <<'PY'
import struct,glob,os
def bmp(p):
    d=open(p,'rb').read(54)
    if d[:2]!=b'BM': return None
    off,=struct.unpack_from('<I',d,10); hs,=struct.unpack_from('<I',d,14)
    w,h,pl,bpp,comp,=struct.unpack_from('<iiHHI',d,18)
    return off,hs,w,h,pl,bpp,comp
rows=[]
for p in sorted(glob.glob('extracted/**/*.bmp',recursive=True)):
    r=bmp(p)
    if not r: continue
    off,hs,w,h,pl,bpp,comp,=r
    rows.append((p,os.path.getsize(p),w,h,bpp,comp,off,w//h if h else 0))
print(f"{'file':52}{'bytes':>9}{'W':>6}{'H':>6}{'bpp':>4}{'comp':>5}{'cells':>7}")
for p,sz,w,h,bpp,comp,off,c in rows:
    print(f"{p:52}{sz:9}{w:6}{h:6}{bpp:4}{comp:5}{c:7}")
PY
```
Checklist when running it: (a) every `skins/*.bmp` and `monsters/*.bmp` should report `W == 5*H` and `H > 35`; (b) `art/attack??.bmp` and `effects??.bmp` should report `W == 8*48` and `H == 10*48`; (c) `art/button*.bmp` should report `H == 48` and `W == 48 * (number of states + 1)` (4, 5 or 6); (d) `art/buttonBar*.bmp` should be a full-width bar; (e) `bmp` should report `bpp == 8` and `comp == 0` — **if `comp == 1` (BI_RLE8) then the shipped files really are RLE and the engine's `SetDIBits` path is relying on undefined behaviour, in which case a faithful port must implement an RLE8 decoder**; (f) every sheet's `px[0 + (H-1)*stride]` is its transparency key.


## Sources

- `work/decomp/all.c`: Ghidra decompilation of Souls.exe (138k lines, '// ==== <VA> <name> ====' per function). Primary source for every claim in the report.
- `work/decomp/functions.tsv`: VA / name / size index of all 3991 functions; use to locate any VA cited below.
- `extracted/skins/readme.txt`: Authoritative skin BMP layout: 5 squares [MAP][READY][ATTACK][WEAK][CHAT], square size = BMP height, MAP is 3x3 sub-squares, lower 1/6 is shadow, RLE 8bpp with souls.pal.
- `extracted/worlds/Evergreen/art/readme.txt`: World art folder doc: item filmstrips are horizontal strips, image N = Nth cell; per-file cell sizes 48x48 / 48x64.
- `extracted/worlds/Evergreen/items.txt`: items.txt:50-135 class->BMP table, arg3 = imageNumber.extensionNumber (0 = empty-handed, up to 9 extensions, xxx_1.bmp..xxx_9.bmp).
- `extracted/worlds/Evergreen/items.txt`: items.txt:330-372 arg16 = attackPath.imageID.flags.weather.effect; 'Each image is an eight-frame animation stored in a file named attackNN.bmp. Each file contains 10 such filmstrips'.
- `extracted/worlds/Evergreen/quest.txt`: quest.txt:795-805,851,1430-1432 ACTOR <id> <name> <skin> <pose> <x,y>; pose = filmstrip cell index; x,y are 0-100 of window size.
- `extracted/worlds/Evergreen/maps/readme.txt`: Map art doc: mapName.JPG mandatory, X4 optional hi-res (exactly 4x), OBJECTS.JPG or OBJECTS.BMP + OBJECTS.OBR, .obl/.ter/.mon.
- `extracted/art`: 84 files: button*.bmp (15), buttonBar*.bmp, bk*.jpg, attack00-09.bmp, effects00-09.bmp, 12 item sheets + suits/misc32/slots/slotCover/equipIcons/tree/spells/trophy*/disease/music/right5-8, souls.pal, *Border256.bmp.
- `extracted/skins`: 82 hero skin filmstrips (hero1-7, named heroes, many 'Name'/'Name2' duplicates for alternate colouring).
- `extracted/monsters`: 127 shared monster/villager filmstrips (spooks, villagers, petK*, ben*, …).
- `extracted/themes/Classic`: Theme override folders: Art/ (11 files), sfx/ (4 wavs), readme.txt.
- `extracted/themes/readme.txt`: themes/readme.txt: each theme folder mirrors art/, sfx/, MIDI/ by name; replacements must keep identical file sizes.
- `extracted/tempsitc.ttf`: Tempus Sans ITC TrueType, registered with AddFontResourceA at startup by FUN_00408a6e.
