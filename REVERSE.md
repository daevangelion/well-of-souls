# Well of Souls: reverse engineering notes

## Ph1: Identification
| Item | Value |
|------|-------|
| Installer | `assets/WellOfSouls.exe`: Clickteam Install Creator stub (PE32, 2007-11-27) plus an overlay holding 875 files. Unpacked with cicdec 3.0.1 (`tools/extract_installer.sh`) |
| Game | `extracted/Souls.exe`: PE32 i386 GUI, MSVC6 + MFC42.DLL, linked 2008-12-06, ImageBase 0x400000, .text 0x401000..0x4c5dae |
| Imports | SRNET.dll (Synthetic Reality networking, bundled), DSOUND, WINMM, MFC42, MSVCRT, KERNEL32, USER32, GDI32, ADVAPI32, SHELL32, COMCTL32, OLEAUT32, WSOCK32 |
| Game version | WoS A96. World `Evergreen` v1.0137 (`worlds/Evergreen/world.ini`) |
| Decompilation | Ghidra 12.1.3 headless: `tools/ghidra/DumpDecomp.java` writes `work/decomp/all.c` (3992 functions, 0 failures) |

## Modules
| Module | Role |
|--------|------|
| Souls.exe | game (primary; all addresses in labels.csv are Souls.exe VAs) |
| SRNet.dll | networking. Not needed for offline play |

## Subsystem references
The detailed findings for each subsystem are in `docs/re/*.md`:
| File | Subsystem |
|------|-----------|
| docs/re/boot_flow.md | startup, menus, hero creation and save format, window layout |
| docs/re/maps.md | .ter/.obl/.mon/maps.txt, movement, encounters |
| docs/re/battle.md | fight scenes, monsters.txt, damage, rewards |
| docs/re/script.md | quest.txt language and table files |
| docs/re/art.md | sprite, UI and monster art formats |
| docs/re/formats_online.md | format notes gathered from online and local documentation |

## Next Tasks
### RE Investigation
- [ ] Confirm map encounter rate constants against the decomp
- [ ] Confirm damage formula constants against the decomp

### Port Fixes

## Corrections to docs/re/*.md (verified against the decomp)
- maps.md 8a: all.c:70909-70911 suppresses the difficulty-0 `.mon` encounters when the hero is ON the nearest link (`onLink == nearest`), not when it is off it. (MapView)
- maps.md 8c: the proximity tiers use the .rdata doubles at VA 0x4cd548 = 0.5 and VA 0x4cd578 = 0.25 (file offsets 0xcc748/0xcc778), i.e. radius/2 and radius/4. (MapView)
- battle.md: FUN_0049b70f is an encrypted-stat getter, not rand(). Assembly 0x4805b8..0x480604 maps +660 defense, +698 offense and +628 level. Physical power = ((level+100)*(str+65)*(offense*B/100+5))/6500; the denominator is (targetLevel+40)*(targetSta+200); the second scale is 200/(targetDefense+200). Monster per-action output scales by min(elapsed/480 frames, 1). The XP base uses HP*40/100, stamina+300, offense+200, defense+300 and level+70; the reward XP is not randomized. The HP algorithm multiplier is 1162/100 (11.62), not 1.162. (Battle)
- XP curve: the Ghidra output drops a multiplier. At 0x4846ee the code calls pow, and at 0x4846f3 it does fmull with the double at 0x4d0228 = 450000; 0x4d0220 = 0.01. Step = min(step*150/100, trunc(pow(L*0.01, 2.5)*450000)+10). Cumulative XP: level 1 = 0, level 2 = 14, level 3 = 35. (FrontHero)
- boot_flow.md 3d: the punctuation test at 0x460805 rejects names where letters+4 < non-letters. The prose states the comparison reversed. (FrontHero)
- art.md 1.4: the transparency key is DIB row h-1 of the bottom-up bitmap, which is the top-left pixel (`indices[0]`) of our top-down Image. Retail check: skins/josh2.bmp and Adventurer.bmp have top-left 0x008080 and bottom-left 0xffffff (the separator). (WorldData, ReArt)
- art.md 4: `Link.background[80]` is the scene image name. Link object rects come from OBJECTS.OBR entry `object_id`, which has explicit rects at +32/+36/+40/+44; the objects are not square cells. (ReArt)
- Derived monster stats (level-0 columns): the HP/MP/offense/defense coefficients are 1162/513/567/834, each /100. The STR/STA/WIS/AGI/DEX coefficients are 197/212/231/189/175, each /100. The record-0 percentage is applied afterwards. None of these are randomized. Damage = base * 8000/((targetLevel+40)*(targetSta+200)) * 200/(targetDefense+200), followed by variance/critical. (Battle)
- objects.obr holds UP TO 1000 x 48 B. Evergreen's is 12288 B (256 rows), and FUN_00463630 accepts the shorter table. A missing .ter is allowed: FUN_0041e421 creates a cleared terrain DIB before the optional load (Springwell has no .ter). Skins need not be an exact multiple of their height: Adventurer is 577x96 (6 cells + a separator column). monsters.txt arg20 is an AI string (FUN_004809a3), not a spell list. (WorldData)
- Hero physical charge/proficiency (0x48b1ad, 0x4a7ba6, FUN_00424dd2): effectivePP = (gauge+15)*handPP/30, and training = 10000*effectivePP/(effectivePP+5000). FUN_00424dd2 is not a clamp; it computes scale*(1-1/(PP*0.0002+1)). When training > 5000, power += trunc(power*pow(1.1892, training*0.001-5)). Each physical action earns 20 hand PP. The gauge does NOT heal; battle.md 4 reads it wrong. (Battle)

## Port Progress
| Subsystem | Status | Files |
|-----------|--------|-------|
| Platform (SDL2): video, input, case-insensitive files, WAV mixer, optional MIDI | done | src/platform/ |
| Engine: framebuffer, BMP/JPEG, 8x8 font, UI, INI, replay/log, RNG, screens | done | src/engine/ |
| World data: quest.txt + #include, tables, scene index, map binaries, filmstrip sheets | done | src/game/world.c |
| Front end: title, menu, world select, Well, New Soul, save/load (.wsh) | done | src/game/front.c, hero.c |
| Map mode: walking, terrain, links, random encounters, panels | done (no detour pathfinder) | src/game/mapview.c |
| Scene VM: dialog, actors, conditions, tokens, cookies, FIGHT | core opcodes done; shops display-only | src/game/scene.c |
| Battle: physical combat, rewards, level-ups | done (no spells) | src/game/battle.c |
| Spells, items/equipment screens, shops, training, music per map | not started | |
