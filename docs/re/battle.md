# Combat system (Souls.exe)

## Summary

Documented the full WoS A96 combat subsystem: how fights start (map .mon placement roll in FUN_00464daf, quest FIGHT opcode, groups.txt selection in FUN_0049099b), the complete monsters.txt→memory mapping (584-byte records at DAT_004e4888, all 21 args incl. flags byte at +0x124, colour/xparent at +0x0E8, sounds, attack path, AI), the 144-slot combatant array (0x6E0 stride at DAT_004e4874+0x128) with every field used in combat, the round-based real-time state machine FUN_00490e7c + actor picker FUN_0048f913, player input/target selection, the hit-roll FUN_004904eb and the full damage roller FUN_004a7794, HP/MP application, regeneration gauges, monster flee, win/loss test FUN_0048fd90, and the payout/level-up chain FUN_0042bb5c with the levels.txt XP/HP/MP tables. Also nailed the monster art-sheet format: 8-bpp palettised BMP, square cells whose size equals the bitmap height, frames left-to-right in row 0, colour key = bottom-left pixel of cell 0, shadow/flat test = bottom-right pixel, plus the 4x4 small-effect grid for indices < 16.

## Architecture

Combat in WoS A96 is a real-time, round-based "scene" subsystem built on three parallel tables (heroes, items/spells, monsters) plus a 144-slot combatant array in a per-scene heap block. A small state machine (FUN_00490e7c) drives rounds; a rotating cursor (FUN_0048f913) picks the next actor; a single shared damage roller (FUN_004a7794) resolves every hit; and FUN_0042bb5c pays out gold/XP/level-ups at the end. Monsters come from either the map's .mon placement table (FUN_00464daf) or groups.txt (FUN_0049099b) or an explicit quest `FIGHT` list. Art is one horizontal strip of square cells per skin .bmp where the cell size equals the bitmap height, with the bottom-left pixel of the first cell acting as the palette colour key.

# Well of Souls — Combat ("scene"/fight) system, solo play

All addresses are Souls.exe VAs. Symbols are Ghidra-generated; the code has no symbols, so I named things by behaviour. Everything below was read out of `work/decomp/all.c` unless marked otherwise.

---

## 0. The three tables and one runtime array

| Structure | Base | Stride | Count | Notes |
|---|---|---|---|---|
| Monster table (`monsters.txt`) | `DAT_004e4888` | `0x248` (584 B) | 4096 | allocated `FUN_00416b1f(DAT_004e4888, 0x248000)` |
| Spell table (`spells.txt`) | `DAT_004e4880` | `0x188` (392 B) | 768 | `0x49800` bytes |
| Item table (`items.txt`) | `DAT_004e4884` | `0x30c` (780 B) | 5120 | `0x13ff*0x30c` |
| Monster groups (`groups.txt`) | `0x008e5b58` | `0x2C` (44 B = 11 ints) | 4096 | `[+0]`=in-use, `[+1]`=count, `[+2..+10]`=monster ids |
| Level/class table (`levels.txt`) | `DAT_004e488c` | `0x6B34` per class | — | int arrays at `+0x1A0A4` (XP lo), `+0x1A0A8` (XP hi), `+0x1A234` (cumulative XP), `+0x1A238` (maxHP), `+0x1A3CC` (maxMP) |
| Hero records | `DAT_0067fbf8` | `0x16CC` (5836 B) | 64 (`0x5B3` ints) | |
| **Combatant array (the fight!)** | `DAT_004e4874 + 0x128` | `0x6E0` (1760 B) | 144 (`0x90`) | loop bound everywhere is `< 0x3DE00`; whole block `0x3E0A0` bytes allocated in `FUN_0048e19a` (0x48E19A) |

The combatant record is the heart of combat. Selected fields (offsets from record start):

```
+0x000  [0]      type: 1 = in use, 2 = dead/corpse, 0 = free
+0x004  [1]      owner id:  -1 = MONSTER,  -2 = NPC/actor,  >=0 = player account id
+0x008  [+2]     name[32]
+0x029  skinName[32]
+0x046  growl wav[80]
+0x079  pain wav[80]           (runtime copy)
+0x0C9  (monster) third sound[80]
+0x124  (+0x29)  skin bmp handle (HBITMAP wrapper)
+0x128  sprite/palette object
+0x128..0x380:  [0x4A..0xE0] spell-effect slots (0x398 + i*4, i<0x23=35)
+0x284  [0xA1]   target/goal y
+0x290  [0xA4]   anim phase accumulator
+0x294  [0xA5]   "is not a monster" flag (XORed into the flip)
+0x2A8  [0xAA]   max HP        (FUN_0042bb5c 0x42bf82: rec[0xAA] = hero[0x74], max HP)
+0x2AC  [0xAB]   max MP        (FUN_0042bb5c 0x42bf8b: rec[0xAB] = hero[0x7C], max MP)
+0x2B0  [0xAC]   gold reward (monsters)
+0x2B4  [0xAD]   XP reward (monsters)
+0x2E8  [0xBA]   queued action: 0 = physical, >0 = spell id, -1 cancel, -3..-5 special, -4 = give gold
+0x2EC  [0xBB]   current target slot
+0x2F0  [0xBC]   secondary target / give-gold amount
+0x2F8  [0xBE]   equipped right-hand item id
+0x380  [0xE0]   last damage delta
+0x384  [0xE1]   tick of last damage
+0x388  [0xE2]   anim state override
+0x5B8  current HP   SEALED (FUN_0049b71b). This, not +0x2A8, is the live HP a fight reads.
+0x5F0  current MP   SEALED (FUN_0049b71b). Likewise the live MP, against +0x2AC's max MP.
The +0x2A8/+0x2AC pair used to be labelled "current HP"/"max HP" here and that was wrong, and it
misled four separate readings of FUN_0042BB5C and FUN_00480499: both write +0x2A8 from a MAX HP
source (hero+0x74, and the monster table's +0xF4) and +0x2AC from a MAX MP source (hero+0x7C, and
+0xF8). The current values live in the two SEALED EncInts at +0x5B8 and +0x5F0. Every FUN_0049b71B
site in the binary targets one of the five EncInts +0x5B8/+0x5F0/+0x628/+0x660/+0x698; +0x2A8,
+0x2AC, +0x2B0 and +0x2B4 are plain words everywhere they appear.
+0x38C  [0xE3]   *** TURN STATE ***  0x2F = "not this actor's turn / done", 0x5F = READY, 0x94 = committed
+0x390  [0xE4]   attack rating (weapon/derived)
+0x398  [0xE5]   last action was a spell
+0x39C  [0xE6]   misc
+0x3A0  [0xE8]   screen x   (1/256 of scene width, 360 logical)
+0x3A4  [0xE9]   screen y   (1/256 of scene height, 256 logical)
+0x3A8  [0xEA]   current anim x
+0x424  [0x109]  slot alloc serial
+0x428  [0x10A]  *** number of attacks made *** (participation)
+0x42C  [0x10B]  strength   (0..255, item-modified)
+0x430  [0x10C]  wisdom
+0x434  [0x10D]  stamina
+0x438  [0x10E]  agility
+0x43C  [0x10F]  dexterity
+0x440  [0x110]  "ability" A (attack power)
+0x444  [0x111]  "ability" B (damage bonus)  [default 100]
+0x448  [0x112]  skin recolour/transform id   (monsters.txt colourTable)
+0x44C  [0x113]  hero index
+0x450  [0x114]  allegiance: 0x7FFFFFFF = ally, 0x7FFFFFFE = enemy, 0 = unaligned
+0x454  [0x115]  monster id (monsters.txt)
+0x468  [0x11A]  *** participation points ***
+0x45C  [0x117]  tick of last turn
+0x484  [0x121]  *** number of frames in this skin sheet ***
+0x29C  hero "TNL" (XP to next level, tenths) - hero record only
```

Hero record (`DAT_0067fbf8 + idx*0x16CC`) fields used by combat:

```
+0x00 in use, +0x04 soul id, +0x0C account id (== local player == DAT_004dd20c)
+0x14 name[16], +0x35 skin name[32], +0x68.. hero->combatant scratch
+0x60 soul CLASS index (selects the levels.txt block)
+0x64 LEVEL
+0x68 total experience points
+0x6C gold
+0x70 max HP, +0x74 current HP
+0x78 max MP, +0x7C current MP
+0x6C8 = -XP (mirror), +0x728 = XP banked toward next level
+0xA70 = -level (mirror), +0xA44 = "best hits" counter
+0xCD0 best level, +0xCD4 XP at best level
+0x680 strength, +0x684 wisdom, +0x688 stamina, +0x68C agility, +0x690 dexterity
+0x724 death count, +0xCD0/CD4 best level
```
(Verified against `FUN_0042bb5c` @0x42BB5C, `FUN_0042baa8` @0x42BAA8, `FUN_0042baf3` @0x42BAF3, and the desync checker `FUN_00491bd1` @0x491BD1 which asserts hero+0x680..0x690 == combatant+0x42C..0x43C.)

---

## 1. Starting a battle

### 1a. Random encounter while walking the map
`FUN_00464daf` (0x464DAF). Map monster placements live at `DAT_00636808`, stride `0x45` ints per map entry: `[0]`=monster id, `[1]`=x, `[2]`=y, `[3]`=radius (all in 1/256 units).

```
for each placement p with valid id:
    d = dist(p.xy, hero.xy)                      // hero xy = hero+0x94/0x98 >> 8
    if d < p.radius:
        spawn p if rand()%100 < 25               // 0x19
        if d < p.radius * _DAT_004cd548:
            spawn p again
            spawn a 2nd copy if rand()%100 < 15  // 0x0f
        if d < p.radius * _DAT_004cd578:
            spawn p again
            spawn a 3rd copy if rand()%100 < 5   // 5
if nothing spawned: spawn the NEAREST placement, or monster id 1 if none
```
Each spawn is `FUN_00480499(monsterId, 0, 1, 0, 0, 0)` followed by `FUN_0047c1e4(monsterId,0)` (mark "seen" in the save file) and `DAT_0067fc00[id*4] = 1`. The constants `_DAT_004cd548`/`_DAT_004cd578` are the two inner falloff fractions (exact float values not recovered — [UNVERIFIED], they are > 0 and the second is smaller than the first).

### 1b. Quest `FIGHT` opcode
Per `quest.txt:992-1053`:
* `FIGHT` with no args = "standard random fight appropriate to vicinity" → the engine runs **SCENE 2**, whose single `FIGHT` opcode resolves to the `groups.txt` path below.
* `FIGHT 1,2,2,4` = spawn exactly monster ids 1, 2, 2, 4 (one combatant per list entry, duplicates allowed).
* `FIGHT 1,2,3,-4` = a **negative** id means that monster fights *on your side* (helper). In the combatant record, allies get `+0x114 = 0x7FFFFFFF` (`FUN_00480499` param2==1) and enemies `0x7FFFFFFE` (param2==2); `FUN_0048e62c` and `FUN_00448fd90` use those to pick targets and decide the winner.
* `FIGHT *, 1, 2, 3` = the listed monsters *plus* the normal random encounter set.
* `FIGHT2` = same, but the scene host is also dragged into the fight until it stands still for ~20 s.
* The opcode is **blocking**: the next scene command only runs once the fight resolves; `IF WIN, @label` / `IF LOSE` then branch (`quest.txt:786` `WIN  Player won last fight in this scene`).
* Scene style: `SCENE n jpeg,FIGHT,"title",theme,monsterLevel` — a FIGHT scene inherits `jpeg`, `theme` and monster level from the closest link point (`quest.txt:551`). `SCENE 2` in Evergreen has no jpeg of its own, so the fight background is whatever the map link says (`extracted/scenes/fight.jpg` is the generic one).

### 1c. `groups.txt` selection (the no-argument FIGHT)
`FUN_0049099b` (0x49099B), logged as `"PickRandomMonster link %d, %d %d %d%% => %d monsters"`.

```
link  = DAT_004f2240                       // current map link index
diff  = linkTable[link*200 + 0]            // @0x00604870, signed "monster difficulty level"
rawPct= linkTable[link*200 + 74]           // @0x00604998 (offset 0x128)
pct   = clamp(rawPct, 20, 80)
if (diff < 0) pct = 100 - pct               // negative difficulty => harder near the link
grp   = &groups[abs(diff)]                 // difficulty -3 and 3 both use group 3
for i in 0 .. grp.count-1:
    m = grp.list[i]
    if (link[74] != 0 && m == 0) continue  // skip the "always" slot
    if (rand()%100 < pct || DAT_00502a38): // DAT_00502a38 = groups.txt row "0, 1" => spawn all
        spawn m;  mark seen;  count++
if count == 0 and grp.count > 0: spawn one at random from the list
```
The distance weighting described in `groups.txt`'s header ("probability increases with distance from the link") is realised through link field 74 (clamped 20..80 %) combined with the sign of the difficulty — a positive difficulty means an easier list near the link.

Groups loader (inside `FUN_00483686`, 0x483686, code at 0x4837xx): `(&DAT_008e5b58)[id*0xB] = 1` as the in-use flag; id 0 stores `DAT_00502a38 = atoi(arg1)` (the "not random" switch).

---

## 2. `monsters.txt` — every column, and where it lands in memory

Loader: `FUN_004809a3` (0x4809A3). Record = 584 bytes, id must be 0..4095, ids must be unique, at least 12 args required (`"*****Not enough arguments in Mons..."`). The loader reads comma-separated tokens, strips quotes, and blanks unused commas.

| arg | column | in-memory | meaning / algorithm |
|---|---|---|---|
| 0 | ID# | `[0]` +0x000 (set to 1) | 1..4095, unique. **0 is special**: it is the "Algorithm Scale %" row. |
| 1 | name (16 char) | `[1]` +0x004, 32 B, NUL at +0x24 | drawn above the monster's head |
| 2 | skinName | +0x025, 32 B, NUL at +0x45 | BMP base name, no extension. `"mirror"` ⇒ draw the local player's skin. |
| 3 | `scale.flags.colorTable.xparent` | `+0x0E8` (colour/xparent), `+0x124` (**flags byte**, tested with `&4`, `&8`, `&0x10`) | scale −8..+8 (0.20..3.00 ×); flags 1 no-tame, 2 own-element-only, 4 may summon, 8 magic-only, 16 only-Arg20-AI, 32 never-flee; colorTable 0..32 (RGB swaps/overwrites/dim/brighten/gray/invert); xparent=1 ⇒ translucent. Exact bit packing of scale+colorTable+xparent inside `+0x0E8` is [UNVERIFIED]; the *flags* byte at `+0x124` is confirmed by `*(byte*)(rec+0x49) & 4 / &8 / &0x10` at 0x105851/0x105857. |
| 4 | `element.alignment` | `[0x3C]` +0x0F0 = element; `+0x128` (`[0x4A]`, 15 chars) = alignment | 0..7 base elements, 8..255 chaos. Alignment is only used by the `#<cookie>` system. |
| 5 | hp | `[0x3D]` +0x0F4 | 1..N, or 0 = from level algorithm |
| 6 | mp | `[0x3E]` +0x0F8 | 1..N, or 0 = from level algorithm |
| 7 | defense | `[0x3F]` +0x0FC | 1..N, or 0 = algorithm |
| 8 | offense | `[0x40]` +0x100 | 1..N, or 0 = algorithm |
| 9 | expPts | `[0x41]` +0x104 | 1..N; 0 = algorithm; −2 = none. Read at 0x41771 as `rec+0x104`. |
| 10 | gold | `[0x42]` +0x108 | 1..N; 0 = algorithm; −2 = none |
| 11 | level | `[0x3B]` +0x0EC | the core scaling value |
| 12 | strength | `[0x43]` +0x10C | 1..255, 0 = algorithm |
| 13 | stamina | `[0x44]` +0x110 | 1..255, 0 = algorithm |
| 14 | agility | `[0x45]` +0x114 | 1..255, 0 = algorithm |
| 15 | dexterity | `[0x46]` +0x118 | 1..255, 0 = algorithm |
| 16 | wisdom | `[0x47]` +0x11C | 1..255; **0 = never casts**; −1 = algorithm |
| 17 | growl wav | +0x096, 80 B | if empty a random standard growl is used (`FUN_0048fe21`) |
| 18 | pain wav | +0x046, 80 B | if empty a random standard pain sound |
| 19 | attack path | `[0x48]` +0x120, packed 30 bits | see §6 |
| 20 | ai command | +0x051, 256 B | pet-style command language; if non-empty the monster runs its own AI (`[0x3B8] != 0` branch at 0x106349) and prints message table entry 0x3C. |

Post-load fixup (same function, 0x480DF6..0x481116): any ability still `<1` (or `<0` for wisdom) is replaced by `FUN_00479509(value, percentage)`; the four "algorithm from level" percentages are the *functions* `FUN_004808e5` (0x48A/100), `FUN_004808f8` (0x201/100), `FUN_0048090b` (0x237/100), `FUN_0048091e` (0x342/100) — i.e. **hp×1.162, mp×2.010, offense×2.310, defense×3.338**, then randomised. `expPts` falls back to `FUN_004807d3`; `gold` falls back to `xp*4/3*50/(level+50)` randomised, minimum 5. Finally the whole array is hashed per record into `DAT_007c5e34[id]` so hot-reloads can detect changes.

### Monster record 0 — the global difficulty/balance row
`0,"Algorithm Scale %",0, 0, 0, 100,100,100,100,100,100, 0, 100,100,100,100,100`
Fields hp/mp/defense/offense/exp/gold/strength/stamina/agility/dexterity/wisdom are **percentages (0..100)** multiplied into every algorithm-computed value; missing or 0 means 100 %. Level and exp are left unscaled.

### The XP/gold algorithms
`FUN_004807d3` (0x4807D3) — XP for a monster of level L:
```
xp = ((((( L%28*300 )/300) * (offense+200))/200 * (defense+300))/300 * (L+70))/70
xp = clamp(xp, 1, 60000)
```
i.e. `xp = (L*300/300) * (off+200)/200 * (def+300)/300 * (L+70)/70`, clamped to [1,60000].

`FUN_00480875` (0x480875) — actual award: takes that value and randomises it: `xp = rand() * xp / level` (so low-level monsters swing wildly), clamp [0,60000].

Death payout (`FUN_00494fcd`, 0x494FCD): `sceneGold += rand()%(monsterGold+1) + monsterGold/2` (clamp 32767), `sceneXP += FUN_00480875(target)`.

---

## 3. Spawning a monster into the fight — `FUN_00480499` (0x480499)

```
FUN_00480499(monsterId, side, doInit, xPct, yPct, forceFlag)
  slot = FUN_00491e45(-1)                     // -1 == "new monster"
  if slot < 0: return slot
  rec = combatant[slot]
  FUN_00480943(monsterId)                      // returns 0 if the table changed under us
  strncpy(rec+0x008, mon+0x004,  31)           // name
  strncpy(rec+0x029, mon+0x025,  31)           // skin
  strncpy(rec+0x0C9, mon+0x096,  79)           // growl
  strncpy(rec+0x079, mon+0x046,  79)           // pain
  strncpy(rec+0x124, mon+0x051, 256)           // AI command
  rec[0x49]  = 1                               // skin loaded?
  rec[0xA9]  = mon[0x3C]                       // element
  rec[0xAA]  = mon[0x3D]                       // hp   (cur = max)
  rec[0xAB]  = mon[0x3E]                       // mp
  rec[0xAD]  = mon[0x41]                       // xp reward
  rec[0xAC]  = mon[0x42]                       // gold reward
  rec[0x11E] = mon[0x4F]; rec[0x120] = mon[0x50]   // growl/pain sound ids
  rec[0x10B] = mon[0x43]  strength
  rec[0x10D] = mon[0x44]  stamina
  rec[0x10E] = mon[0x45]  agility
  rec[0x10F] = mon[0x46]  dexterity
  rec[0x10C] = mon[0x47]  wisdom
  rec[0x111] = 100                            // ability B
  rec[0x110] = clamp(wisdom/4 + 50, .., 100)  // ability A
  rec[0x112] = mon[0x3A]                       // colourTable / xparent
  rec[0x115] = monsterId
  if side == 1 (ally): rec[0x114] = 0x7FFFFFFF
  if side == 2 (enemy): rec[0x114] = 0x7FFFFFFE
  x,y = FUN_004931c9(nextOrdinal, &x, &y, 1)   // formation slot
  rec[0x9E] = clientWidth + rand()%32          // enters from off-screen right
  rec[0x9F] = y; rec[0xA0] = x; rec[0xA1] = y  // "walk to" target
  rec[0x114] = 0x7FFFFFFF / 0x7FFFFFFE as above
  FUN_00491bb7(rec)                            // recompute derived stats
  FUN_00496982(rec)                            // post-init (equip defaults etc.)
```

`FUN_00491e45` (0x491E45) allocates the slot: 144 records, reuse an existing one with the same id, else the first free one, else fail. Defaults `[0x124]=1, [0x110]=[0x111]=100, [0xAA]=[0xAC]=[0xAD]=100, [0x38C]=0x2F`. For `id == -1` (monster) it uses formation grid 0 and `x = -(rand()%64)`; for `id == -2` (NPC) grid side 1 and `x = 0, y = 0`; for a player id, grid side 1 and `x = clientWidth + rand()%32`, plus a random start tick `tick - rand()%5000 - 3000`.

`FUN_004931c9` (0x4931C9) — formation, in 1/256 units, `ordinal % 9`:
```
row = tbl_x[side][idx]        // 0x502A58 / 0x502AA8
col = tbl_y[side][idx]        // 0x502A80 / 0x502AD0
x = col*0x24 + 0x12                       // 18 + col*36
y = (row+1)*0x20 + (col even ? 0x10 : 0) + 0x10
```
The per-ordinal row/col tables are runtime state (never written in the decompiled code) — treat as a 3×3 grid with the formula above; the hero takes the first ordinal and monsters follow to its right in practice. [UNVERIFIED] on which physical side the hero ends up on; the code only fixes "hero ordinal < monster ordinals, all enter from the right edge".

---

## 4. Battle screen layout and drawing

**Logical scene space: 360 × 256** (`extracted/scenes/readme.txt`; `0x168` and `0x100` appear literally in the code). Horizontal scroll only.

* Background: `FUN_0048a316` (0x48A316) → `FUN_0048a1a7` (0x48A1A7) tries `"%s/worlds/%s/scenes/%s.jpg"` then `"%s/scenes/%s.jpg"`, with a decoded-bitmap cache in `"%s/temp/sceneCache%d_%s.jpg"`. Scaled to the client rect (`FUN_004864e0` builds an 8-bpp DIB section sized to the client area).
* Screen mapping: `FUN_00493306` (0x493306) `sx = (logicalX - scrollX) * clientWidth / 360`; y is `(logicalY * clientHeight) / 256`. Scroll origin `DAT_0050280c`, camera `FUN_0048aada` (0x48AADA) eases toward the average hero x or the current attacker (`FUN_0048aa42` 0x48AA42), clamped to [0,360]. Cursor changes per interaction mode in `FUN_00495e6e` (0x495E6E).
* Ground: y is clamped to ≥ 0x80 (128) for walking (`FUN_0048f913`), i.e. the bottom half — that is the "horizon" (`cave.300.25` style names encode width×3 and horizon 25 % from the top).
* Scene view paint: `FUN_0049331e` (0x49331E). Two passes: pass 1 draws backgrounds/behind-actors via `FUN_004927fa`; pass 2 draws, for each of the 144 combatants that is type 1 (or type 2 when `DAT_004e6f6c`), its sprite (`FUN_004928f7/fa/fd` for effect overlays, `FUN_00416426` for the body) and then its **name plate** with a typewriter reveal:
  `chars = (GetTickCount() - rec[0x9B]) * 0x1E / 1000` (30 chars/s), plate rect `w = min(clientW/3, 200)`, `h = min(clientH/4, 120)`, positioned at the sprite's feet (`FUN_004932d6` 0x4932D6 tests visibility).
* Bottom panel: `FUN_00477c6b` (0x477C6B) — the button bar and the two status bars.

**Button bar** (10 slots, `FUN_00478673` 0x478673):
* Hit rect (`FUN_004787b2` 0x487B2): `(clientW-0x30 - i*0x33 - 3, top+8) .. (clientW - i*0x33, top+0x38)` → 48×48 px cells laid out **right to left**, 51 px pitch. `FUN_0047876b` maps a click point to a slot.
* Each button image is a `.bmp` whose cells are `0x30 × 0x30`, indexed horizontally by a state index (0..5) chosen from the current state and a `GetTickCount() & 0x300` blink (0x478473 region): states 1 = normal, 2 = disabled, 3 = pressed, 4/5 = blink frames. A 3×3 hand-drawn 3-D button is synthesised when the image is missing.
* In a FIGHT scene the bar is: `Items, Spells, Equip, Stats, Map, [Exit | Flee]` (`FUN_004789d5` 0x4789D5 + `FUN_00478e5a` 0x478E5A). Button 5's action code is `0x47F` (Flee) in a fight, `0x480` (Exit) otherwise; `0x482` = Camp, `0x484` = Map, `0x485` = Stats.

**Status bars** (`FUN_00477a74` 0x477A74, called from `FUN_00477c6b`):
```
rect inflated by (-2,-3), filled black
fill 0x2100A5 (dark blue)  to width = boxW * cur / max        // HP,  max=hero+0x70 cur=hero+0x74
fill 0xFF8080 (red)         to width = boxW * cur / max        // MP,  max=hero+0x78 cur=hero+0x7C
fill 0x808080 then 0xFFFFFF  to width = boxW * regenPct / 100   // regeneration gauge overlay
text  "%d/%d"
```
Numeric formatting `FUN_00477beb` (0x477BEB): `<10000 → "%d"`, `<1000000 → "%dK"`, else `"%d.%dM"`. The one-line summary is `"%s  PP:%s  TNL:%s %s"` = gold, `FUN_0042b829(class, level+2, xp)` (XP still needed to reach level+2), `FUN_0042b6f3(class, level)` (XP to next level in tenths, with `" (full)"` appended at `DAT_004e1018`), and the trophy counter `"vC %d"`.

**Regeneration gauges** (`FUN_00478840`, 0x478840, runs on the panel timer):
```
regenHP = min(100, (now - regenHPStart) * (strength/2 + 100) / 10000)
regenMP = min(100, (now - regenMPStart) * (wisdom  /2 + 100) / 10000)
```
Both start at **25** when the scene opens (`FUN_0048e19a` 0x48E19A sets `DAT_004f91c4 = DAT_004f91c8 = 0x19`) and are reset to 0 (timer restarted) every time the hero takes an action, physical or magical (`FUN_0048b1ad` 0x48B1AD, 0x102908/0x102927). When the hero acts the gauge is cashed in as a heal: `FUN_0048b069` (0x48B069) `heal = (gauge + 15) * attribute / 30`, then `FUN_00419306` (HP path, +0x14) or `FUN_004258b8` (MP path, `spellMP*0x14/2 + 0x14`), and `hero[0x28E]++` (heals-used counter). This is the WoS "attack = small heal" mechanic. **There is no separate attack-speed gauge** — see §5.

---

## 5. Timing, turn order, and how the player attacks

**It is real time, but quantised into rounds.** `FUN_00490e7c` (0x490E7C) is the state machine; state is `*(int*)(view+0x84)`:

| state | action |
|---|---|
| 0 | idle (jump to 1) |
| 1 | roll a random map encounter; the 5-second housekeeping tick `FUN_00495cf0` runs here |
| 2 | wait until `now - scene[0x3E038] > scene[0x3E03C]` ms, then 1 |
| 3 | → 5 |
| 4 | `FUN_0049099b()` (groups.txt pick), `DAT_005006b8 = 1`, `view+0x78 = 1`, → 5 |
| 5 | **start of round**: every live combatant with `[0x38C] != 0x2F` gets `FUN_00491bd1` (desync check) and `[0x38C] = 0x5F` (READY); `DAT_005006b4 = 1` if the local hero is among them; `_DAT_005006cc = GetTickCount()` = round start; poison/DoT ticks `FUN_004a6c46`; then → 6 |
| 6 | `FUN_00490723()` (one actor acts) — see below |
| 7 | apply the pending damage |
| 8 | wrap-up: every local-hero combatant walks off (`x = -0x32`) unless already fleeing, → 1 |
| 9, 10, 11 | ASK / countdown / misc script waits |

**Actor selection** `FUN_0048f913` (0x48F913): a rotating cursor `DAT_00502994` walks the 144 slots (`(cursor + n) % 0x90`, max 144 probes) and returns the first live, ready combatant. Any combatant whose `[0x38C] == 0x94` is returned immediately. While scanning it also (a) advances fleeing monsters off the left edge (`x < -10` → run away) and (b) does the wander/retarget step described in §7. Because the cursor rotates, this is round-robin.

**Round pacing**: `FUN_00490723` (0x490723) returns state `5` (start a new round) once `GetTickCount() - roundStart > 2000 ms` with nobody having acted. So a round lasts at most ~2 s of idling; otherwise it ends as soon as every combatant has committed.

**Player input** — `FUN_0048b1ad` (0x48B1AD), the scene window's `WM_LBUTTONDOWN`. Mode is `view+0xF4`:
* `0` = walk/click-to-move: `x = scrollX + mouseX*360/clientW`, `y = mouseY*256/clientH`.
* `1` = item use, `2` = **spell/attack target selection**, `3` = ?, `4` = give gold, `5` = flee/cancel.

In target mode the current target is `scene[0x3E044]`; clicking a combatant (hit test on the sprite/name rect) validates it (`FUN_0048b020` 0x48B020) and then:
```
rec[0xBA] = spellId (0 = physical, -1 = abort)
rec[0xBB] = targetSlot
rec[0x38C] = 0x2F            // committed
```
so the attack is **queued, not immediate**. The gate is `FUN_0048b17a` (0x48B17A): the hero may only act when `[0x38C] == 0x5F` and, if it is this round's actor, at least **500 ms** have passed since the round started. Once the turn reaches the hero, the state machine fires the queued attack.

**Monster turn** `FUN_00490723` (0x490723):
```
actor = FUN_0048f913()            // monsters ([1]==-1) and NPCs ([1]==-2) only
if actor: 
   actor[0x38C] = 0x2F
   FUN_00490645(actor)             // re-pick a target / a spell if the current one died
   if actor[0x3B8]:                // monsters.txt arg20 AI command present
        actor[0x2EC] = FUN_0048e62c(actor, 0, 1)
   code = FUN_004904eb(actor, actor[0x2E8], actor[0x2EC], actor[0x2F0])   // hit/miss roll
   FUN_0048fe80(actor, actor[0x2E8], actor[0x2EC], actor[0x2F0], code)     // announce + animate
   return 7
```
`FUN_00490645` (0x490645): if the current target is gone, prefer a spell when `actor[0x2E8] != 0` or the spell is "free of failure" (`spell[0x124] != 0`), else a physical target — `FUN_0048e62c(actor, 1 or 2, 1)`.

**Target selection** `FUN_0048e62c` (0x48E62C) simply builds a list of every legal enemy slot (`FUN_00448b020` "can I hit this" + `FUN_0048e4d9` 0x48E4D9 team/PvP legality) and returns a **uniformly random** one. There is no threat-based AI for plain monsters; monsters with an arg20 AI command use the pet command language instead.

**Attack announcement** `FUN_0048fe80` (0x48FE80) sets the shared "current attack" block in the scene and starts the animation:
```
scene[0x3E048] = spellId (0 = physical)
scene[0x3E04C] = extra
scene[0x3E050] = attacker slot
scene[0x3E054] = target slot
scene[0x3E058] = GetTickCount()
scene[0x3E05C] = duration ms : 1000 physical, 4000 spell, 1250 for the special -3/-4/-5 actions
scene[0x3E068] = damage
scene[0x3E070..0x3E07C] = same info for the display layer
actor[0x124] = 2;  if (spellId > 0 && actor[0x484] > 6) actor[0x124] = 6;   // animation pose
actor[0x294] = (targetX < actorX)                                   // facing
actor[0x428]++                                                      // participation
```
then prints `"%s attacks %s"` or `"%s casts %s on %s[ and team]"`, and plays the fist/weapon/spell sound (`fist.wav` by default). Damage itself is computed by `FUN_004a4259` (0x4A4259) and resolved in state 7 via `FUN_004a8677` → `FUN_004a7794`.

**Attack path** `FUN_0048ccf2` (0x48CCF2) decodes the packed `attackPath.imageID.flags.weather.effect` (monsters.txt arg19 / items.txt arg16) into five 6-bit fields plus a duration:
```
f0 = bits>>26, f1 = (bits>>18)&0x3F, f2 = (bits>>12)&0x3F, f3 = (bits>>6)&0x3F, f4 = bits&0x3F
dur = [1000, 1500, 2000, 3000][f2 & 3];  if (f0 >= 6 && f0 < 8) dur = dur*3/2
```
`items.txt` documents f0 as LUNGE/JUMP/LEAP/STAB/DBL-STAB/TRAMPLE/HOP (mover) and 30..33 ARROW/STONE/LOB/Swoop (a projectile image from `art/attackNN.bmp`, 8 frames per strip, 10 strips per file, `image 143 → strip 3 of attack14.bmp`). `FUN_0048cdf3` (0x48CDF3) implements the mover: it offsets the sprite along `sin(elapsed/duration * PI)` between start and target for physical attacks, and sets the flip flag.

---

## 6. Monster art sheet format (`monsters/*.bmp`, `skins/*.bmp`)

Loading `FUN_0048df3c` (0x48DF3C) + `FUN_004864e0` (0x4864E0) + `FUN_004864c0` (0x4864C0); blitting `FUN_00416426` (0x416426) + `FUN_00486d20` (0x486D20).

**The format, exactly:**
1. Plain Windows **8-bit palettised** BMP (`biBitCount = 8`, 256-entry palette, loaded into a `CreateDIBSection` DC and blitted through the palette — `SetDIBColorTable` at 0x4865xx).
2. It is **one horizontal filmstrip of square cells**. Cell size = **the bitmap's height** (`FUN_004864c0` returns `abs(biHeight)`). The number of frames in the strip = `biWidth / biHeight`, cached at `combatant+0x484` by `FUN_0048df3c`:
   `frames = (bitmap width rounded to 4) / cellSize`.
3. All frames live in **row 0**. Frame `f` is source rect `(f*cell, 0) .. (f*cell+cell, cell)`, with `f` clamped to `[1, frames]` (`FUN_00416426`, 0x4169xx).
4. **Colour key = the palette index of the pixel at the bottom-LEFT corner of the first cell**, i.e. byte `pix[(cell-1)*stride + 0]`; every pixel with that index is transparent. The **bottom-RIGHT** corner of the first cell, `pix[(cell-1)*stride + 1]`, is the "edge" colour: if it equals the key the sheet is treated as flat (no ground shadow, `param_7 &= 2`).
5. `FUN_00486d20` handles the actual palette-indexed copy with clipping; `param_7` selects the effect: 1 = plain transparent, 2/3/5 add wobble/dim variants.
6. **Animation index** passed to the blitter is `animState + 0x10`:
   * `index < 0x10` → a **4×4 grid of small cells** in the *lower* part of the image, each `cell/3` square, at `(index&3)*cell/3 , ((index>>2)&3)*cell/3` — these are the small hit/spark effects.
   * `index >= 0x10` → `frame = index - 0x10` in the main top strip.
7. Anim states seen: `0x2F` = not acting (skipped), `0x5F` = READY, `0x94` = committed, and `1..6` for poses (attack uses **2**, or **6** for a big attack when the sheet has more than 6 frames). `FUN_00485f21` (0x485F21) cycles idle poses 1/2/3 for NPCs on a 200 ms tick with a 0..14-frame dwell.
8. **Shadow**: an ellipse `cell × cell/4` is filled with brush `0x808080` when the draw mode is 1.
9. **Colour table / xparent** (monsters.txt arg3) are applied when the sheet is loaded: `FUN_0048d8f4` (only for owner `-1`) and `FUN_004224e5` re-map the palette.
10. Path resolution `FUN_0048de8d` (0x48DE8D):
    * skinName == `"mirror"` → `"%s/skins/%s.bmp"` with the **player's own** skin name;
    * else if `"%s/worlds/%s/monsters/%s.bmp"` exists → use it;
    * else `"%s/monsters/%s.bmp"`.
    If the load still fails it falls back to `josh1` for monsters, or the hero's equipped right-hand item art for actors.

**Name plate**: drawn after the body with a typewriter effect and bounded to `min(clientW/3,200) × min(clientH/4,120)`.

---

## 7. Damage, HP/MP, monster AI attacks

### 7a. To-hit roll — `FUN_004904eb` (0x4904EB)
```
miss = agility(target) - dexterity(attacker)      // FUN_004a761f, stats 0x67/0x68
if miss > 0:
    miss = min(miss, 50)
    bVar = rand()%100 < miss
    if attacker[0x394] != 0: bVar = false         // blinded/hit effects cancel the miss
if attack == 0 (physical):  return bVar ? 0 : -1  // 0 = hit, -1 = miss
if spell[0x124] == 0:        return spell[0x128]  // never fizzle -> return its own code
if bVar && spell[0x134] == 0: return 0
return -spell[0x128]
```
`FUN_004a761f` (0x4A761F) is the *effective* stat reader: base ability clamped to [0,255], then modified by the eight element-resist slots at `rec+0x258 + element*4` (negative = `base*(1+n)`, positive = `base/(1+n)`), then zeroed if the target is a "ghost" (`hero+0xA34 > 999999`).

### 7b. The damage roller — `FUN_004a7794` (0x4A7794)
Signature `FUN_004a7794(spellId, attackerSlot, targetSlot, hitCount, &result, &out2, buf)`.
Returns a **negative number = damage**, positive = heal; 0 = no effect; special codes:
`0x7FF7` trophy, `0x7FF8` summon, `0x7FF9` spell failure, `0x7FFA` cure, `0x7FFB` infection, `0x7FFC` give item, `0x7FFD` give gold, `0x7FFE` give item(s), `0x7FFF` MISS.

**Physical branch** (spellId == 0, no spell), per hit:
```
if a right-hand item is equipped (FUN_0048af50):
    a = strength                                   // FUN_004a761f(attacker,100)
    W = a * attacker[0x440]
    den = attacker[0x440] + attacker[0x444] + 30; if (den<2) den=2
    power = (a * attacker[0x444] + W) / den        // weighted average of the two abilities
    if weapon's element != attacker's element:  power = power * 50 / 100
else (bare hands):
    power = ((rand() * attacker[0x444]) / 100 + 100) * (strength + 65) * (that + 5) / 6500
if attacker[0x2F8] < 1 or target is a monster:                 // no elemental weapon
    raw = (power * 8000) / ((rand() + 40) * (targetStamina + 200))
else:                                                            // element multiplier
    raw = ((power*8000) / ((rand()+40) * (targetStamina+200))) * 35 / (elementMultiplier + 25)
raw = (raw * 200) / (rand() + 200)                              // second 0..1 roll
raw = FUN_004a6974(&raw, &critFlag, attackerFacingFlag, attackerIsMonster, critDivisor)
if (raw < 1) return -1                                          // total miss / immunity
if (both alive) raw = DAT_004e0ff0 * raw / 100                  // PvP damage scale
return -raw
```

**Critical hits / variance** `FUN_004a6974` (0x4A6974):
```
d = max(1, dmg*9/10)
if (dmg > 1) dmg = (rand()%dmg)/5 + d            // +-20 % around 90 %
if (rand()%critDivisor == 0) {                   // critDivisor = 50, or (25-ability)*2
    dmg = attackerIsMonster ? 2*d : (rand()%3 + 2)*d
    critFlag = 1
}
clamp to [-32768, 32000]                        // FUN_004a6956
```
`FUN_004a761f`-style ability reads also apply a `0xFFFFFF`-ish cap of 255.

**Spell branch** (spellId != 0, record size 0x188):
```
if (spell[0x128] == -300) "Summons Item"      -> 0x7FF8
if (spell[0x128] < -199)  "Summons Monster"   -> 0x7FF8
if (spell[0x124] != 0) {                        // has an effect component
    base = (attacker[0x440] * ((rand()+25) * (attackerDex-derived) * spellPower) / 2665) / 100
    if attacker is a player:
        base = (((rand()*attacker[0x444])/100 + 200) * base) / 200
        if spellPower < resist: base += (int)pow(1.1892, resist - spellPower)   // resistance curve
}
```
`pow(1.1892, ...)` is the elemental resistance/damage curve and appears twice (also in the physical path at 0x4A77xx), always as `if (resist*_DAT_004d17a8 > _DAT_004d1830) dmg += (int)pow(1.1892, resist*_DAT_004d17a8 - _DAT_004d1830)`.

The 0x188 spell record fields used: `[0x124]` = "can fizzle" flag, `[0x128]` = failure code, `[0x130]` = power, `[0x134]` = "never fizzle" / team flag, `[0x124+0x49]`=`[0x16D]` element, `[0x16D]` level requirement, `[0x170..0x17C]` = MP/GP/… costs, `[0x158]` = target flags (`&4` = single-target only).

### 7c. Applying it — `FUN_004a8677` (0x4A8677) / `FUN_004946b2` (0x4946B2) / `FUN_004a61fd` (0x4A61FD)
```
FUN_004a8677(spellId, attackerSlot, targetSlot, nTargets, &out, &out2, buf):
    r = FUN_004a7794(spellId, attackerSlot, targetSlot, nTargets, &out, &out2, buf)
    FUN_004a6956(&r)                 // clamp
    if (DAT_0050b500 != 1) r = 0      // "no effect this frame" (used to re-roll once)
    DAT_0050b508 = r
```
`FUN_004946b2` then formats the message and applies:
* `param_3 < 0` → `"%s loses %d hit points"`, `> 0` → `"%s gains %d hit points"`, `== 0` → `"%s's hit points are unchanged"`.
* HP: `FUN_004a6255` → `FUN_004a61fd(&cur, delta, max, force)`: if `cur > 0 || force || targetIsSlot1`, `cur = clamp(cur + delta, 0, max)` and it returns `wasAlive - isAlive`, i.e. **1 when the target just died**.
* MP is handled by the spell's own record.
* The delta/tick/result/message are latched into the target: `rec[0xE0]=delta, rec[0xE1]=tick, rec[0xBF]=result, rec[0xC0]=message`.
* Special codes print `Trophy`, `Refused Trophy`, `Summons X`, `The attack on %s fails with %s`, `%s is cured with %s`, `%s is infected with %s`, `Refused %s`, `Got Item`, `Refused Item`, `MISS` / `The attack misses %s`.

Death is detected in `FUN_0048e2ef` (0x48E2EF) (`roll < 1 && !noDeathFlag`) and handled by `FUN_00494fcd` (0x494FCD) which prints `"%s has been killed."`, increments `hero+0x724` (death count) for the local player, and — **this is the solo-mode behaviour** — when `DAT_00502a48 != 0` it fully resurrects the hero: XP is recomputed with `FUN_0042b765`, the level is reset with `FUN_0042b6c1`, and max HP/MP are restored from `FUN_0042b7c7`/`FUN_0042b7f8`, followed by a message and `FUN_004306f6(0x43,...)` (a stat/scene update broadcast, no-op offline).

### 7d. Monster AI cadence
There is no per-monster attack timer. A monster acts once per round because `FUN_00490723` only ever resolves one actor per state-6 tick and state 5 re-arms everyone; rounds are bounded to 2 s by the `2000 < now - roundStart` test. Attack animation time is 1000 ms (physical) / 4000 ms (spell), and `FUN_004915ed` (0x4915ED) interpolates positions every frame.

---

## 8. Fleeing

**Player "Flee" button** (action code `0x47F`, `FUN_00478e5a` 0x478E5A) → `FUN_0042b06a` (0x42B06A) → `FUN_00436c9d`; if that fails it prints `"You attempt to flee... but are dragged back"` (`s_You_attempt_to_flee__but_are_dra_004e8338`) and the fight continues. On success it exits the fight, stops the theme, and returns to the map.

**Monster flee**, in `FUN_0048f913` (0x48F913), evaluated when the monster's owner is `-1`, it is not dead (`[0x38C] != 0x2F`) and it has been idle > 2000 ms (`now - rec[0x45C]`):

1. *Cowardice* check `FUN_00448f816` → `FUN_0048f816` (0x48F816): requires no active "cowardice" effect and `rec[0x114] == 0`. For each live player hero it rolls `r1, r2`; if `r1 - r2 > 20` the flee chance is `(r1-r2)*100/500`; if that is > 50 **and** the monster's wisdom > 100 **and** its current HP > 0, the chance is boosted by `+100 - rand()*100/hp`. If it fires: `rec[0x1B4] = 1`, `rec[0xA0] = -0x1E` (walks off the left edge) and it prints `"%s takes fear of %s and flees."`
2. *Generic* flee: `chance = min(100, (rand()+4)*10)`; if `rand()%100 < chance` the monster sets `[0xBC] = -1, [0xBB] = -1, [0xBA] = 0`, picks a new target, and if it has no legal target left marks itself `[0x38C] = 0x2F` and leaves the fight. (The `(rand()+4)*10` floor of 40 % looks like a 10 %-step table whose low end is never reached in practice — [UNVERIFIED], but the code is unambiguous.)
3. A monster that wanders off past `x < -10` is removed: `FUN_004306f6(0x41,5,slot,…)` + `FUN_0048e16e` (clears the record, `[0x38C] = 0x2F`).
4. The "never flee" flag is `monsters.txt` arg3 flag **32** (masked with `FUN_0048afe7`).
5. After the fight, state 8 walks the hero off with `x = -0x32` unless `rec[0x1B4]` (already fled).

---

## 9. Victory, rewards, level-up

### Win/loss test `FUN_0048fd90` (0x48FD90)
```
players = monsters = 0
for each combatant with type>0, alive (FUN_0049b70f()/FUN_0048ef26) and owner > -2:
    if (owner == -1 && (allegiance==0 || allegiance==0x7FFFFFFE)) monsters++
    else                                                    players++
if (players && monsters) return 0
return (players - monsters) || 1
```
In `FUN_00490723`: `result = FUN_0048fd90(); if (result < 1) "The fight is over! We lost!" else "The fight is over! We won!"` — so a positive result (surviving players, no monsters) = **win**, matching the doc at 0x106320.

### Payout `FUN_0042bb5c` (winFlag, totalGold, totalXP, totalParticipation)
```
if (!win): play "lost" sting, return                       // s_lost_mid
count survivors: combatants with type==1, owner != -2, rec[0x10A] > 0
if survivors: totalXP /= survivors
FUN_00414ad1(totalXP)                                      // battle log / accounting
play "victory"/s_won_mid sting
for each surviving combatant with owner != -2:
    share = totalParticipation ? totalGold * rec[0x11A] / totalParticipation : totalGold
    xp    = totalParticipation ? totalXP   * rec[0x11A] / totalParticipation : totalXP
    cap   = FUN_0042baa8(hero)                              // gold ceiling
    share = min(share, cap - hero[0x6C]);  clamp >= 0
    if (level < 96) xp = min(xp, (FUN_0042b829(class, level+2, xp) + 3) / 2)
    if (owner == local player):
        if (no participation) print "You earned nothing from this battle."
        else:
            print "You earn %d %s and %d exp points."
            if (DAT_004e6f64) print "That's a %d/%d share of %d %s and %d exp points."
            FUN_0042b867(1+rand,rand,rand&0x400)             // trophy roll
    if (rec[0x10A] > 0 && !(cheat 0x800)):
        FUN_0042baf3(hero, share)                           // add gold
        hero[0x68] += xp                                     // total XP
        hero[0x6C8] = -hero[0x68]
        hero[0xEFC] = FUN_0042b6c1(class, hero[0x68])        // new level
        hero[0xCD0] = max(hero[0xCD0], hero[0x68])
        hero[0xCD4] = (was 0 ? hero[0x68] : hero[0xCD4] + xp)
        hero[0x64]  = FUN_0042b6c1(...)  via FUN_0042b6c1    // new level
        hero[0xA70] = -hero[0x64]
        hero[0x74]  = FUN_0042b7c7(class, level)             // new max HP
        hero[0x7C]  = FUN_0042b7f8(class, level)             // new max MP
        hero[0x70] = hero[0x74];  hero[0x78] = hero[0x7C]
        rec[0xAA]/[0xAB] = hero HP;  rec[0xAC]/[0xAD] = hero MP
        if (level went up):
            print "%s is now a level %d %s."        (FUN_0049c28b -> levels.txt name)
            print "Your HP rose by %d points." / "Your MP rose by %d points."
            play s_levelup sting
            FUN_00417f1b(world, DAT_0067fbf8)              // persist hero
print "The battle is over."  (FUN_004967e4(10, …))
```

### Level maths (all from `levels.txt`)
* `FUN_0042b6c1` (0x42B6C1) XP→level: walk the cumulative-XP array at `+0x1A234` **downwards** from 100; return 100−i for the first entry ≤ xp; floor 1. Level 100 shares level 99's row (per `levels.txt` header).
* `FUN_0042b829` (0x42B829) `xpToLevel(class, level, currentXp)` = `xpHi[class*0x6B34 + level] - currentXp` (0 if level ≥ 100 or the threshold isn't beaten).
* `FUN_0042b6f3` (0x42B6F3) "TNL" = `level*10 + clamp(((xp − xpLo)*10 + 9)/(xpHi − xpLo), 0, 9)` — the XP needed to the next level, in **tenths**.
* `FUN_0042b765` (0x42B765) interpolated XP at a fractional level: `lerp(xpLo[L], xpHi[L+1], level%10)/10`.
* `FUN_0042b7c7` / `FUN_0042b7f8` (0x42B7C7 / 0x42B7F8) max HP / max MP at a level, read from `+0x1A238` / `+0x1A3CC`, with a post-100 overflow term `(L-101)*50` for HP and `(L-101)*20` for MP.
* `FUN_0042baa8` (0x42BAA8) gold ceiling = `clamp((level+1)*10000, 0, 1000000)`, further limited by the class cap at `+0x1ABDC` if it is positive.
* `FUN_0042baf3` (0x42BAF3) `addGold` keeps `+0x25D` (a "banked/escrow" mirror) in sync and clamps to the ceiling.
* `levels.txt` arg2/arg3 are the per-level HP/MP increments, arg4 the level name, arg5 the magic ratio, arg6 the right-hand weapon class (1 Sword, 2 Staff, 3 Bow, 4 Instrument, 5 Fist, 6 Dart, 7 Book, 8 Spirit).

`FUN_0042b867` (0x42B867) is the trophy roll (called once per victorious player).

---

## 10. Message strings you will need (all from `work/decomp/all.c`)

```
"The fight is over! We lost!"                     s_The_fight_is_over__We_lost__00502e08
"The fight is over! We won!"                      s_The_fight_is_over__We_won__00502e24
"%s attacks %s"                                   s__s_attacks__s_00502da8
"%s casts %s on %s" [+ " and team"]                s__s_casts__s_on__s_00502d80 / s__and_team___00502d70
"%s casts %s on self"                             s__s_casts__s_on_self_00502d94
"%s uses %s"  (special actions)                    s__s_uses__s_on__s_00502db8
"%s gives %d %s to %s"                            s__s_gives__d__s_to__s_00502dcc
"%s takes fear of %s and flees."                  s__s_takes_fear_of__s_and_flees__00502d48
"%s has been killed."                             s__s_has_been_killed__00503118
"%s loses %d hit points"                          s__s_loses__d_hit_points_00503048
"%s gains %d hit points"                          s__s_gains__d_hit_points_00503010
"%s's hit points are unchanged"                   s__s_s_hit_points_are_unchanged_00503028
"The attack misses %s" / "MISS"                   s_The_attack_misses__s_005030f8 / s_MISS__00503110
"You earn %d %s and %d exp points."               s_You_earn__d__s_and__d_exp__point_004e8620
"You earned nothing from this battle."            s_You_earned_nothing_from_this_bat_004e85cc
"That's a %d/%d share of %d %s and %d exp points" s__That_s_a__d__d__share_of__d__s_a_004e85f4
"%s is now a level %d %s."                        s______s_is_now_a_level__d__s_____004e85ac
"Your HP rose by %d points." / "Your MP rose by %d points."
"The battle is over."                             s_The_battle_is_over_004e851c
"PickRandomMonster link %d, %d %d %d%% => %d monsters"   s_PickRandomMonster_link__d___grou_00502e84
"%s misses"                                       s__s_misses__004ee2ec
"PickRandomMonster didn't pick anything…"         s_PickRandomMonster_didn_t_pick_an_00502e40
```

## 11. What is still uncertain (flagged in place above)

* The exact packing of `scaleFactor / colorTable / xparent` inside monster record `+0x0E8` (the flags byte at `+0x124` **is** confirmed).
* The contents of the two 9-entry formation tables at `0x502A58/0x502A80` (side 0) and `0x502AA8/0x502AD0` (side 1) — never written in the decompiled code; only the position formula `x = 18 + col*36`, `y = (row+1)*32 + (col even ? 16 : 0) + 16` is known.
* The two float constants `_DAT_004cd548` / `_DAT_004cd578` in the map-encounter falloff.
* The exact numeric value of `_DAT_004d17a8` / `_DAT_004d1830` (the resistance curve's scale and offset) and `DAT_004e0ff0` (PvP damage scale).
* Per-file cell dimensions of individual `monsters/*.bmp` — the rule is "cell = biHeight, frames = biWidth/biHeight", so each sheet must be measured from its own header; several sheets may additionally be RLE-compressed, which the loader accepts.
* Whether the hero is drawn left or right of the monsters; the code only fixes the ordinal ordering and that everyone enters from the right edge.


## Sources

- `work/decomp/all.c`: Full Ghidra decompilation, 138k lines; every function starts with `// ==== <VA> <name> ====`
- `extracted/worlds/Evergreen/monsters.txt`: Monster table; 21-column header comments, the "Algorithm Scale %" row 0, and 200+ real monsters
- `extracted/worlds/Evergreen/groups.txt`: Monster groups: `id, count, m1..m9`; header documents distance-weighted selection and signed difficulty; row `0, 1` is the "spawn all" flag
- `extracted/worlds/Evergreen/quest.txt`: Quest script; SCENE 2 is the standard FIGHT scene, FIGHT/FIGHT2 opcode docs at lines 992-1053, scene style constants at 1255-1262
- `extracted/worlds/Evergreen/levels.txt`: Per-class/per-level table driving XP thresholds, max HP/MP and level names; header lines 1-93
- `extracted/worlds/Evergreen/items.txt`: arg16 attackPath.imageID.flags.weather.effect documented at lines 330-380 (LUNGE/JUMP/LEAP/STAB/DBL-STAB/TRAMPLE/HOP, ARROW/STONE/LOB/Swoop, art/attackNN.bmp)
- `extracted/scenes/`: Scene backgrounds; readme.txt states JPGs are 360x256
- `extracted/monsters/`: Monster skin sheets (joshNN.bmp etc.), 8bpp palettized horizontal strips
- `extracted/skins/`: Hero skin sheets, same strip format
