# Well of Souls — portable C port gap analysis (vs Souls.exe A96)

## Scope & method

Compared the original MFC game (`extracted/Souls.exe`, WoS A96, MSVC6/MFC42; decompile at
`work/decomp/all.c`) against the portable C port in `src/`, using the RE corpus as the statement of
what the original does:

- `REVERSE.md` — Port Progress table, Corrections, Next Tasks.
- `docs/re/boot_flow.md`, `maps.md`, `battle.md`, `script.md`, `art.md`, `formats_online.md`.
- `docs/architecture_port.md` — the port's own deviation list.
- `work/decomp/all.c` — consulted for FUN_ addresses cited in the docs (grep only, not read whole).
- `src/` — read the loaders, VMs, panels and platform boundary; grepped for feature keywords,
  TODO/FIXME markers and opcode spellings.

Method: for each subsystem I took the original behaviour from the RE source, then looked for the
port-side implementation (file + function) and the delta. Everything is cited. Items I could not
fully verify are marked **uncertain**. The port's deliberate deviations (documented in
`docs/architecture_port.md`) are listed as gaps with an explicit "intentional" note, because the
question asked is "what does the original have that the port does not".

Only this file was written; no code, docs or `labels.csv` were touched.

---

## Cross-cutting High-severity summary (ranked)

| # | Gap | Where |
|---|-----|-------|
| H1 | **Scene VM implements roughly half the QUEST opcode set.** ~25 documented opcodes fall through to `unknown_op()` and silently advance the PC: `GAME`, `MISSION`/`MISSIONS`, `HTML`, `CALL`/`RETURN`, `PUSH`/`POP`, `SHUFFLE`, `SET_LEN`/`SET_SUBSTR`, `STRSTR`, `NTH_TOKEN`, `F_ADD/F_SUB/F_MUL/F_DIV/F_MOD`, `AND/OR/XOR/NOT`, `GET/SET_SERVER_VAR`, `SET_LOWER` (0x4D), `EJECT`, `FACE`, `COLOR`, `MENU`, `0x43`, `0x32`/`0x33`/`0x34`. `PARTY` is a logged no-op. Several `IF` conditions are also missing (`H`, `J`/`JA`/`JQ`, `KB`/`KM`, `M0`/`M2`/`M4`, `P0`/`Pnn`, `GS`). | `src/game/scene.c:433-581` (`step`), `src/game/scene.c:324-353` (`atom`) |
| H2 | **Missions subsystem absent.** No `missions.ini` reader, no mission state, no mission picker. The `MISSION`/`MISSIONS` opcode is unimplemented, yet retail Evergreen exercises it (scenes 120, 2000). | `src/game/scene.c` (no case), `src/game/world.c:226-231` (no parse case) |
| H3 | **No death / respawn flow.** Losing a fight sets `hp = 0` and nothing else; there is no death screen and no solo auto-resurrect. The original resurrects the hero in solo mode (`FUN_00494fcd`, `DAT_00502a48 != 0`). | `src/game/battle.c:206-227` (`finish`), `src/game/front.c:13` (state enum has no death state) |
| H4 | **Netplay / SRNet.dll not implemented** (intentional: offline target). Whole online surface absent. | `docs/architecture_port.md` deviations; no `src/` net code |
| H5 | **Front-end state machine reduced from 13 states to 5.** Missing: `+STORY` scroller (state 4, `art/chapter.jpg`), death screen (state 8, `art/death.jpg`), web-browser view (state 11), credits, and four of the six main-menu entries (help file, website, online check, Golden-Soul thank-you). | `src/game/front.c:13`; `docs/re/boot_flow.md` §1 |

---

## 1. Platform / OS & input layer

**Original:** `InitInstance` (0x408C00) does `GetModuleFileName`→install root→`SetCurrentDirectory`,
creates `temp`/`temp\sceneCache`/`save`, `GetVersionEx`, `AfxSocketInit`, `Enable3dControls`,
`LoadStdProfileSettings`, `SetRegistryKey("Synthetic Reality")`, profile reads for sound prefs and
`currentThemeName`, then loads 10 `RT_CURSOR` cursors (IDs 0x8B,0x8C,0x91,0x92,0xA8,0xBE,0xC9,
0xD3,0xDF,0xEC) + `IDC_ARROW`. Registry/profile storage, WinHelp, MIDI via `winmm`, WAV via MCI.
(`docs/re/boot_flow.md` §0.)

**Port status: complete for the solo target.** `src/platform/platform.h` is the only host boundary;
`src/platform/sdl2/platform_sdl2.c`, `files_sdl2.c` (case-insensitive open + directory listing),
`audio_sdl2.c` provide video/input/time/file/WAV/MIDI. `src/platform/sdl2/main_sdl2.c` supplies
`--data`/`--save`.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Low | Custom cursors not implemented (SDL default cursor). Presentation-only. | original: `docs/re/boot_flow.md` §0 step 11; port: no cursor API in `src/platform/platform.h` |
| Low | Windows registry / `GetProfileInt` preferences (`bleeper`, `dunceMute`, `shoutMute`, `heartEavesdrop`, `languageMuteLevel`, `currentThemeName`, `TraceMask`, `Filters\allCaps`) have no portable equivalent and are not read. | original `docs/re/boot_flow.md` §0 step 9; port has no settings store |
| Low | `--data`/`--save`/replay flags are port-only CLI; the original had `argv[0]=="/setup"` → SRNet setup helper. Online-only. | `docs/re/boot_flow.md` §0 step 10 |

No functional gap found for offline video/input/files/audio plumbing.

---

## 2. Engine core (framebuffer, image/BMP/JPEG, INI, text/font, log, screen, replay)

**Original:** one art loader `FUN_00486690` sniffing BMP/JPEG into an 8bpp DIB
(`docs/re/art.md` §1); transparency = palette index of pixel (0, h-1) (`art.md` §1.4); every
character sheet is a horizontal filmstrip of `h×h` cells with `frames = width/height` (`art.md`
§1.5); button cells are 48×48 (`art.md` §5.2); scenes are 360×256 JPEGs; map JPEGs 4× stretched.

**Port status: complete.** `src/engine/fb.c`, `image.c` (BMP/JPEG via vendored `stb_image`),
`font.c`/`text.c` (embedded 8×8 font — documented deviation), `ini.c` (full section/key parser),
`ui.c`, `rng.c` (RNG), `log.c`, `screen.c`, `replay.c`. Filmstrip math is in `src/game/world.c`
(`sheet_load_*`, `sheet_draw*`).

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Low | Font: original uses Tempus Sans ITC (`tempsitc.ttf`, `AddFontResourceA`, `FUN_00408a6e`); port uses an 8×8 bitmap font. Documented deviation, presentation only. | `docs/re/art.md` sources; `docs/architecture_port.md` deviations |
| Low | Fixed 640×480 layout instead of client-rect-derived layout. Documented deviation. | `docs/architecture_port.md`; original `docs/re/boot_flow.md` §6 |
| Low | `.rsrc`-sourced dialog chrome / cursors / app icon are not reproduced. | `docs/re/boot_flow.md` §7 |

No gap found in image format coverage (BMP incl. RLE8 and JPEG both load through `stb_image`),
INI parsing, or the log/replay determinism contract.

---

## 3. Audio / soundfont / MIDI

**Original:** MIDI through `winmm`; per-map playlists from `music.ini` (`fight`, `victory`, `lost`,
`levelup`, `numMidi`, `midi1..N`), root section then `[common]`; scene `THEME` plays hard-coded
themes 0-19 plus `+THEMES` extras (looped WAV with `seconds=oneshot` pairs); `SOUND` one-shots;
`sfx\openDialog.wav`/`closeDialog.wav`; positional `walk.wav` pan/volume.
(`docs/re/script.md` §7, `docs/re/formats_online.md` §6.3.)

**Port status: complete for solo.** `src/game/audio.c` (`game_music`, world→root `MIDI/` lookup),
`src/game/scene.c` `theme()` (parses `+THEMES`, loops a WAV by computing `data*60/rate` frames),
`sound()` for `SFX/`; `src/platform/sdl2/audio_sdl2.c` mixes 8 WAV voices + TinySoundFont MIDI.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Low | `+THEMES` `"seconds=oneshot"` pairs are not parsed — a theme row with a MIDI file is played as music, a WAV is looped; the one-shot overlay list is ignored. | original `docs/re/formats_online.md` §6/§2.3; port `src/game/scene.c:278-317` (`theme`) |
| Low | Positional `walk.wav` pan/volume from the map offset is not reproduced. | original `docs/re/maps.md` §6; port `src/game/mapview.c` has no walk sound |
| Low | `THEMES`/`music.ini` "loop" flag and playlist advance: the port advances on track end but ignores an explicit `loop` flag. | original `docs/re/formats_online.md` §6.3; port `src/game/audio.c` |

No gap found for the music playlist lookup order or fight/victory/lost/levelup stings
(`src/game/battle.c` `battle_music`, `src/game/world.c` `world_music`).

---

## 4. World data & formats

**Original:** `quest.txt` + `#include` spliced inline, lexer with `;` truncation and
quote/whitespace rules (`FUN_00479594`/`FUN_0047a0e4`), 16 section parsers, then per-map
`.jpg`/`X4.jpg`/`.ter`/`.obl`/`.mon`/`objects.bmp|jpg`/`objects.obr`; per-world INIs
(`config.ini`, `gender.ini`, `music.ini`, `missions.ini`, `slots.ini`, `springy.ini`); a world CRC
pair compared against the update manifest.
(`docs/re/script.md` §1, §5-7; `docs/re/maps.md` §1-5.)

**Port status: substantially complete.**
`src/game/world.c`: `world_tokenize` (comma/whitespace lexer with quotes and comments),
`world_load` (quest.txt + `#include`, 16 section names, per-table parsers for MAPS, TERRAINS,
MONSTERS, GROUPS, LEVELS (+class directives: `DESCRIPTION`, `AUTO_MAX`, `START_ABILITY`,
`MAX_ABILITY`, `START_ELEMENT_PP/HAND_PP`, `MAX_*`, `START_LOCATION`, `START_ITEMS/SPELLS/TOKENS`,
`DEFAULT_SKIN`, `MAX_WALLET`, `NO_GIFTS`, `HIDDEN_CLASS`, `MAGIC_RATIO`, `HAND_RATIO`), ITEMS,
SPELLS, ELEMENTS, HANDS, TROPHIES), `map_load` (jpg, X4, `.ter`, `.obl` 256×800, `.mon` 1000×276,
`objects.bmp`→`objects.jpg`, `objects.obr`), filmstrip sheets, `music.ini`, `config.ini`
(`startingGP`, `maxUnspentPP`), `gender.ini`. All the format corrections in `REVERSE.md` are
reflected (terrain grid `(jpg+3)/4`, OBR accepts <1000 rows, missing `.ter` cleared, filmstrip
pitch, etc.).

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Med | `+TOKENS` table not parsed (`SEC_TOKENS` has no case) and the `TOKEN n,"text"` opcode is a no-op (`scene.c:460`). Token *bits* work (GIVE/`IF T`/`START_TOKENS`), but token descriptions and the diary (`%T<n>`) are absent. | original `docs/re/script.md` §4.1, `FUN_004814d2`/`FUN_00481654`; port `src/game/world.c:236-381` (no `case SEC_TOKENS`), `src/game/scene.c:460` |
| Med | `config.ini` only reads `startingGP` and `maxUnspentPP`. `spellSuccessPercent`, `pkHandPercent`, `pkMagicPercent`, `cookieProtection`, `maxPKAttackAdvantage`, `goldName`, `NO_GIFTS`-adjacent keys are ignored. `spellSuccessPercent` is a solo-relevant fizzle modifier. | original `docs/re/formats_online.md` §6.2; port `src/game/world.c:506-512` |
| Med | `missions.ini` is never read (see §7/H2). | original `docs/re/script.md` §7; no reader in `src/` |
| Low | `+EQUIP` slot-rename table ignored; slot labels are hardcoded `{"Head","Body","Feet","Guard","Ring","Charm","Hand"}`. | original `docs/re/formats_online.md` §2.5; port `src/game/panels.c:20` |
| Low | `+CREDITS` / `+STORY` sections are recognised (so they parse safely) but their content is discarded. | `src/game/world.c:227-231,408-409`; original `docs/re/boot_flow.md` §1 |
| Low | World CRC-1/CRC-2 check not implemented (no `world.ver` verification). Documented deviation. | `docs/re/boot_flow.md` §2; `docs/architecture_port.md` deviations |
| Low | `slots.ini`, `springy.ini`, `racers.txt`, `stocks.ini` not read — minigame/easter-egg data. | `docs/re/script.md` §7 |

---

## 5. Front end (title, menus, world select, New Soul / hero creation, save/load)

**Original:** 13-state machine `FUN_0041b891` (0 title, 1 main menu, 2 world-select title,
3 world list, 4 story scroller, 5 in-world/Well, 6/7 scenes, 8 death, 9/10 transitions, 0xB web);
per-mille hotspot menu; `Pick a Soul` custom `CWnd` (`FUN_004766ee`) with NEW SOUL / INCARNATE /
PURGE; New Soul dialog 138 (`FUN_0045fda9`/`FUN_00460a7e`) with class list, gender combo, name
validator (`FUN_00460805`) and PK opt-in; follow-up dialog 149; `.her` save = fixed 0x16CC record +
checksum at +0x16C8.
(`docs/re/boot_flow.md` §1-4.)

**Port status: partial.** `src/game/front.c` implements TITLE, MENU, WORLDS, WELL (Pick-a-Soul),
NEW_SOUL with class/gender/name + the validator (punctuation sign corrected per `REVERSE.md`) and
the Well button bar (Incarnate/Haunt/New/Restore/Map). `src/game/hero.c` does creation, level maths
and save/load.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Med | Story scroller (state 4, `+STORY` + `art/chapter.jpg`) absent. | original `docs/re/boot_flow.md` §1 (state 4); `src/game/front.c:13` |
| Med | Death screen (state 8, `art/death.jpg`) absent (see H3). | original `docs/re/boot_flow.md` §1; `src/game/front.c:13` |
| Med | Save/load is a port format (`.wsh`, WSH3) and cannot read retail `.her` files. Documented deviation, but it means no import of existing souls. | port `src/game/hero.c:5,142,172-196,242`; original `docs/re/boot_flow.md` §4 (`.her` is a plain 5836-byte record + checksum, **not** encrypted) |
| Low | Main menu has only "Play now" and "Depart this realm". Missing: "Read the attractive help file" (0x46D, `WinHelp`), "Visit synthetic-reality.com" (0x483, web view), "Check On Line for New Worlds" (0x498, online), the Golden-Soul thank-you string (0x46C), `tos.rtf` terms. | original `docs/re/boot_flow.md` §2; port `src/game/front.c:204-210` |
| Low | World-select "previous/next page" hotspots (0x472/0x473) and the `"--- or Create Your Own World ---"` (0x620) entry are absent (the port scrolls a 12-row list instead). | original `docs/re/boot_flow.md` §2; port `src/game/front.c:211-222` |
| Low | `Pick a Soul` "PURGE SOUL" (delete) button absent; no soul deletion path. | original `docs/re/boot_flow.md` §3a (button id 0x464, `FUN_00477461`); port `src/game/front.c` |
| Low | PK opt-in confirmation skipped (documented; always non-PK). | `docs/architecture_port.md` deviations |
| Low | Post-creation follow-up dialog 149 not reproduced (documented flow approximation). | original `docs/re/boot_flow.md` §3e; port `src/game/front.c` |
| Low | No "Place Yourself On Gaiea" prompt; a fresh hero starts above link 0 of map 0. Documented deviation. | `docs/architecture_port.md`; original `docs/re/boot_flow.md` §4 |

---

## 6. Map mode (walking, terrain, links, random encounters, panels)

**Original:** map units = low-res JPG pixels, 4× zoom, 256×256 walk window; `.ter` 8bpp BMP at
`(jpg+3)/4`; walkability `FUN_004631c6` (terrain 0 free, 9 never, others need a token or equipment
capability); links `FUN_004636d3`/`FUN_00463853` with kinds 0-4; random encounters
`FUN_0046260e` (per-tick Bernoulli `200*3/(bitlen(diff)+…)`/10000, 5 s cooldown) resolved by
`FUN_0049099b` (groups.txt) or `FUN_00464daf` (`.mon` proximity); idle auto-wander
`FUN_004610d9`; link sprites + names `FUN_004639eb`.
(`docs/re/maps.md` §1-9; `REVERSE.md` corrections.)

**Port status: complete (walking/terrain/links/encounters/minimap/objects).**
`src/game/mapview.c`: camera, click-to-walk, `grid_snap` `(v/4)*4+2`, `line_march`,
`path_build` (2000-node detour), `map_walkable` with terrain tokens + movement effects (fly/speed),
`nearest_link`, link activation by kind, `encounter_tick` with the corrected difficulty-0 on-link
suppression and the 0.5/0.25 tiers, `draw_minimap`, objects.obr sprite blit, map music playlists.
`src/game/world.c:583-625` loads the full per-map file set.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Low | Link display names are never drawn (original draws the `+0x0C0` name above the sprite when `hasBeenUsed` or map flag 32 is set). | original `docs/re/maps.md` §4 (`FUN_004639eb`); port `src/game/mapview.c:603-660` |
| Low | Idle auto-wander (`FUN_004610d9`) not implemented; the hero only moves on click. This also changes the effective encounter opportunity rate. | original `docs/re/maps.md` §6; port `src/game/mapview.c` |
| Low | Keyboard walking and menu shortcuts are port-only additions (documented). | `docs/architecture_port.md` |
| Low | Encounter roll is converted to per-60 Hz-step rather than per animation tick (documented). | `docs/architecture_port.md`; original `docs/re/maps.md` §8a |
| Low | Nearby-player duel scan (`FUN_0046260e` player branch) absent — online only. | original `docs/re/maps.md` §8a; port `src/game/mapview.c` |
| Low | Map `.mon` sprites and terrain overlay are debug-only in the original too, so their absence is not a gap. | original `docs/re/maps.md` §9 |

No gap found for terrain decoding, `.obl`/`.mon`/`.obr` loading, X4 handling, walkability rules or
the corrected encounter constants.

---

## 7. Scene VM (dialog, actors, conditions, tokens, cookies, timers, arithmetic, FIGHT, shops)

**Original:** opcode table `FUN_0047a6b0` (~70 opcodes) with `FUN_0047d577` as the interpreter;
cookies in a per-hero INI `[cookies]`; stock cookies (`num.*`, `str.*`, `g.*`) in `FUN_0047c6da`;
`%` substitutions in `FUN_00484b3e`; one line per call with suspended states for WAIT/ASK/FIGHT/
HTML/GAME.
(`docs/re/script.md` §2-4.)

**Port status: partial.** `src/game/scene.c` implements the actor/dialog/presentation core, cookies
(per-hero `.cookies` sidecar — documented deviation), stock cookies, `%` substitutions, TIMER/
COUNTDOWN, IF condition grammar with `+`/`-`/`|`, COMPARE/IF family, GOTO/GOTO EXIT/SCENE/LINK,
GIVE/TAKE family, FLAGS, LOCK, OFFER/OFFER2, FIGHT/FIGHT2, arithmetic ADD/SUB/MUL/DIV/MOD,
STRCMP, WEATHER/FX (logged unsupported), PARTY (no-op).

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| **High** | Unimplemented opcodes (fall through to `unknown_op`, which logs and advances the PC): `GAME`, `MISSION`/`MISSIONS`, `HTML`, `CALL`/`RETURN`, `PUSH`/`POP`, `SHUFFLE`, `SET_LEN`, `SET_SUBSTR`, `STRSTR`, `NTH_TOKEN`, `F_ADD/F_SUB/F_MUL/F_DIV/F_MOD`, `AND/OR/XOR/NOT`, `GET_SERVER_VAR`, `SET_SERVER_VAR`, `SET_LOWER` (0x4D), `EJECT`, `FACE`, `COLOR`, `MENU`, and opcodes 0x32/0x33/0x34/0x43. | original `docs/re/script.md` §2.2 (full table); port `src/game/scene.c:433-581` |
| **High** | Missing `IF` conditions: `H` (age hours), `J`/`JA`/`JQ` (missions), `KB`/`KM` (kill counts), `M0`/`M2`/`M4` (cheater/mod), `P0`/`Pnn` (PK), `GS` (golden soul). Present: `#`,`C`,`F`,`G`,`I`,`T`,`S`,`V`,`R`,`E`,`Q`,`Z`,`ALIVE`,`DEAD`,`WIN`,`LOSE`,`YES`,`NO`,`XP`. | original `docs/re/script.md` §3.1; port `src/game/scene.c:324-353` |
| Med | `%` substitutions incomplete: `%3` (random insult), `%4`/`%5`, `%E<n>` (equipped item name), `%K*` (kills), `%T<n>` (token text), `%Z<n>` (trophy name) not handled; `%L` emits the level number, not the level name; `%Cn`/`%In`/`%Mn`/`%Sn`/`%Rn` handled. | original `docs/re/script.md` §4.4; port `src/game/scene.c:208-256` (`expand`) |
| Med | `FIGHT *` semantics wrong: the star should mean "listed monsters **plus** the normal random encounter"; the port ignores `*` and runs a forced fight with only the listed ids. `FIGHT 0` (map monsters only) works by coincidence. | original `docs/re/script.md` §2.3; port `src/game/scene.c:528-534` |
| Med | Cookie persistence uses a `.cookies` sidecar rather than the hero INI `[cookies]`; no `secure*` hash/cheat-point machinery (explicitly droppable offline). Documented deviation. | `docs/architecture_port.md`; original `docs/re/script.md` §4.2 |
| Low | `PARTY` is a logged no-op and `EJECT` unimplemented (offline has no party). Documented deviation. | `src/game/scene.c:473-475` |
| Low | WEATHER/FX produce no visuals (logged `visual=unsupported`). Documented deviation. | `src/game/scene.c:469-471` |
| Low | `TOKEN` opcode no-op and `+TOKENS` unparsed (see §4). | `src/game/scene.c:460` |
| Low | Scene `COLOR` (palette table) and `FACE` unimplemented. | `src/game/scene.c` |
| Low | `@eventActorClick/Give/Attack/Spell` async re-entry labels are not dispatched (no actor-click events). Evergreen's camp lock (`@eventActorClick9`) will not fire. | original `docs/re/script.md` §2.6; port `src/game/scene.c` (no event dispatch) |

---

## 8. Battle (physical + spell combat, damage, rewards, XP curve, level-ups, training)

**Original:** real-time round state machine `FUN_00490e7c`, actor picker `FUN_0048f913`, to-hit
`FUN_004904eb`, damage roller `FUN_004a7794`, apply `FUN_004946b2`, win/loss `FUN_0048fd90`,
payout/level-up `FUN_0042bb5c` with participation shares and a trophy roll `FUN_0042b867`.
(`docs/re/battle.md` §3-9.)

**Port status: complete and faithful.** `src/game/battle.c`: 144-combatant array, round pacing,
AI target selection, monster pet-verb AI (`HEEL/BITE/ATTACK/HEAL/USE`), physical damage with the
corrected encrypted-stat getter, spell damage with resistance wheel + `1.1892^` training curve,
fizzle via magic ratio, bound weapon spells, HP/MP charge gauges with the "attack = small heal"
mechanic, monster scaling coefficients (1162/513/567/834, 197/212/231/189/175), spell/ailment
handling, victory/defeat stings. XP curve uses `pow(L*0.01, 2.5)*450000+10` and the corrected
denominators (`src/game/hero.c:261-263`, `src/game/battle.c:240-260`).

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Med | No victory trophy roll (`FUN_0042b867`). Trophies are parsed and readable via `IF Z`/cookie, but a won fight never drops one. | original `docs/re/battle.md` §9; port `src/game/battle.c:206-231` (`finish`), no trophy call |
| Med | Reward sharing uses `fight.actors[0].attacks` as an all-or-nothing gate rather than per-combatant participation points (`rec[0x11A]`); no `"That's a %d/%d share…"` split. Solo impact is small (one player). | original `docs/re/battle.md` §9; port `src/game/battle.c:206-231` |
| Low | Monster flee (`FUN_0048f816` cowardice + generic `(rand()+4)*10`) and the `never flee` flag 32 not implemented; monsters fight to the death. | original `docs/re/battle.md` §8; port `src/game/battle.c` |
| Low | Player flee always succeeds (documented deviation). | `docs/architecture_port.md` |
| Low | Escort/mercenary pull-in (`FUN_0048e16e`) and monster summon helpers not implemented. | original `docs/re/script.md` §2.3, `docs/re/battle.md` §3; port `src/game/battle.c` |
| Low | Attack-path/effect visuals replaced by element-coloured flashes; `attackNN.bmp`/`effectsNN.bmp` strips unused. Documented deviation. | `docs/architecture_port.md`; original `docs/re/battle.md` §6 |
| Low | PvP damage scale (`DAT_004e0ff0`) and PK percentages not modelled (online). | original `docs/re/battle.md` §7b |

---

## 9. Spells, items, equipment, shops, training

**Original:** spell learning debits PP and respects affinity/token/level
(`FUN_0044E3BD`); items by class 0/1/2/3/4/5/10-23/100-105/200/201 with use effects, spell binding
(arg6) and attack paths (arg16); `OFFER`/`OFFER2` shop filter (`items.txt findProbability != 0`,
class < 100, gp > 0, level in range; sell = gp/2); training screens debit the PP wallet with the
element-erosion rules (opposite −15 %, distance 3/5 −5 %, distance 2/6 −1 %, floor 20).
(`REVERSE.md` corrections; `docs/re/script.md` §6; `docs/re/formats_online.md` §6.7.)

**Port status: substantially complete.** `src/game/hero.c`: `hero_learn_spell`/
`hero_can_learn_spell`/`hero_spell_known`, `hero_gain_training`/`hero_train` with the erosion rules
and the 20 floor, `hero_pp_level` thresholds, equipment slots + attack/defense totals, item
requirements/limits, `hero_use_item` for potions, antidotes and ability seeds (100-105).
`src/game/panels.c`: ITEMS/SPELLS/EQUIP/STATS/TRAIN panels; `panel_open_shop` with the OFFER2
filter (`find_probability != 0 && klass < 100 && gp > 0 && level range`) and sell = gp/2.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Med | Item classes 2 (special), 3 (exit), 4 (TRAVEL/ticket), 5 (throwable), 200 (pet), 201 (HTML) are unsupported: `hero_use_item` returns 0. Evergreen gives `I99` ("ticket home") and travel items are a documented movement mechanism. | original `docs/re/formats_online.md` §6.7; port `src/game/hero.c:417-452` |
| Med | No trophy-bag subsystem: no bag geometry (`num.TrophyBagWidth/Height/…`), no bag UI, no `art/trophy*.bmp`, no trophy item semantics. Only the `IF Z`/cookie read path exists. | original `docs/re/script.md` §6.6; port `src/game/scene.c:350,390` only |
| Low | Pet system (class-200 items, `petPen.jpg`, pet level/ID cookies, taming) absent; only monster pet-verb AI in battle exists. | original `docs/re/boot_flow.md` §3b, `docs/re/formats_online.md` §6.7; port `src/game/battle.c:316` (comment only) |
| Low | Spell effect strips (`spells.bmp`, `effectsNN.bmp`) unused; spells draw as flashes. Documented deviation. | `docs/architecture_port.md` |
| Low | `+EQUIP` slot labels hardcoded (see §4). | `src/game/panels.c:20` |
| Low | `NO_GIFTS` / `MAX_WALLET` partially honoured; `goldName` display name ignored. | `docs/re/formats_online.md` §6.2/§6.6; port `src/game/hero.c:338-339` |

No gap found for the training erosion math, PP thresholds, OFFER2 filter, shop sell pricing, or
spell learning gating (these match the `REVERSE.md` corrections exactly).

---

## 10. Pathfinding / detour route

**Original:** `FUN_00461b11` grid snap = `(v/4)*4+2`; `FUN_00461b93` Bresenham line march with the
optional four-neighbour clearance; `FUN_00461dcc` bounded 2000-node detour builder using the 8-dir
tables at `0x4f21a0`/`0x4f21c0`; `FUN_0046206d` validates/repairs a click target.
(`docs/re/maps.md` §6; `REVERSE.md` corrections.)

**Port status: complete.** `src/game/mapview.c` `grid_snap`, `line_march` (with diagonal
clearance), `path_build` (`PATH_NODES_MAX 2000`, eight-direction tables `path_dx`/`path_dy`),
`start_path`/`path_arrive`/`move_step`.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Low | `REVERSE.md` Port Progress still says "done (no detour pathfinder)", but the detour builder **is** present (`path_build`) — a stale note, not a code gap. | `REVERSE.md` Port Progress vs `src/game/mapview.c:337-380` |

No functional gap found.

---

## 11. Netplay (SRNet.dll)

**Original:** imports `SRNET.dll` (Synthetic Reality networking); online worlds, PK, chat channels
(`Solo Channel` vs network types), the `IWebBrowser2` web view, the registration/Golden-Soul dialog
(0x64), `AfxSocketInit`, `lastIncarnationName` server vars, server-rule cookies (`num.rule`,
`maxPlayers`, `Ladder`, …), world CRC distribution, `MIX/`, `srnet`, `wosViewer`.
(`docs/re/boot_flow.md` §1/§2; `docs/re/script.md` §4.3; `docs/re/formats_online.md` §6.7.)

**Port status: missing (intentional).** No networking anywhere in `src/`; `docs/architecture_port.md`
states "Networking (SRNet.dll), online worlds, PK, the chat and the world CRC check are not
implemented — the target is offline solo play".

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| **High (intentional)** | Entire online layer absent: netplay, PK, chat, ladder, web view, registration dialog, server-variable cookies. | `docs/architecture_port.md` deviations; no `src/` net code |

Confirmed absent and confirmed out of scope. Nothing to fix for the offline target.

---

## 12. Editors / console / debug / minigames

**Original built-in authoring + extras surface:**

- **Minigames** via the `GAME` opcode (0x8c5fe): 1 Slobber Slots, 2 Monster Racer, 3 Search for Pi
  at Home, 4 Blackjack, 5 Pokegatchi Training, 6 Asteroids, 7 Stock Market, 8 Tetris; plus the
  `"Quadris"` tactics `CWnd` (400×430) and `slots.ini`/`racers.txt`/`stocks.ini` data.
  (`docs/re/script.md` §2.2 opcode 0x17; `docs/re/formats_online.md` §6.4; `docs/re/boot_flow.md`
  §3b dialog 8.)
- **Editors:** Link Editor (`FUN_0045d65f`/`FUN_0045cce5`), object placement
  (`FUN_0040f849`/`FUN_0040f8fd`), terrain brush (`FUN_004642c7`), monster placement
  (`FUN_00464b06`), `.obl`/`.mon` writers (`FUN_0046393f`/`FUN_00464421`), story editor (state 7),
  world editor (state 9/10), `/version` world signing.
  (`docs/re/maps.md` §9; `docs/re/boot_flow.md` §6.)
- **Debug/console:** `Debug` menu strings `"terrain"`/`"monsters"` toggling the overlays, `TraceMask`
  profile key, `/springy` easter egg (`springy.ini`), `/battle`/`/battle2`/`/battle3` simulators.
  (`docs/re/maps.md` §9; `docs/re/script.md` §7; `docs/re/formats_online.md` §6.7.)

**Port status: missing.** No editor, minigame, console or easter-egg code in `src/` (grep for
`slots|blackjack|asteroids|tetris|quadris|racer|pokegatchi|stocks` and `editor` found only
unrelated `third_party/` matches). `GAME`, `HTML`, `MENU` opcodes fall to `unknown_op`.

**Gaps**

| Sev | Gap | Evidence |
|-----|-----|----------|
| Med | All 8 minigames + the `GAME` opcode + `slots.ini` absent. Retail Evergreen scene 4 (the casino) uses `GAME 1`, so that scene loses its function. | original `docs/re/script.md` §2.2/§9; port `src/game/scene.c` (no `GAME` case) |
| Low | Authoring editors (link/object/terrain/monster/story/world) absent — expected for a play-only port, but a real part of the original surface. | original `docs/re/maps.md` §9, `docs/re/boot_flow.md` §6 |
| Low | Debug menu, terrain/monster overlays, `/springy`, battle simulators absent. | original `docs/re/maps.md` §9, `docs/re/formats_online.md` §6.7 |

---

## Coverage summary (auditable)

| Subsystem | Port verdict | Notes |
|-----------|--------------|-------|
| 1. Platform / OS & input | Complete (offline) | cursors + registry prefs only |
| 2. Engine core | Complete | bitmap font + fixed layout are documented deviations |
| 3. Audio / soundfont / MIDI | Complete | theme one-shot pairs, walk sound minor |
| 4. World data & formats | Substantially complete | `+TOKENS`, `missions.ini`, `+EQUIP`, several `config.ini` keys missing |
| 5. Front end | Partial | story/death/web screens, menu entries, `.her` import missing |
| 6. Map mode | Complete | link names, idle wander minor |
| 7. Scene VM | Partial | ~25 opcodes + 6 conditions missing (H1) |
| 8. Battle | Complete | trophy roll + participation shares missing |
| 9. Spells/items/equip/shops/training | Substantially complete | item classes 2/3/4/5/200/201, trophy bag missing |
| 10. Pathfinding / detour | Complete | no gap found |
| 11. Netplay (SRNet.dll) | Missing (intentional) | confirmed absent, out of offline scope |
| 12. Editors / console / minigames | Missing | `GAME` opcode used by Evergreen scene 4 |

### "No gap found" statements (what was checked)

- **Image/BMP/JPEG loading, filmstrip math, transparency key** — matches `docs/re/art.md` §1-2;
  `src/engine/image.c`, `src/game/world.c` `sheet_*`.
- **`.ter` decode, terrain walkability, movement-effect equipment** — matches `docs/re/maps.md`
  §3/§7; `src/game/world.c:600-646`, `src/game/mapview.c` `movement_effect`.
- **`.obl`/`.mon`/`.obr` loaders and the corrected record layouts** — matches `docs/re/maps.md`
  §4-5; `src/game/world.c:540-556`.
- **Encounter roll constants and the difficulty-0 on-link suppression** — matches `REVERSE.md`
  corrections; `src/game/mapview.c:484-540`.
- **Damage formulas, XP curve, level maths, training erosion, PP thresholds** — match the
  `REVERSE.md` corrections; `src/game/battle.c`, `src/game/hero.c`.
- **OFFER2 shop filter and sell pricing** — matches `REVERSE.md` correction; `src/game/panels.c:119`.
- **Detour pathfinder** — matches `docs/re/maps.md` §6; `src/game/mapview.c` `path_build`
  (`REVERSE.md`'s "no detour pathfinder" note is stale).

---

## Method notes / uncertainty

- Claims about the *original* are taken from the cited RE documents; where the docs themselves flag
  `[UNVERIFIED]` (e.g. the `.ter` bpp was later resolved in `maps.md` §3, attack-path grammar) I did
  not assert more than they do.
- The port's absence claims rest on grep + reading the dispatch/switch sites (`scene.c` `step`,
  `world.c` section cases, `hero.c` `hero_use_item`, `battle.c` `finish`) — these are exhaustive for
  the spellings checked. If an opcode is handled under an alias I did not grep, the finding would be
  optimistic, not wrong.
- Severities weigh: gameplay correctness for the retail Evergreen data (High), subsystem breadth the
  original has (Med), presentation/authoring/debug surface (Low).
