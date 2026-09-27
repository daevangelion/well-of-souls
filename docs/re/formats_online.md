# WoS data formats (online + local docs)

## Summary

Produced a dense, source-cited technical reference for Well of Souls (Evergreen, A96) data formats, grounded primarily in the world's own self-documenting text files plus the official synthetic-reality.com developer pages. Covers: MAPS table + maps.txt link scheme; the .TER (Windows BMP terrain map, 10 terrain ids), .OBL (link-point table, fixed-size ~200 KB) and .MON (monster placement, fixed-size ~269.5 KB) binaries; the full QUEST scene language (SCENE/END, ~70 commands, GOTO/labels, IF condition grammar, ACTOR/MOVE/POSE/N:, FIGHT, cookies, math, CALL/RETURN) and the #include table structure; monsters.txt 21-column layout and monster BMP frame layout; hero skin RLE-BMP frame layout (3x3 walking map + READY/ATTACK/WEAK/CHAT/CREDITS), the 6-px and multiple-of-6 sizing rules and the control-pixel transparency convention; the .her save path/solo flow/first map/battle ATB model; and world.ini, config.ini, music.ini, slots.ini, gender.ini, levels.txt, items.txt, spells.txt, groups.txt, trophies. The three map binaries and .her are the only formats that could not be byte-verified: this environment's executable/PE reader is broken and grep skips binaries, so no hexdump or disassembly was possible; those sections are explicitly labelled [UNVERIFIED] with the evidence that does exist (BMP magic, fixed file sizes, printable-string content) and a concrete decoding plan.

## Architecture

WoS data is layered as: (1) a text world definition — `quest.txt` plus `#include`d tables (maps/monsters/items/spells/levels/groups/trophies) and INI overlays (world/config/music/slots/gender/stocks/missions); (2) binary per-map link/terrain/monster data (`.obl`/`.ter`/`.mon`) in `MAPS\`, keyed by the MAPS table's `rootName` arg2, which is simultaneously the OBL/TER/MID root and the `music.ini` section name; (3) raster assets — RLE-8 BMP filmstrips for items (`ART\`), hero skins (`SKINS\`, global), monsters/villagers (`MONSTERS\`, world-then-global lookup), scene JPEGs at 360x256 (`SCENES\`), map JPEGs (+ optional X4 high-res) in `MAPS\`; (4) `OBJECTS.BMP`+`OBJECTS.OBR` providing the link-point icon filmstrip and its named rectangles. Scripts (`+SCENES`) are event-driven token machines, not turn-based; battles are real-time with per-hero charge meters rather than an ATB queue. For an offline port the natural cut is: parse quest.txt tables, render maps from `rootName.jpg` with `rootNameX4.jpg` and `OBJECTS.BMP` tiles, walk links from `.obl`, gate terrain from `.ter`, spawn monsters from `.mon`/GROUPS, and drive dialog/battles from the scene VM.

# Well of Souls — Data Format Reference (Evergreen, Souls.exe A96)

Scope note on evidence: every claim below is tagged with either a **local path + line range** or a **URL**. Formats that could not be verified byte-for-byte are marked **[UNVERIFIED]** with the evidence that does exist and a decoding plan.

**Tooling limitation that shaped this report:** in this environment `read` on `Souls.exe` / `Mix.exe` (PE) fails with `java.lang.IllegalArgumentException: Path element starting with '.' is not permitted` (reported via `xd://report_issue`), and `grep` silently skips all binary files. So no hex-dump or disassembly of the map binaries or the engine was possible. Binary claims below rest on: (a) file sizes from directory listings, (b) the printable-ASCII projection of `read :raw`, (c) the shipped `readme.txt` files, which are the vendor's own format spec.

---

## 1. Maps

### 1.1 The MAPS table (the linking scheme)

`quest.txt` `+MAPS` is an empty shell; the data is `#include`d:
```
+MAPS
#include maps.txt
-MAPS
```
(`extracted/worlds/Evergreen/quest.txt:296-303`)

Column order, per the table's own header comment (`quest.txt:288`):
```
;  arg0   arg1             arg2             arg3                    arg4       arg5
;  ID     image file       OBL/TER/MID root Map Name                Map FLAGS  Map Theme
```
Semantics (`quest.txt:220-286`):
* `arg0` map ID **0-999** (0 = main world map; link points are addressed by map number).
* `arg1` background image file name (JPG, or RLE BMP ok).
* `arg2` **root name** — no extension. It is simultaneously the OBL file, the TER file, and the *old-style* MIDI name, and is the **section name in `music.ini`**. If a `.ter` exists it must share this name.
* `arg3` display name, quoted if it has spaces.
* `arg4` bitmask map flags (decimal or `0x…` since A77). Full table at `quest.txt:240-286`:
  `1` pet-vs-pet · `2` pets vs players/pets · `4` no-PKer-protection · `8` no PK at all · `16` ladder reporting · `32` show all link names · `64` no shadows · `128` no pets · `256` no eavesdropping · `512` guild hall · `1024` humans can't hurt monsters/pets · `2048` NO_REWARD · `4096` NO_HEAL (heals give 1 HP) · `8192` NO_TICKETS · `16384` NO_MINI_MAP · `32768` NO_STUN · `65536` NO_BUFF · `131072` NO_PKREZ · `262144` hide link boxes until visited · `524288` no waypoints · `1048576` no items in scene.
* `arg5` sound theme 0-255 while walking on the map (see §6 THEMES).

Actual Evergreen rows (`extracted/worlds/Evergreen/maps.txt`):
```
	0,		evergreen.jpg,		evergreen,			"Evergreen",				131072
	1,		stonehenge.jpg,		stonehenge,			"The Rune Ruins",		131072
	2,		castle1.jpg,		castle1,			"Macgyver Castle",		8
	5,		town2.jpg,			westin,				"Westin",				8
	16,		arenaTown.jpg,		pkarena,				"Field of Honor",		22
```
Note maps 16 and 17 **share** `arenaTown.jpg` and differ only by root name (`pkarena` / `petarena`) — proof that the JPG name and the OBL/TER root name are independent axes.

The file's own trailing comment is a load-bearing warning for a parser: *"Please leave some blank lines at the end to avoid running into a nasty bug"* (`maps.txt:22-25`).

### 1.2 Map background images

`extracted/worlds/Evergreen/maps/readme.txt:14-46`:
* `mapName.JPG` is **mandatory**. It is shown in the upper-right minimap, and it is **stretched ×4** to fill the walking view — *unless* an optional high-res file is present.
* `mapNameX4.JPG` is **exactly 4× the width and 4× the height** of the low-res JPG (verified in-tree: `evergreenX4.jpg`, `grottoX4.jpg`, `fireAndIceX4.jpg`, `rustRockX4.jpg`, `island1X4.jpg`, `arenaTownX4.jpg`). Requires A30+.
* Recommended size: **512×512** for the main world map (`maps/readme.txt:11-13`); `wosDev1.htm` says "on the order of 768 pixels wide and/or tall. (The larger your map is, the slower it will load)".
* The low-res JPG is always required even when the X4 exists.

Actual JPEG pixel dimensions are **[UNVERIFIED]** — no image-header reader available here. `read :raw` on a JPEG does not expose the SOFn marker legibly.

### 1.3 `.ter` — terrain map

**Confirmed: it is a Windows BMP.** `read .../evergreen.ter:raw` begins `B` `M` (`0x42 0x4D`) — the BITMAPFILEHEADER magic — followed by a DIB header whose size field renders as `0x28` (40 = `BITMAPINFOHEADER`) and then a width field whose low byte renders as `0x36` (54). The `readme` calls it "a file created by the terrain editor which indicates areas which require special boots (or objects) to be crossed" (`maps/readme.txt:30-32`).

Sizes (`extracted/worlds/Evergreen/maps/` listing):

| file | size |
|---|---|
| `evergreen.ter` | 37.1 KB (main world map — biggest) |
| `stonehenge.ter`, `rustRock.ter`, `roundtree.ter`, `inferno.ter`, `animalKingdom.ter` | 11.6 KB (identical) |
| `floodedMaze.ter` | 9.0 KB |
| `wormCave.ter` | 8.6 KB |
| `castle1.ter` | 8.2 KB |
| `pkarena.ter`, `petarena.ter` | 3.7 KB (identical) |

**Cell size / bpp / palette: [UNVERIFIED].** What is known: the per-cell value is a **terrain id 0-9**, defined by the `+TERRAINS` table:

```
+TERRAINS
;	arg0	arg1				arg2	arg3
	0,		"No Hindrance",		0,	0
	1,		"Lava",			10,	0
	2,		"Electricity",		10,	0
	3,		"",			0,	0
	4,		"Deep Water",		0,	0
	5,		"Clouds",		0,	0
	6,		"",			0,	0
	7,		"",			0,	0
	8,		"Healing",		-10,	0
	9,		"Impassable",		0,	0
-TERRAINS
```
(`quest.txt:125-148`; `arg0` id, `arg1` name, `arg2` damage on step — *noted in-file as "terrain damage not yet implemented"*, `arg3` optional token that permits crossing). Terrain 0 is always passable. The in-file note "Up to 10 terrains (0-9) may be defined and then 'painted' onto maps" plus the fact that exactly 10 ids exist is consistent with a **4-bit-per-cell** encoding, but that is inference, not measurement.

**Decoding plan (cheap, no engine RE needed):** parse the BMP DIB header directly (it is a standard BITMAPINFOHEADER), read `biWidth`/`biHeight`/`biBitCount`/`biClrUsed`, and check whether `biWidth == jpeg_width` and `biHeight == jpeg_height` for the matching `mapName.jpg`. If heights are positive the rows are bottom-up; if negative, top-down. Then index the palette / nibble pairs against the `+TERRAINS` ids 0-9. Cross-check with the rendered `evergreen.jpg` (it is a 3-D rendered world map, so terrain blobs should line up with the visible lava/water/desert regions).

### 1.4 `.obl` — link points

`readme` (`maps/readme.txt:24-28`): *"A file created by the LINK editor which defines where links are placed on the base file, and special aspects about each link (whether it links to a scene, another map, etc.)"*

**Structural fact (measured, high confidence): all 20 `.obl` files in Evergreen are exactly the same size — 200.0 KB** (`maps/` listing; `evergreen.obl`, `stonehenge.obl`, `castle1.obl`, `westin.obl`, `stonetree.obl`, `shrimpee.obl`, `grotto.obl`, `rustRock.obl`, `roundtree.obl`, `springwell.obl`, `floodedMaze.obl`, `wormCave.obl`, `inferno.obl`, `isleLight.obl`, `NorthUmbrage.obl`, `pkarena.obl`, `petarena.obl`, `animalKingdom.obl`). A fixed size strongly implies a **fixed-slot table padded to capacity**, not a variable record stream. Exact byte count needs `stat` (**[UNVERIFIED]**; the directory reader rounds).

**Content fact (measured):** the printable-ASCII projection of `evergreen.obl` (via `read :raw`) shows a sequence of null-separated-ish strings that are recognisable game data, e.g. the first records render as:
```
map0 greek1 Gateway of Dreams #nochange
map0 cloudyMountain Springwell fight1.mid
map0 3soho Hampton fight2.mid
...
map1 henge1 The Rune Ruins fight2.mid
...
scene17 Macgyver Castle fight1.mid
...
scene4  fight4.mid
scene10 Goblin Camp mortal.mid
beach  fight3.mid
giza5  fight4.mid
aztec Inferno Temple fight4.mid
```
and `stonehenge.obl` renders a *different* head:
```
map0 aztec Temple Exit Q6 henge1 Rune Well 3[ henge1 F3q! henge1 ...
... scene8 ... bwOilFire ... scene6 ... karnak ... cloudyVeldt ...
```
Two conclusions follow, both [UNVERIFIED] as to exact byte offsets but strongly supported:
1. A record carries **at least**: a map number, a link/object name (matching names in `objects.obr` such as `karnak`, `cloudyVeldt`, `bwOilFire`), a **target/display name** ("Gateway of Dreams", "Goblin Camp", "Inferno Temple"), and a **music override** (`fight2.mid`, `mortal.mid`) that matches the values seen in `music.ini` (`fight = mortal`, `victory = orbwon.mid`) and the map table's mid references.
2. A file's *first* record differs per map but the file size does not, so the file is a **fixed-size array of link slots** with the per-map records at the front and empty slots filling the rest.

**Semantic contract (verified, from `wosDev1.htm`):** *"each link point on a map is numbered"*; *"If the link is to another map, they appear to walk out of a link point on that map. If the link is to a scene, the view changes to the side-view"*; *"Which ever link point is closest to the player (no matter how far away it might be) controls these extra properties"* — i.e. nearest-link governs scene background, music, monster presence and the Quest scene script. Link addresses are the pair `(map, link)` used by `GOTO LINK`, `START_LOCATION`, `TRAVEL` items, and the incarnate default.

**Link icons** come from one world-wide sheet (`maps/readme.txt:48-64`, `wosArt.htm` "Villages" section):
* `OBJECTS.JPG` or preferably `OBJECTS.BMP` (256-colour RLE) — *"If both files are present, only the BMP will be used."* Evergreen ships `objects.bmp` (378.2 KB); an `Image1.bmp` (218.4 KB) is also present, **[UNVERIFIED]** which one is live.
* `OBJECTS.OBR` (12.0 KB) — *"gives a name to selected rectangles of the [OBJECTS] file"*. Its printable projection is a flat list of names, e.g. `house, reservoir, pyramid, hi pyramid, gold bldg, cone, office bldg, castle1, agoda, town1…town6, gazebo, fort, easterHead, village1, Town Well, Portcullis, Art Gallery, Back Alley, Skull House, Deer, Bog Plant, Farm Field, Dog House, Potion Shop, Boot Shop, Horse Statue, Throne Room, Cave Entrance, Dead Tree, Wagon, Bush 1, Swamp, Boulders, Tree 1, Cottage, Saloon, Khut, sand trap, cemetery, palm tree, mesa, mountain, glacier, pine trees, Armory, Jewelry, Weapons, InvisibleBig, chicken coop, sphinx, liberty, shanty, aztec1, aztec2, light house, New York, castle1, japan1, japan2, ruins, boulder, bush 2, tree (autumn), tree (winter), trees (stand), joshWell, joshPotion, joshBoots, joshFountain, joshGate, joshCabin, joshWeapons, joshSkull, joshRock, joshCoral, joshShell, joshPlant, joshGrave, joshMarsh, joshTree, joshAdobe, joshCobble, joshBooth, joshBackside`.
  The `+`/`*`/`.` characters immediately preceding each name in the raw projection are almost certainly the rectangle's x/y/w/h fields — i.e. **the `.OBR` is a flat sequence of `{rect, name}` records**. **[UNVERIFIED]** on field widths.
* "The map is actually 'zoomed in on' so each pixel is 4 or 8 times larger than a village pixel" (`wosArt.htm`).

### 1.5 `.mon` — monster placement

`readme` (`maps/readme.txt:34-36`): *"A file created by the monster placement editor (part of the LINK editor) which defines the placement of monsters on the map."*

**Structural fact (measured): all 14 `.mon` files are the same size — 269.5 KB** (`evergreen.mon`, `stonehenge.mon`, `springwell.mon`, `rustRock.mon`, `roundtree.mon`, `isleLight.mon`, `inferno.mon`, `grotto.mon`, `floodedMaze.mon`, `wormCave.mon`, `animalKingdom.mon`). Fixed size ⇒ fixed-slot array again, not a stream. Exact byte count **[UNVERIFIED]**.

Internals **[UNVERIFIED]**: the raw projection shows no ASCII at all — only binary and long runs of spaces — which is consistent with a table of fixed-width binary records (x, y, monster id, …) rather than a name-bearing format. `wosDev2.htm` adds only *"Place monsters at specific map locations (using the link editor)"*.

**Alternative to `.mon` you can implement instead:** the `GROUPS` table associates a *difficulty level* with a monster-id list, and `wosDev2.htm` lists three ways to populate a fight — `.mon` placement, monster groups tied to a map link, and explicit `FIGHT` calls in a scene. A single-player port can ship with `GROUPS` alone.

---

## 2. `quest.txt` — the QUEST language

### 2.1 File structure

```
; comment lines must BEGIN with a semi-colon
+SECTIONNAME
 <obj#1>
 <obj#2>
-SECTIONNAME
```
(`quest.txt:14-22`; the developer overview calls these "Tables", `wosDev0.htm`.) Section names (`quest.txt:24-41`): `CREDITS`, `STORY`, `TERRAINS`, `MAPS`, `TOKENS`, `ELEMENTS`, `HANDS`, `ITEMS`, `SPELLS`, `MONSTERS`, `GROUPS`, `LEVELS`, `SCENES`, `EQUIP`, `THEMES`. "Sections can appear in any order."

`#include "filename"` splices a file inline (`quest.txt:42-46`). Evergreen's actual includes:
```
quest.txt:300   #include maps.txt
quest.txt:500   #include items.txt
quest.txt:507   #include trophies.txt
quest.txt:514   #include spells.txt
quest.txt:522   #include monsters.txt
quest.txt:530   #include groups.txt
quest.txt:538   #include levels.txt
quest.txt:2266+ #include QuestScenes20.txt … QuestScenes2000.txt   (one per quest chapter)
```
Note the includes sit **inside** the `+SECTION`/`-SECTION` block, so an included file contributes rows to the enclosing table, not sections of its own. Only `quest.txt` is mandatory; every other file name is conventional (`wosDev0.htm`).

There is one file that **must not** be included this way: `missions.ini` — *"All of your world's missions must be declared in this one file (and you cannot use #include here)"* (`missions.ini:18`).

### 2.2 Scene anatomy

```
SCENE <num>, <jpg>, <Style>, <Name>, <Effects>, <Weather>
  …scene commands…
END
```
(`quest.txt:560-575`, `1236-1270`). A scene must begin with `SCENE` and end with `END`; otherwise the next scene's first command executes. Between those two, the player cannot fight, use items, or cast. After `END` the player remains in the scene and may then use items/magic.

`SCENE` args (`quest.txt:1239-1272`):
| arg | meaning |
|---|---|
| 1 | scene number, unique; **0 = Well of Souls**, **1 = Camp**, **2 = Fight**. Treat 0-9 as reserved. |
| 2 | background JPG, **no `.jpg` extension** (A76+ allows `name.WIDTH.HORIZON`, e.g. `cave.300.25` = 3× normal width, horizon at 25% from top, 0 = unrestricted vertical motion) |
| 3 | Style: `WELL` \| `FIGHT` \| `SCENE` (normal) \| `CUT` (heroes invisible) |
| 4 | Name, shown in the scene title bar |
| 5 | Effects: `0` none, `1` underwater ripple, `2` lake ripple, `3` coarse video, `4` jitter, `5` earthquake |
| 6 | Weather: `0` none, `1/2/3` rain light/med/heavy, `7/8/9` snow light/med/heavy |

Live example, `quest.txt:1424`:
```
SCENE 0		temple, WELL, "Well of Souls", 2, 7
```

### 2.3 Command set (A94, `wosquest.htm` — the master list; `quest.txt:560-1410` is the Evergreen-annotated subset)

**Atmosphere / staging**
* `THEME <n>` — `-1` stop, `0` use nearest-link theme, `1` brook, `2` rain, `3` windy aerie, `4` light wind, `5` waterfall, `6` campfire, `7` raging fire, `8` birds (`quest.txt:582-604`). Themes 0-19 are hard-coded; `+THEMES` extends them (`quest.txt:196-216`: `20, cavedrip, campfire1, 10=drip1,10=bird1,10=splash1` — `arg0` id, `arg1` name, `arg2` looped wav, `argN` `"seconds=oneshot"` pairs).
* `MUSIC <file>` — change background MIDI; world `MIDI\` then root `MIDI\`. Naming a nonexistent file turns music off.
* `SOUND "<file.wav>"` — one-shot WAV from world `SFX\`.
* `BKGND <jpeg>` — swap background at runtime. **JPG only.** World `SCENES\` then root `SCENES\`.
* `WEATHER <id>`, `FX <id>`, `FLAGS <mapflagmask>` (override MAP flags for the rest of the scene), `COLOR <n>` (color table).

**Actors and motion**
* `ACTOR <ID[.layer]>, "name", <skin>, <pose>, <x>, <y> {, <colorTable>} {, "<pain.wav>"}` — up to 64 actors, IDs 0-63, but **only 0-9 speak easily** (`wosquest.htm`). `x`,`y` are **0-100 percent of the scene window**, so x=100 is always the right edge regardless of window size; out-of-range values park the actor offscreen for a `MOVE` walk-on. `layer` is `-1000..+1000`; higher is more in front, ties broken by `y`. Player characters are always layer 0. Re-using an actor ID replaces the actor. Skins come from the **MONSTERS** folder, as a horizontal filmstrip; frame 0 is conventionally the credits frame.
* `MOVE <ID>, <x>, <y> {, mode}` — real-time walk. `mode 0` normal, `1` teleport, `2..5` progressively faster (A74+).
* `POSE <p1> {, p2} {, p3}` — swaps the current actor's frame. Three poses auto-cycle p1/p2 with occasional p3 and **keep animating after the script's `END`**.
* `FACE <dir>` — turn an actor.
* `SELECT <ID>` / `SEL <ID>` — set the current speaker.

**Dialog**
* `'` + text — speech bubble from the current actor (the legacy form).
* `N:` + text — shorthand for `SELECT N` + bubble, N = 0-9. **`H:`** (A57+) puts words in the *scene host's* mouth. **`N:`** (A61+) is *narration*: chat window + large scrolling text, no bubble. One bubble at a time; the script auto-pauses until the previous clears.
* Text substitutions (`quest.txt:912-931`): `%0` speaker name, `%1` host player name, `%2` your own name, `%3` random insult, `%4` last chatter's name, `%5` person who typed your name, `%C`/`%Cn` class name, `%En` equipped item in slot n (`0` = right hand), `%In` item name, `%K0`/`%Kn` monsters killed, `%L`/`%Ln`/`%L-1` level name/number, `%Mn` monster name, `%Rn` random 1..n, `%Sn` spell name, `%Tn` token name, `%%` literal `%`.
* `ASK <seconds> [,1]` — pause up to N s (or until the host types chat), set `YES`/`NO` and `Q<word>` flags. The second arg `1` keeps the answer private.

**Flow control**
* `@label` on a line by itself. Labels may be qualified by scene: `47@hiThere`.
* `GOTO @label` · `GOTO EXIT` (boot back to map) · `GOTO SCENE <n>` · `GOTO LINK <map>, <link> [, <dropin>]`.
* `CALL [@]<scene#>@label [, arg0 … arg9]` / `RETURN` — subroutine. Args land in cookies `arg0`..`arg9`, destroyed on use. A function ends with `RETURN`, not `END`.
* `IF <cond-list>, @label` — jump if all terms true; `-` negates a term, `+`/whitespace ANDs.
  Conditions (`quest.txt:749-793`): `#n` on map n · `ALIVE` · `Cn` class n · `DEAD` · `Enn` **equipped** with item nn · `Fnn` map has flag nn · `Gnn` ≥nn gold · `GS` golden soul · `Hnn` character ≥nn hours old · `Inn` has item nn · `Jnn` completed mission nn · `KBn[.x]` killed ≥x times **by** monster n · `KMn[.x]` killed ≥x **of** monster n · `LOSE` · `M0` cheater · `M2` played in a mod · `M4` disabled "avoid modified quest files" · `NO` · `P0` PKer · `Pnn` PKer with ≥nn PKs · `Qword` host's last answer contains word (`^` for spaces) · `Rnn` nn% random (00-99) · `Snn` knows spell nn · `Tnn` has token nn · `Vnn` level ≥nn · `WIN` · `XP` countdown expired · `YES` · `Zn[.c]` ≥c of trophy n.
  "all tokens measured against scene **SERVER** only."
* `COMPARE A,B` then `IF=` `IF>` `IF<` `IF>=` `IF<=` `IF<>`/`IF!=` `IFEVEN` `IFODD` — `COMPARE` stores `A-B` in a slot that survives until the next `COMPARE`. A and B are signed decimal constants or `#<cookie>`.
* `PARTY <cond-list>` — each **client** evaluates and leaves the party if false (then you `GOTO LINK` to move the survivors). `EJECT <cond-list>` does the same and also removes them from the scene.
* `WAIT <seconds>` (decimals allowed) · `TIMER <id> <seconds>` · `COUNTDOWN <seconds>` (test with `IF XP`), `COUNTDOWN 0` cancels.

**Items, economy, quests**
* `GIVE <id>[.<count>]` — e.g. `GIVE G100`, `GIVE I43`, `GIVE T3`, `GIVE xxx.5`. Affects **all** members of the scene. Variants `HOST_GIVE`, `PARTY_GIVE` (and matching `TAKE` forms).
* `TAKE <id>` — same, reversed.
* `OFFER <item>,<item>,…` — SHOP button with an explicit list. `OFFER2 <minLvl>,<maxLvl>,<class>[, …]` — SHOP of a whole class (only the first ~50 qualifying items make it).
* `GAME <n>` — 1 Slobber Slots, 2 Monster Racer, 3 Search for Pi at Home, 4 Blackjack, 5 Pokegatchi Training, 6 Asteroids, 7 Stock Market, 8 Tetris. Cannot be combined with `OFFER` in one scene.
* `MISSION 1,2,3,…` — adds the MISSIONS button; definitions live in `missions.ini`, trophies in `+TROPHIES`.
* `LOCK 1|0` — lock scene to newcomers.
* `TOKEN <n>, "text"` — *defines* a token and its diary sentence; may appear anywhere inside `+SCENES`. Numbering is the author's convention; Evergreen uses tens per quest so diary entries stay grouped (`wosDev1.htm`).
* `HTML <url-or-local-file>` · `MENU` · `LOCK` · `SHUFFLE` · `NTH_TOKEN`.

**Cookies and math**
* `SET <name>, "<string>"` — read back as `#<name>`. Names are letters+digits, no punctuation. Expansion happens **before argument evaluation**, so it works inside quoted strings and inside `GIVE I#<itemID>`. Nesting is not allowed.
* Math, all signed **32-bit integer**: `ADD` `SUB` `MUL` `DIV` (integer; divide by zero = no-op) `MOD`/`MODULUS` (negative follows C), and float variants `F_ADD` `F_SUB` `F_MUL` `F_DIV` `F_MOD`.
* Strings: `SET_LEN`, `SET_SUBSTR`, `STRCMP`, `STRSTR`.
* Stack: `PUSH <cookie>`, `POP <cookie>`.
* Server vars: `GET_SERVER_VAR <cookie>`, `SET_SERVER_VAR`.
* `NTH_TOKEN`.
* `#<g.keyword>` is a special read-only cookie that resolves through `gender.ini`; `#<num.xxx>` / `#<str.xxx>` are stock numeric/string cookies (full list in `wosquest.htm` and `wosCookies.htm`).

**Battles**
* `FIGHT` — no args = standard random fight appropriate to the vicinity. `FIGHT 1,2,2,4` = four monsters, two of type 2. A leading `*` (`FIGHT *,1,2,3`) adds to the random encounter. A **negative** monster id makes that monster fight on your side (`FIGHT 1,2,3,-4`).
* `FIGHT2` — "sticky" variant that does not end the scene.
* The script resumes only once the fight resolves (all heroes dead or all monsters dead), then `WIN`/`LOSE` work (`quest.txt:992-1002`).

### 2.4 A real scene, verbatim

`extracted/worlds/Evergreen/QuestScenes20.txt:8-45`:
```
SCENE 20 giza5, SCENE, "Fort Roundtree"
	THEME	0
	MUSIC	waltz2.mid
	ACTOR	1,	"King Thicke", joshRoyalty, 18, 25, 90
	ACTOR	2,	"Queen Jett", joshRoyalty, 20, 45, 73
	POSE	19,20
	IF	T22, @truth
	1: mmmmppt! [chewing]
	POSE	17
	' Welcome [gulp] to our kingdom, stranger.
	...
	GIVE	T20
	...
	END
@truth
	IF	T23,	@thankyou
	...
	ACTOR	4,	"The Gladstone", spooks, 10, 88, -10
	SELECT	4
	MOVE	4,88,50
	4: Let...truth..be...seen
	SELECT	2
	2: Aaargh!
	SOUND	"thunder1.wav"
```
Note: `GIVE` for a token, `SOUND` for thunder, an actor spawned mid-scene with a *monster* skin (`spooks`) used as a prop, and `MOVE` to x=88 to bring it on-screen.

### 2.5 Supporting tables

**`+TOKENS`** (`quest.txt:361-393`) — `arg0` id, `arg1` diary sentence. Evergreen: 1-3, then tens per quest (10-15 Fire Boots, 20-23 Lost Gem, 30-34 Scroll of Fidelity, 40-44 Troubled Twins, 50-55 Goblin Army) plus 5-7 for the "Quest 8 of the Book".

**`+ELEMENTS`** (`quest.txt:396-449`) — exactly 8, ids 0-7, forming a wheel:
```
         0
      7  |  1
       \ | /
    6 ---+--- 2
       / | \
      5  |  3
         4
```
`0` is always the healing element and heals everything regardless of affinity; the element opposite any target deals the most damage, adjacent less, and near-neighbours a little. Evergreen: `0 Life, 1 Water, 2 Nature, 3 Earth, 4 Death, 5 Fire, 6 Spirit, 7 Air`. Ids 8-255 are **chaos** elements, named "chaos" (nameable since A78) and behaving like Death (`quest.txt:432-441`, `wosFAQ.htm`). Art is positional: `elements.bmp` must be reordered if you renumber.

**`+HANDS`** (`quest.txt:451-489`) — the 8 right-hand weapon classes, **numbered 0-7 here but 1-8 everywhere else**:
```
;  ID	Name		Range		SoundFX
	0,	"Sword",	30,		"sword1.wav"
	1,	"Staff",	20,		"staff.wav"
	2,	"Bow",	100,		"sword8.wav"
	3,	"Music",	50,		"chirp4.wav"
	4,	"Fist",	20,		"fist.wav"
	5,	"Dart",	80,		"dart.wav"
	6,	"Book",	70,		"chant6.wav"
	7,	"Spirit",	90,		"crow1.wav"
```
Range 0-100 is the radius in which an EQUIP attack **cannot miss** — the "no-miss circle" drawn at your feet (`wosFAQ.htm`).

**`+EQUIP`** (`quest.txt:150-182`) — optional rename of slots `0` Helmet, `1` Armor, `10` Boots, `11` Shield, `12` Ring, `13` Amulet.

**`+GROUPS`** (`groups.txt`) — `arg0` = *monster difficulty level* 1-4095, `arg1..arg9` = monster ids (duplicates allowed, drawn randomly with likelihood rising with distance from the link). Group `0` is a flag row: `0, 1` = "monster groups are NOT RANDOM and all members will appear". Level `0` conventionally means "no monsters near this link". Bands used by Evergreen: 1-99 land, 100-149 water pools, 150-199 fully underwater, 200-249 bosses/special, then 10xx/11xx/12xx/13xx/14xx/15xx per region.

---

## 3. `monsters.txt` and monster art

### 3.1 Columns

One line per monster, comma-separated; `;` comments; quoted names recommended. The file carries its own column ruler twice (`monsters.txt:328-330` and `:340`):
```
;					skin		scale		el	hp		mp	def	off	exp	gld	lev	str	sta	agi	dex	wis		growl.wav	pain.wav		attack path
;	0	1................	2			3		4	5		6	7	8	9	10	11	12	13	14	15	16		17	18	19
```
| arg | name | notes |
|---|---|---|
| 0 | ID | 1-4095, unique |
| 1 | name | 16 chars, shown over the monster |
| 2 | skinName | BMP in the world `MONSTERS\` folder, **no extension**; `"mirror"` renders the player's own skin |
| 3 | `scaleFactor.monsterFlags.colorTable.xparent` | scale −8..+8 (0 = 100%, 1 = 125% … 8 = 300%; −1 = 90% … −8 = 20%). Flags: `1` untameable, `2` own-element spells only, `4` can summon, `8` magic only, `16` only Arg20-AI spells, `32` never flees. `colorTable` 0-32 (0 none; 1-3 channel swaps; 4-9 single-channel overwrites; 10-12 intensity cuts; 13-20 dim→brighten; 21-25 grayscale; 26-32 per-channel invert). `xparent` = 1 for ghost transparency. |
| 4 | `element.alignment` | element 0-7 or 8-255 chaos; optional ≤15-char punctuation-free alignment word |
| 5 | hp | 1-N, or 0 = derive from level |
| 6 | mp | 1-N, or 0 = derive |
| 7 | defense | 1-N, or 0 = derive (the monster's "armour") |
| 8 | offense | 1-N, or 0 = derive (its "weapon") |
| 9 | expPts | 1-N, 0 = derive, `-2` = none. Shared among survivors by participation. |
| 10 | gold | 1-N, 0 = derive, `-2` = none |
| 11 | level | the core value; everything else should be 0 so it derives |
| 12 | strength | 1-255, 0 = derive — physical damage |
| 13 | stamina | 1-255, 0 = derive — damage taken |
| 14 | agility | 1-255, 0 = derive — makes **it** miss more |
| 15 | dexterity | 1-255, 0 = derive — makes **it** miss less |
| 16 | wisdom | 1-255; `0` = casts nothing ever; `-1` = derive. Gates which element-wheel neighbours it can reach (`wosFAQ.htm`) |
| 17 | growl wav | `"snarl.wav"` |
| 18 | pain wav | `"ouch.wav"` |
| 19 | attack path | `-1` pick a random one at world load, `0` none, `N.x.x…` an animation path (same grammar as ITEMS arg 16) |
| 20 | ai command | *"I should document that here, but since I haven't written it yet, that would be pointless"* — i.e. **undocumented even by the author** |

**Monster 0 is special**: a row of percentages (1-100) that de-rate the level-based algorithms globally. Affected args: `hp, mp, offense, defense, strength, stamina, wisdom, dexterity, agility, XP, GP`. Missing or 0 = 100%. The actual row (`monsters.txt:330`):
```
	0,	"Algorithm Scale %",0,	0,	0,	100,	100,100,100,100,100, 0,	100,100,100,100,100
```

Real rows (`monsters.txt:344-353`):
```
	1,	"Green Jelly",	josh2,	0,	1,0,	0,0,	0,	0,	1,	0,	0,	0,	0,	0,	"growl5.wav",	"pain4.wav",	-1
	5,	"Slobber",	josh20,	0,	3,0,	0,0,	0,	0,	4,	0,	0,	0,	0,	-1,	"growl3.wav",	"pain10.wav",	-1
	8,	"Carnivorous Hat",	ben2,	0,	6,0,	0,0,	0,	0,	7,	0,	0,	0,	0,	-1,	"growl8.wav",	"pain8.wav",	-1
```
(Wisdom `0` = "casts no spells, ever"; `-1` = derive. This is the Green-Jelly-won't-cast question in `wosFAQ.htm`.)

### 3.2 Monster BMP layout

From `wosArt.htm` ("Monsters"): RLE BMP on the 256-colour `souls.pal`. Six square frames, laid out left to right:
```
[UNUSED/CREDITS] [READY]
[ATTACKING]      [PAIN/WEAK]
[GROWLING]       {[BLANK] [MAGIC ATTACK]}
```
* Monsters have **no 3×3 map block** — the 3×3 is reserved (recommended for artist credits) and is why monster sheets can be smaller.
* The far-right square is the **growl** frame (hero CHAT's counterpart). While waiting to bite, the engine flips READY ↔ GROWL.
* Optional frames 5 and 6 to the right: frame 5 is wasted space, frame 6 is the **magic-attack** frame; without it ATTACK is reused for both physical and magical attacks.
* Monsters are drawn looking **down and to the right** so they face the hero, who looks up and to the left.
* Same guide/control-pixel rules as hero skins (below).

Villager files are just long filmstrips of arbitrary length (suggest ≤32), frame 0 = credits, drawn looking right or straight ahead; **all villagers in one file must be floating or all must be standing** (`wosArt.htm` "Villagers"). `.MONSTERS\readme.txt` in the world confirms per-world monsters with fallback to the root `MONSTERS\` folder. Evergreen's own `Monsters/` folder is empty apart from its readme, so **every Evergreen monster skin resolves from the root** `extracted/monsters/` (125 files, e.g. `spooks.bmp`, `villagers.bmp`, `smiley.bmp`, `petKDH.bmp`, `ben2.bmp`).

---

## 4. Hero / character art

### 4.1 Frame layout

`extracted/skins/readme.txt` (authoritative, in-tree):
> The internal layout of a skin file is a horizontal strip of square images. However many pixels tall the BMP file is determines the size of the square. […] five basic squares […] `[MAP] [READY] [ATTACK] [WEAK] [CHAT]`. The MAP square is divided into nine sub-squares and is used to supply the artwork of your hero while walking on the map in the **8 possible directions**. The center image is used when the character is 'camping' (or fighting). Do not include a shadow in these map images as a simple oval shadow will be provided automatically.

`wosArt.htm` shows the same as six frames — the sixth being CREDITS:
```
[MAP(3x3)] [READY] [ATTACKING] [PAIN/WEAK] [CHATTING] [CREDITS]
```
with: READY = facing away from camera toward the upper left; ATTACK = the blow (the engine mirrors the image when the enemy is on the right, so **always draw the hero looking left**); WEAK = doubled over, used **red while being hit** and **black when dead** (and as a silhouette in other solid colours for effect flashes); CHAT = turned to the camera, with a speech bubble; CREDITS = your own logo. "Note that the credits frame is optional, but highly recommended" for the *original* format — but hero files "must add a 6th frame to the far right for credits", whereas monsters and villagers may put credits in frame 0 instead.

Consequences for a renderer: if BMP height is `H`, every frame is `H × H`; frame 0 is `3H × 3H`; total width is `6H` (or `5H` for monsters). Evergreen's 96-px hero form (`hero1.bmp`…`hero7.bmp` ≈ 19 KB) ⇒ `H = 96`, `576 × 96` px. The 3×3 map grid is the 8 compass directions with the centre used for camp/fight — *the engine's exact clockwise ordering of the 8 directions is not documented* ([UNVERIFIED]; a sensible assumption is N, NE, E, SE, S, SW, W, NW in reading order, centre = facing viewer).

### 4.2 Guides, control pixels, sizing

`wosArt.htm`:
* *"all skin files must be a **multiple of 6 pixels high**, and then each 'square' is that many pixels wide, with the **guideline at the top and left edges** of the square."* The black 1-px guidelines are not drawn.
* *"The pixels along the top-left edge of a skin file are the **CONTROL PIXELS** […] the most important one is the **extreme upper left pixel** which sets the 'transparency' color for the entire skin file. The aqua color **RGB(0,128,128)** is recommended."* Keep the whole top row at the guideline colour, then clear only the control pixels you want.
* **Non-floating skins**: also clear the pixel immediately right of the transparency pixel — "offset (0,1) … Skins with this bit cleared (set to the transparency color) will not float." Standard skins float with a drop shadow; the lower 1/6 of each scene square is the shadow area.
* Files must be **RLE-encoded BMP coerced to the 256-colour `souls.pal`** (`extracted/art/souls.pal`, 1.0 KB). 24-bit art will not match.
* Anti-cheat minimum: "any 'reasonable' skin will be displayed"; oversized skins (>~20 KB) will not transfer over the network — irrelevant offline, but the size target is a good sanity check.
* `SKINS\` is a **root** folder shared by all worlds, deliberately, so skins are portable between characters/worlds (`skins/readme.txt`).

### 4.3 There are no "bodies/heads" assets

`extracted/art/` contains **item filmstrips and UI chrome only** — no body/head compositing. Confirmed contents relevant to a port: `souls.pal`, `slots.bmp`, `spells.bmp`, `items.bmp`, `trophy[2-4].bmp`, `music.bmp`, `swords/staffs/bows/right5..right8.bmp`, `helmets/armor/boots/shields/rings/amulets/darts.bmp`, `disease.bmp`, `effects00..09.bmp`, `attack00..09.bmp`, `button*.bmp`, `buttonBar*.bmp`, `*Border256.bmp`, and UI art (`title.jpg`, `earth.jpg`, `chapter.jpg`, `createsoul.jpg`, `picksoullabel.jpg`, `quadris.jpg`, `petPen.jpg`, `where.jpg`, `bk*.jpg`, `questdiary.*`). **Hero appearance is a single composed skin BMP — no layer system.** [Confirmed by directory listing.]

`ART\readme.txt` (and the identical world copy) give the item filmstrip geometry:
```
AMULETS.BMP 48x48   ARMOR.BMP 48x64   BOOTS.BMP 48x48   DARTS.BMP 48x48
HELMETS.BMP 48x48   ITEMS.BMP 48x48   RINGS.BMP 48x48   SHIELDS.BMP 48x64
STAFFS.BMP 48x64    SWORDS.BMP 48x64
```
plus `slots.bmp` = **48 × 384**, eight 48×48 symbols (`slots.ini:16-17`).

### 4.4 Skin selection ↔ class

`levels.txt` binds a class to four per-gender default skins via `DEFAULT_SKIN`:
```
	100,	0,	20,	0,	"Sword-User",   10,				1
	DESCRIPTION	"Warriors are often the first to battle, and last to leave. …"
	AUTO_MAX	20, 3759,  10, 1186,  20		
	START_ABILITY   0,  0,   10,   0,   0
	DEFAULT_SKIN	adventurer, adventurer, Tracker2, Tracker2
...
	200,	0,	15,	5,	"Magic-User",		90,				2
	DEFAULT_SKIN	Warlock2, Warlock2, Enchantress, Enchantress
```
(`levels.txt:290-311`.) The four values are genders 0-3. A world's `gender.ini` supplies per-gender defaults too (`adventurer=`, `menu=`) and a per-gender pain WAV (`pain1.wav=pain10.wav` for gender 2). The player overrides their own skin via the Book of Skins (solo mode only), and the class does not constrain it — "people can choose any skin for their character, no matter what class".

---

## 5. Save file, solo flow, and how a battle works

### 5.1 `.her`

* Location: **`C:\WoS\Save\<WorldName>\savedHeroes\*.her`**, confirmed verbatim by `wosSuperFAQ.htm` ("you should have files like this: `<c:\WoS\Save\Evergreen\SavedHeroes\*.her>`") and by `extracted/Save/readme.txt`. `extracted/worlds/Evergreen/readme.txt:20-22` describes a per-world `savedheroes` folder of `.her` files.
* **They are encrypted.** The in-game help states: *"NOTE: do [not] attempt to rename your .her file or change the world. You may think it succeeded, but in fact the file will be corrupted. **These are encrypted.**"* (`WELLOFSOULS.HLP`, "Saving in Well of Souls" topic, recovered from the `:raw` projection; the surrounding text is LZ-compressed so this quote is partially reconstructed — **[UNVERIFIED]** on exact wording, reliable on substance).
* Each PC is assigned a **soul ID** on first run; a `.her` created under a different soul ID makes the character a permanent "modder". A Golden Soul can re-import. Consequently a port should simply define its own save format and not attempt to read A96 `.her` files.
* **Byte layout: entirely [UNVERIFIED].** The `Save/` folder in the extracted tree contains only its `readme.txt` — there is no sample `.her` to examine, and PE disassembly was unavailable.
* Related: each world also has a **world diary** (`blank_diary.doc` appears in both the root and `worlds/Evergreen/`), and `worlds/Evergreen/world.ver` is a 1.2 KB binary whose printable projection is `i9`…`Dan Samuel\0Dan Samuel L\0` repeated — almost certainly the author-identity/signature blob used by the `/version` anti-mod system described in `wosDev7.htm` ("How to version your world, so that others can't hack it"). **[UNVERIFIED]**.

### 5.2 Offline / solo flow

1. `wosHome.htm` "How to Play": pick **SOLO** or a network; pick a world (Evergreen is the only bundled one).
2. `+STORY` (`quest.txt:87-114`) scrolls the back story over the world map, preceded by `ART\splash.jpg` if present (Evergreen has `art/splash.jpg`) — *"You can't control how long the splash will be seen… it will be seen longer on slower computers."*
3. **Scene 0 — the Well** (`quest.txt:1424+`): `SCENE 0 temple, WELL, "Well of Souls", 2, 7`, a looping ambient theme, and the Blind Sage actor who explains the NEW / RESTORE / INCARNATE / MAP / HAUNT buttons. Per `wosDev1.htm`: *"Once this introduction has finished (or the player dismisses it prematurely), the player is taken scene 0 (zero…) This is where they pick their starting character, or an old character to resume."* The **new-soul dialog** asks for a name, a class (from `LEVELS`), PK opt-in, ability-point assignment, and a skin (`wosHome.htm`, `WELLOFSOULS.HLP` "The New Soul" / "Soul Attributes" topics).
4. **INCARNATE** — "For an old character, this will take them to the last link point visited by that character. For a new character, they will be taken to **just outside of link-point 0 of map 0**" (`wosDev1.htm`). Override per class with `START_LOCATION <map>, <link>, <dropIn>` (`levels.txt:139-146`; `dropIn=1` incarnates *into* the link, so the link must point at a scene; A76+). *"Otherwise, they will appear above link 0 on map 0."*
5. **First map / first link.** Evergreen's link 0 on map 0 is the **Gateway of Dreams** — the resurrection/recharge point, and the very first link the `IF`-token script pattern in `quest.txt:332-347` and `wosHome.htm` ("Resurrection") both reference. New characters are also intercepted by **SCENE 3** (`quest.txt:1583+`): *"this is the evergreen starting scene on the main map, where you can return to be healed"* — the Soul Brother asks you to agree to the golden rule, and `GIVE T1` on agreement.
6. Map walking: click a point, the hero walks there; walking **onto** a link drops you in. `CAMP` starts a camp scene (scene 1) for talking/self-buffing; you may be **pulled into a fight scene** (scene 2) whether you expect it or not. `RETURN TO WELL` saves position and returns to scene 0.
7. Two display modes throughout: **MAP** (aerial walking) and **SCENE** (side view). "Fighting always takes place in a scene."

The reserved scenes, exactly as implemented in Evergreen (`quest.txt:1493`, `1572`, `1583`):
```
SCENE 1          ; CAMP — THEME (no args), COMPARE/IF on #<num.isPKAttack> and #<num.mapNum>
                 ;   → FLAGS 45056 + LOCK 1 + N: announcements for a PK camp;
                 ;   → map 2 (Macgyver Castle) gets a clickable LOCK actor
SCENE 2
	FIGHT         ; FIGHT — the standard auto-fight script is literally this one line
	END
SCENE 3  aztec   ; Soul Brother, healing/re-charge, the golden-rule gate for token T1
```

### 5.3 How a battle works — **real-time with charge meters, not a turn queue**

`WELLOFSOULS.HLP`, combat topic (recovered from the `:raw` projection, partially compressed — substance reliable):
> "Since this is a multiplayer game, **it's not always your turn to attack.** You can attack when the cursor is 'smoking' (with magic sparks) […] In the meantime, **people will be moving around, chatting, and so on until it's time to make an attack. All this can go on indefinitely — a few seconds to maybe a minute — but eventually, someone (it might not be you) will start an attack, and the fight will continue.**"

Mechanics that follow, all from `wosFAQ.htm` and `quest.txt`:
* **No fixed turn order.** Any participant may act whenever their own attack becomes available. The visible affordance is the "smoking" attack cursor.
* **Charge meters**: *"Careful examination of the HP and MP meters in the lower left will show a simple white bar beneath each. This is the charge bar for **physical attacks (HP)** and **magical attacks (MP)**. It discharges whenever you attack, and then slowly recharges back to maximum. In general, **if you wait for it to recharge before attacking again, your attack will be stronger**."* The effect is much larger at low training and nearly vanishes by training level 9. Exact recharge rate and the damage curve are **[UNVERIFIED]** — not documented in any text asset.
* **Miss**: attacker's **dexterity** vs target's **agility**, for both physical and magical attacks. A miss shows `MISS` above the target.
* **Fizzle**: independent of dexterity — controlled by the class **magic-ratio**. 100% → never fizzle; 0% → fizzle about half the time; proportional in between. Non-elemental (book/chaos) weapon spells cannot fizzle as of A51; Life spells never fizzle.
* **Damage math inputs**: STR (damage dealt) · STA (damage taken) · AGI (avoid being hit) · DEX (accuracy) · WIS (spell power and PP earned per attack) — `wosFAQ.htm`. Monster side: `arg7 defense` (armour) + elemental protection, `arg8 offense` (weapon), `arg12 strength`, `arg13 stamina`, `arg14 agility`, `arg15 dexterity`. Damage from element `e` against element `t` scales with wheel distance (§2.5). Healing spells do not use defense.
* **Training (PP)**: 16 training categories = 8 elements + 8 hands. Every attack earns PP, or you spend wallet PP on the training screens. Training is non-linear and **level 10 is unreachable** — you asymptote at ~9.99. Excess training over a spell/weapon's minimum multiplies power by ~**1.19^excess** (A43). PP thresholds: `L0 0-554, L1 555-1249, L2 1250-2141, L3 2142-3332, L4 3333-4999, L5 5000-7499, L6 7500-11665, L7 11666-19999, L8 20000-44999, L9 45000-1000000` (`levels.txt:214-226`; the file itself recommends a ~20-unit buffer on each bound for cross-platform float safety).
* **Class effectiveness**: magic ratio also scales spell power — ~90% for a Magic-User, ~80% Spirit-User, ~70% others (`wosFAQ.htm`).
* **Magnificent attack**: purely random, natural frequency ~1 in 50, nudged up by ghost players blessing you and down by cursing you (`wosFAQ.htm`).
* **Rewards**: XP and GP from `monsters.txt` args 9/10 (0 = derive from level, scaled by **monster 0**'s percentages), shared among survivors **by participation**; suppressed entirely on maps with flag 2048 (`NO_REWARD`). Map flag 4096 makes heals give 1 HP.
* **Balance guidance** (`wosDev2.htm`): "a single monster of the same level as yourself would be difficult to win against. You would get killed roughly half the time"; 2-3 same-level monsters is winnable with healing. Use `/battle`, `/battle2`, `/battle3` to simulate class matchups.
* **Scene-level control**: `FIGHT` blocks the script until the fight resolves; `WIN`/`LOSE` then drive the branches. `FIGHT 1,2,3,-4` puts monster 4 on your side. `LOCK 1` seals the scene. `FLAGS` can neutralise spells/tickets/heals for the rest of a camp.

---

## 6. INI files and the remaining tables

### 6.1 `world.ini` — 1.2 KB? no: 265 B, hand-maintained by `/version`
```ini
[world]
name=Evergreen
version=1.0137
home_url=http://www.synthetic-reality.com/wosHome.htm
zip_url=http://www.synthetic-reality.com/Evergreen.zip
ini_url=http://www.synthetic-reality.com/Evergreen.ini
author=Dan Samuel
author_email=dan@synthetic-reality.com
author_url=http://www.synthetic-reality.com
wos=A96
```
`wosDev0.htm`: *"World.ini (special file maintained by the /version command. do not edit)"*; `readme.txt:9-12` says to keep `version` equal to the `info` file on the publishing web site. **Purely a multiplayer-distribution artifact — ignore for an offline port.** The companion `world.ver` binary is the signature/mod-detection half of that scheme.

### 6.2 `config.ini` — `[General]`, world tuning
All keys are overrides; most ship commented out. The ones that matter for single-player simulation (`worlds/Evergreen/config.ini`):
| key | value | effect |
|---|---|---|
| `goldName` | `GP` | currency display |
| `pkHandPercent` / `pkMagicPercent` | 100 | scales PK damage only, *not* healing |
| `spellSuccessPercent` | 100 | multiplies spell-cast success |
| `cookieProtection` | 1 | 0 none / 1 only `secure*` / 2 all (irreversible) |
| `startingGP` | **500** | starting gold regardless of class |
| `maxUnspentPP` | 100000 | cap on unspent training PP |
| `maxPKAttackAdvantage` | 10 | levels |
| `noGivingGP`, `pkTrophy`, `petsCanBitePeople` | | multiplayer/social |
| `karmaPointsAreAlsoWarPoints`, `monsterXPAreAlsoWarPoints`, `tacticsWinGivesWarPoints`, `tacticsSourceUrl`, `worldHomeUrl` | | ladders/web (ignore offline) |

### 6.3 `music.ini` — one INI section per map root name
Rules (`music.ini:5-30`): sections are named after the MAPS table's **root name**; a **`[common]`** section supplies defaults; lookup order is the map's own section → `[common]` → the MAPS table's own `fight` column. Fields: `fight`, `victory`, `lost`, `levelup`, `numMidi`, and `midi1`…`midiN` (**1-based**, not 0). Blank `fight` = a random stock fight song; blank `midiN` = fall back to `<rootname>.mid`. Playlists are supported since **A70**; a MIDI flagged `loop` never ends and stalls the rest of the list. A blank/invalid name turns music off. Files are looked up in the world `MIDI\` then root `MIDI\`.
```ini
[common]
fight = mortal
victory = orbwon.mid
lost = lost.mid
numMidi = 1
midi1=scrn_overworld

[inferno]
fight = mortal
numMidi = 3
midi1=ahad_attack
midi2=jeremy_march
midi3=desert sin
```
There is a dead `[pkArena]` section — the actual arena roots are `pkarena`/`petarena` (maps 16/17), so those arenas fall through to `[common]`. **[Verified inconsistency in the shipped data; harmless, but a port should decide deliberately.**

### 6.4 `slots.ini` — the Slobber Slots mini-game (GAME 1)
`[0]`..`[7]`, each with `percent` (instances of that symbol per 100-slot wheel; the eight values must sum to 100), `triple` (GP for 3-of-a-kind), `double` (GP for a match between the **first** wheel and the second or third — `0XX` does **not** count). Art is `art/slots.bmp`, **48 × 384, eight 48×48 symbols, index 0 = top**. The wheel is built at load to match the percentages, positions randomised. Total combinations 100³ = 1,000,000. Evergreen: symbol 0 is rarest (`percent=2, triple=1000000`), symbol 7 common (`percent=43, triple=100, double=5`); the eight percents sum to 100. The file's own long comment walks through the expected-value math and settles on a payout slightly under the cost per pull.

### 6.5 `gender.ini` — optional, 4 genders (0-3)
Sections `[0]`..`[3]`; keys are *keywords*, values are what scripts see via `#<g.keyword>`. Evergreen gender 0 is a duplicate of male for upward compatibility; 2 is female (`he=she, his=her, him=her, male=female, adventurer=sorceress, pain1.wav=pain10.wav`); 3 is defined but not on the menu. Non-standard keys: `adventurer=` (fallback skin), `menu=` (show on the gender selection menu), `pain1.wav=` (hurt sound). Keywords are **case-insensitive**, and the same keyword set must appear in every section. `/gender N` sets your own.

### 6.6 `levels.txt` — classes, levels, and the class macro
Key = `classId * 100 + level`; class ids 1-88 (`wosDev2.htm`). `arg0` is that 4-digit key, `arg1` is **no longer used**, `arg2` max-HP gained at that level, `arg3` max-MP gained, `arg4` level name, and **only at level 0** `arg5` magic-ratio and `arg6` right-hand weapon (1-8, or 0 for none). Levels run 1-99; "a bonus level 100 is supplied by the game, but it uses the same table entry as level 99". Missing lines inherit from the previous one; an empty name (except level 1) reuses the last. Gendered names go in one quoted string with `|` separators: `"Emperor|Emperor|Empress"`. The **level 0 row is the class itself** — its name is the class name shown in the new-soul dialog, and its HP/MP are the level-1 starting values.

Class commands, which must appear **between the `nn00` and `nn01` lines** or they have no effect:
```
DESCRIPTION "…"                    MAGIC_RATIO n        HAND_RATIO n
START_ABILITY str,wis,sta,agi,dex  MAX_ABILITY  str,wis,sta,agi,dex
START_ELEMENT_PP 0,1,2,3,4,5,6,7    MAX_ELEMENT_PP 0,1,2,3,4,5,6,7
START_HAND_PP 0,1,2,3,4,5,6,7       MAX_HAND_PP  0,1,2,3,4,5,6,7
AUTO_MAX minHP,maxHP,minMP,maxMP,startMPLevel
START_LOCATION <map>,<link>,<dropIn>   MAX_WALLET n      NO_GIFTS
START_ITEMS 1,2,3,…   START_SPELLS 1,…   START_TOKENS 1,…
HIDDEN_CLASS [<startLevel>]           DEFAULT_SKIN s0,s1,s2,s3
```
`AUTO_MAX` computes the whole HP/MP curve semi-logarithmically between level 1 and level 100 and **overrides** the table's HP/MP columns. Right-hand semantics: magic-ratio **> 50** permits a staff, **< 50** a sword, **= 50** both. `HIDDEN_CLASS` classes are reachable only via `SET num.hostClass, <n>`. A class's "preferred right hand" is **always pre-trained to level 5** regardless of `START_HAND_PP`, for backwards compatibility.

### 6.7 `items.txt`, `spells.txt`, `trophies.txt`, and the other optional files
* **ITEMS** — arg0 id 1-5119 (1-1023 = stackable up to 100, 1024-5119 = single); arg1 name; arg2 class; arg3 `image[.extension]` (index into the class filmstrip, extension `1`-`9` selects `xxx_1.bmp`…`xxx_9.bmp` in the world `ART\`; keep bitmaps ≤4096 px wide ≈ 64 cells); arg4 GP (0 = unsellable/untradeable); arg5 `Level[.EquipToken].Flags…`; plus ability deltas, sound effects, and the arg-16 attack-path grammar that `monsters.txt` arg 19 refers to. Classes: `0` POTION `items.bmp` · `1` ANTIDOTE · `4` TRAVEL (the `4.mode.num.link.dropin[.sceneNum]` ticket form) · `5` THROWABLE `darts.bmp` · `10`-`23` the equipment slots (`helmets/armor/swords/staffs/bows/music/right5/right6/right7/right8/boots/shields/rings/amulets.bmp`) · `100`-`104` attribute bonuses · `105` ATTR_ALL (opens the ability dialog) · `200` ITEM_CLASS_PET (a Pokegatchi; arg3 holds a **monster id**, sellable only via `OFFER`, not `OFFER2`) · `201` ITEM_CLASS_HTML (arg15 holds a URL; non-`http://` = a local file in the world `HTML\` folder). Classes 2 and 3 are marked `<<DO NOT USE YET>>`.
* **SPELLS** — arg0 id 1-767 (0 = algorithm tweaks); arg1 name; arg2 PP cost (`-1` unlearnable, `0` auto); arg3 element; arg4 `damage[.monsterID]`. The negative-damage code table (`spells.txt:56-88`) is the whole special-effect vocabulary: `-1` RESURRECTION, `-2..-11` diseases (POISON, SAP, STUN, GAG, NUMB, SLEEP, CONFUSE, CHARM, TAME, FREEZE), `-20..-24` ability debuffs, `-200..-203` summons, `-300` SUMMON_ITEM. **A cure is `diseaseDamage - 100`** (`-102` cures poison). Regular (non-special, non-Life) damage is computed by the engine from spell level; PP cost, MP cost and power are all hardwired algorithms. Life heals and Disease effects are the only designer-settable damage values.
* **TROPHIES** (`trophies.txt`) — trophy definitions for the trophy-bag system (`wosDev8.htm`); a zone in Evergreen is referenced by cookie `#<num.Trophy1>` (`quest.txt:1705`).
* **`missions.ini`** — declared separately, cannot be `#include`d, and defining a mission there does not expose it; a scene must run `MISSION n,…` to offer it (`missions.ini:18-21`).
* **`racers.txt`**, **`stocks.ini`** (root, 6.6 KB — the multiplayer stock-trading company list, overridden per world), **`MIX/admins.dat`**, **`srnet`**, **`wosViewer`** — all multiplayer or authoring tools. Ignore for a single-player port.
* **`+SCENES` split across 20 files** — `QuestScenes20.txt` … `QuestScenes2000.txt`, all pulled in by `#include` at the end of `quest.txt:2266-2340`, plus `questscenes170.txt` which is the only lowercase filename in the world (a Windows case-insensitivity trap worth noting).

---

## 7. What is still unverified, and the cheapest way to close it

| Item | What is known | How to confirm |
|---|---|---|
| `.ter` cell size, bpp, palette, dimensions | Is a Windows BMP (`BM` magic verified); holds terrain ids 0-9; per-map size varies 3.7-37.1 KB | Parse the BITMAPINFOHEADER directly; assert `biWidth/biHeight` equal the matching JPG's SOFn dimensions; sanity-check nibble values ∈ 0-9 |
| `.obl` record layout | Fixed ~200 KB; contains map #, object name, target name, music override per link | Treat as a 200,800-byte array and search for known 16-byte-padded strings; the printable projection already reveals 4-5 distinct string fields per record. Link index ⇒ the record slot index |
| `.mon` record layout | Fixed ~269.5 KB; purely binary; no strings | Brute-force: place a known monster in the link editor, diff two files. Or synthesize your own `.mon` and observe — a solo port can legitimately ship without one and use `GROUPS` |
| `.her` internals | Encrypted; lives at `Save\<World>\savedHeroes\` | Not worth it — design a fresh save format |
| `world.ver` | 1.2 KB binary containing repeated `Dan Samuel\0` | Ignorable offline |
| 8-direction ordering inside the skin 3×3 MAP block | 3×3, centre = camp/fight | Render all Evergreen skins, pick the one with an obvious N-up/S-down asymmetry; or supply your own skins and mirror the engine's choice |
| Charge-meter recharge rate and damage-vs-charge curve | Charge exists, recharges slowly, stronger at full; ~1.19^excess from training | Tune by feel; no documented constants exist in any text asset |
| Exact argument grammar of `monsters.txt` arg 19 / `items.txt` arg 16 (attack paths) | Cross-referenced but not written down in either file | Only reachable by experiment or by the engine's own docs for a newer version |
| JPEG pixel dimensions of each map/scene image | Scenes are **360×256** (verified in-tree); maps recommended 512×512, "768 or so" for the world map | Read SOFn markers |
| `souls.pal` palette contents | 256 entries, must be used exactly or colors break | Load it; index 0 is the guideline/transparent control pixel in skins |


## Sources

- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/maps/readme.txt`: Authoritative description of the MAPS folder: mapName.JPG (mandatory), mapName.OBL (links), mapName.TER (terrain/boots), mapName.MON (monster placement), mapNameX4.JPG (optional 4x), OBJECTS.JPG/BMP, OBJECTS.OBR. 512x512 recommended max for the main world map.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/quest.txt`: Master world script (2395 lines). Sections +CREDITS/+STORY/+TERRAINS/+EQUIP/+THEMES/+MAPS/+TOKENS/+ELEMENTS/+HANDS/+ITEMS/+TROPHIES/+SPELLS/+MONSTERS/+GROUPS/+LEVELS/+SCENES, the MAPS flag table, the full QUEST command reference, and SCENEs 0/1/2/3.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/maps.txt`: Concrete MAPS table rows: id, image file, OBL/TER root name, display name, map flags. Maps 0-17.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/monsters.txt`: MONSTERS table: 21-column doc, monster-0 algorithm-scale row, and real monster rows (Green Jelly, Archer Fish, Slobber...).
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/levels.txt`: LEVELS table: class/level ID scheme (n100 = class n level 0), per-class commands (DESCRIPTION, AUTO_MAX, START_ABILITY, DEFAULT_SKIN...), PP training table.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/items.txt`: ITEMS table: class codes 0/1/4/5/10-23/100-105/200/201, image+extension filmstrip indices, GP, level/equip-token/flags.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/spells.txt`: SPELLS table: element ids, the negative-damage "disease"/summon code table, cure = damage-100 convention.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/groups.txt`: GROUPS table: difficulty-level -> monster-id list, group 0 flag row, distance-based randomisation.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/music.ini`: [common] plus one INI section per map root name: fight, victory, lost, levelup, numMidi, midi1..N.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/slots.ini`: Slobber Slots config; documents art/slots.bmp as 48x384 with 8 48x48 symbols.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/config.ini`: [General] world tuning: startingGP, spellSuccessPercent, pk percent, cookieProtection, maxUnspentPP, maxPKAttackAdvantage, tactics URLs.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/world.ini`: [world] name/version/home_url/zip_url/ini_url/author/author_email/author_url/wos=A96.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/gender.ini`: Gender 0-3 keyword tables plus adventurer=/menu=/pain1.wav= overrides; documents the #<g.keyword> cookie.
- `/home/jshield/src/games/WoS/extracted/skins/readme.txt`: Hero skin layout: [MAP][READY][ATTACK][WEAK][CHAT], MAP split 3x3, square size = image height, RLE BMP on souls.pal.
- `/home/jshield/src/games/WoS/extracted/art/readme.txt`: Item filmstrip BMPs and their per-class cell sizes (48x48 / 48x64).
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/scenes/readme.txt`: Scene background JPEGs are 360x256; links bind to scene numbers, quest.txt binds scene numbers to scripts.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/Monsters/readme.txt`: Monster/villager skins live per-world, with fallback to the root MONSTERS folder.
- `/home/jshield/src/games/WoS/extracted/Save/readme.txt`: Save folder holds per-world subfolders of hero files; heroes are world-bound.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/maps/evergreen.ter`: Terrain bitmap for map 0 — starts with 'B','M' (verified Windows BMP). 37.1 KB, larger than sub-map .ter files.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/maps/evergreen.obl`: Link table for map 0; every .obl is the same fixed size (~200 KB). Contains printable link/scene/music strings.
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/maps/evergreen.mon`: Monster-placement file; every .mon is the same fixed size (~269.5 KB).
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/maps/objects.obr`: Named rectangles into objects.bmp: house, reservoir, pyramid, Town Well, Cave Entrance, joshWell, ... (link-point icons).
- `/home/jshield/src/games/WoS/extracted/worlds/Evergreen/QuestScenes20.txt`: Concrete SCENE 20 script example with SCENE/THEME/MUSIC/ACTOR/POSE/IF/GOTO/GIVE/SOUND/MOVE/END.
- `/home/jshield/src/games/WoS/extracted/WELLOFSOULS.HLP`: In-game WinHelp; contains the real-time battle description and the note that .her files are encrypted.
- `http://www.synthetic-reality.com/wosquest.htm`: Uncle Dan's Quest Language Dictionary (A94) — the full ~70-command QUEST reference with syntax and examples.
- `http://www.synthetic-reality.com/wosArt.htm`: Art submission page — hero, monster and villager BMP frame layouts, guideline/control-pixel rules.
- `http://www.synthetic-reality.com/wosDev0.htm`: World developer overview — folder/file inventory, table conventions, #include, built-in editors.
- `http://www.synthetic-reality.com/wosDev1.htm`: Story and Place — maps/scenes/link points, link-point properties, well/incarnate flow.
- `http://www.synthetic-reality.com/wosDev2.htm`: Heroes and Monsters — class ID scheme, monster table, monster algorithms, ways to place monsters.
- `http://www.synthetic-reality.com/wosFAQ.htm`: FAQ — training/PP curve, charge meters, dexterity/agility miss rules, magic-ratio fizzling, element wheel.
- `http://www.synthetic-reality.com/wosSuperFAQ.htm`: SuperFAQ — C:\WoS\Save\<World>\savedHeroes\*.her layout, soul ID / modder / cheater semantics.
