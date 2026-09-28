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
- maps.md 6: grid snap FUN_00461b11 (all.c:70251) is (v/4 truncated)*4+2, i.e. the containing cell's centre, not ((v+3)>>2)*4+2. Pathfinder direction tables: dx[0..7] = {0,1,1,1,0,-1,-1,-1}, dy = {1,1,0,-1,-1,-1,0,1}. (MapView)
- OFFER2 item filter (all.c:93027-93032): items.txt findProbability != 0 (record +0x2d*4, loader all.c:96158), class < 100, gp > 0, used, and level within the inclusive min..max. The args are `min,max[,class]` triples; sell price = gp/2. The docs call the first test 'inUse'; it is the find probability. (Panels)
- Training screens (found with objdump; Ghidra missed them): hand click 0x419107 and element click 0x425c30 debit the PP wallet, then call 0x419306/0x4258b8. The default amount is 200 (0x425bfb); an alternate dynamic amount comes from 0x425b88. Right-click untrains, never below START_*, and refunds half. Training an element erodes the opposite element by 15%, the neighbours at wheel distance 3/5 by 5% and those at distance 2/6 by 1%, with affinities floored at 20. spells.txt ppCost 0 means a computed cost, not auto-known; a spell must be Learned. (FrontHero)
- Loader and asm checks (WorldData): the items.txt clamp to 20 applies to arg12 abilityPoints, not arg13 findProbability (the retail Health Potion has find=80). Trophy count minima are 1, not 2. Spell maxFx caps at 1023, not 100. The class max PP arrays default to 5,000,000. Spells with raw 0 PP/MP/damage are completed by the integer asm at 0x480274 (145/150/130% steps). Measured art: items.bmp is 4096x64 with a 64 px pitch (not 48); effects00 is 256x160; attack00 is 384x480.
- Timing (Core): the original has no 60 Hz tick. CWinApp::Run at 0x40a8d9 is a PeekMessage pump that handles input, then WM_TIMER, then idle/paint; idle runs only when the queue is empty, gated at 20 ms (FUN_0040a7c7: `GetTickCount() - last > 19`). GetTickCount is the only rule tick source: the import table has no timeGetTime. QueryPerformanceCounter (0x42895c) and time() are only used for the net graph and anti-tamper. See docs/re/timing.md.
- RNG (RngCensus): there are 180 rand() and 4 srand() sites. FUN_00443929 saves and restores the seed (`r = rand(); srand(p); ...; srand(r)`). FUN_0046260E (the encounter roll) consumes 0, 1 or 2 rands per tick: rng_calls.md says 2-4, and MapView corrected it from the decomp. FUN_0048E810 (monster spawn) consumes 2-771. FUN_0044A63A is a 1000 ms "freshening monsters" timer that runs in solo play. The %3 insult generator FUN_00485FF5 consumes 1-3 rands. See docs/re/rng_calls.md.
- maps.md 6 (MapView, then oracle probe): FUN_004620F3 builds a waypoint path only when user option id 7 "Enable automatic Way Point calculations" (DAT_006840D0[7], read by FUN_00467312(7)) is on AND map flag 0x80000 is clear. The option table is initialised .data at 0x4F2A58: 32 records of {id, default, label}, with the count 32 at 0x4F2BD8. Id 7 defaults to 1, so retail pathfinds around obstacles with the detour builder FUN_00461DCC. The live values for ids 0..32 are 0,1,1,1,1,1,0,1,0,0,1,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1,0,0,1,0,1,1,1. Id 9 has no record and stays 0 (work/oracle_probe_options.log).
- script.md 2.2 (MiniGames): the keyword table at VA 0x4FAD50 (85 records {opcode, name}, count at 0x4FAFF8) gives GAME=0x11, TOKEN=0x17, MENU=0x2B, TIMER=0x43, STRLWR=0x4D, MISSION(S)=0x4A, SHUFFLE=0x4C, NTH_TOKEN=0x4E; see agent MiniGames' table. GAME n does not block: it only arms button-bar slot 6 (FUN_00478E04, command 0x493, x 283-330, y 8-55 at 640x480), and GAME 0 closes the mini-game. Monster Racer rolls 4 + Binomial(4,0.2) rands per paint (FUN_00405FD1), driven by the 20 Hz idle; the 500-gold branch is unreachable.
- boot_flow.md 4 (FrontHero): `.her` byte offsets:
  - 0x60 class, 0x64 level;
  - 0x68 XP with -XP mirrored at 0x6C8, 0x6C gold with -gold at 0x974;
  - 0x70/0x74 HP, 0x78/0x7C MP;
  - 0x80 defence and 0x84 attack (not level/level-name);
  - 0x90 map, 0x67C link, 0x680-0x690 the five abilities;
  - 0x694 magic ratio, 0x69C PP wallet with -PP at 0x760, 0x6A0 preferred hand, 0x6C4 world CRC-1;
  - 0x6CC element_pp[8], 0x6FC hand_pp[8];
  - 0x734 seconds played, 0x738 the selected element per hand (33 ints; only 0-8 are understood);
  - 0x76C token bitmap of 4096 bits;
  - 0x9E0 hand ratio, 0xA4C save counter, 0xA78 time() of the last kill, 0xAA0 gender;
  - 0xCE0 trophy bag (128 words, `(count<<8 | id<<16) ^ 0x1D43E217`), 0xEE0 bag geometry, 0xEE8/0xEEC pet ids, 0xEF0 lifetime PP, 0xEFC energy;
  - 0x16C8 checksum;
  - inventory: one count byte per id at 0x272 for ids below 0x400, plus a 0x200-byte presence bitmap at 0xAC8;
  - learned spells: 768 bits at 0x210.

  FUN_004181A2 clears only 0x0C, 0x58, 0x5C, 0xCC, 0x6C0 and 0x978 on load. It requires the XP and PP mirror pairs to sum to zero; they do not have to be zero. Checksum FUN_00416ABB: mode 1 is `h ^= b << (i % 0x18)` with seed 0x379ADE; mode 0 shifts by `i & 1`, and only for non-zero bytes. Per-hero cookies and kill tallies live in the INI `savedHeroes/<Name>` (no extension) under [cookies], [monsters killed], [killed by monster] and [monsters seen]. Ailments are not saved: they live in the combatant record at +0x398.
- boot_flow.md 2 (FrontHero): the FUN_004056E7 arguments are (layer, flags, tLenMs, text, font, colour, fontSize, x_from, y_from, fontSize_to, x_to, y_to, hwnd, msg, lparam); argument 10 is a second font size. Hotspot font size = min(client_w*80/100, client_h) * perMille / 1000 (0x405194).
- items (Panels): FUN_004A6353 reduces a positive potion heal to 1 only on NO_HEAL maps (flag 0x1000) or when the opponent is poisoned, not when the hero is poisoned. A class-200 item's arg3 is a monster id. The trophy bag is clamped to 1..16 per side, 128 cells maximum. The item loader's arg5 floor is 1, not 2. Dialog ids: equipment 179, items 200, shop 212, trophy bag 231, stats 149, status 148.
- world (WorldData): the dotted arg12 overrides in spells.txt fill +0x164 then +0x168, which is the reverse of the documented order (0x4800F2/0x480108). Spell 0 is never marked used. `;` starts a comment only as the first non-blank character of a line. `#include ` needs the trailing space. A row holds at most 32 tokens. The spell post-pass draws one rand per used spell (0x4803E9); that is the only rand while loading.
- timing.md / rng_calls.md (Core): there are 8 SetTimer sites. Ghidra merged the functions at 0x428803 and 0x4a1e18. Every timer belongs to a dialog; no game rule runs on a timer. The world runs on the 20 ms idle gate FUN_0040A7C7, which calls FUN_0042895C. That function draws one discarded rand() on EVERY call, before its 25 ms sub-gate, and it is on the solo path, not SRNet-only. The 25 ms gate then calls FUN_0041BDB4, the whole game world. Boot seeding (FUN_004269AF): srand(time), rand(), srand(time). The front end has THREE states before the Well: menu, the "Where Do You Want To Play Today?" splash, and Choose Your World.
- battle.md (Battle): the r1/r2 values in the cowardice flee are the two combatants' levels (read through FUN_0049B70F at rec+0x628), not rands. FUN_00480875 draws no rand. The regen gauge is `min(100,(now-start)*(attr/2+100)/10000)`, with no +25 term, recomputed on every paint. The payout calls FUN_0042B867(1+(rand()&1), rand(), rand()&0x400). FUN_00494FCD prints the kill, increments hero+0x724 and applies the XP rollback only when DAT_00502A48 is set. An ordinary offline encounter can't be fled: DAT_00502B14 is 0, and the only escape is the 30% interrupt of an attack that is already animating (FUN_00436C9D). Each monster spawn draws 2 rands and each ally 3 (FUN_00491E45). _DAT_004cd548 = 0.5, _DAT_004cd578 = 0.25.
- script.md (MissionsHtml): MISSION(S) is opcode 0x4A. It formats its args as %04X, stashes the list at DAT_004F91E0 and arms button slot 6 with buttonMission.bmp (message 0x53C); FUN_00484E72 is GIVE/TAKE. Mission progress is stored in `savedHeroes/<name>.mis`, section `<job>`, key Status (0-3). JF means status==3, JA status 1 or 2, JQ not completed and Qualify holds. RewardLevel is atof then ftol: the level, in the hero's own class. HTML (0x29) still advances the PC; the suspension is the scene state. Option 22 "Show HTML pages in scenes" gates HTML. Button slots (FUN_004787B2): 48x48 at y 8..56, stride 51, slot N at x = clientWidth-51-51*N; slot 6 is shared by SHOP, MISSION and GAME.
- minigames (MiniGames): FUN_0048C5FE is the MENU (0x2B) popup, not the GAME dispatcher. The Blackjack shuffle draws 52000 rands. Stock Market draws none. Games 3, 5, 6 and 8 never pay out.
- battle.md (Battle, second pass): combatant state 0x2F means "has not acted this round". FUN_00490E7C case 5 sets every combatant not in state 0x94 back to 0x5F. The monster idle block also rolls `rand()%100 > 33` to choose between attacking and closing in. FUN_0048E16E is the record clear used when a monster walks off (x < -10); the escort pull-in is FUN_0048EBE3, gated by rand()%1000 < 250 and monsters.txt arg3 flag 4. FUN_0048B1AD queues the player's action and sets state 0x2F, which is how the hero takes a turn. FUN_00436C9D sets 0x2F before the flee roll, so a round gives exactly one 30% escape chance. Monster gold on a kill = rand()%(gold+1) + gold/2 (FUN_004946B2). FUN_0042B867 walks the ITEM table (0x30C stride) and prints "You found %s's %s". The offline attack rating rec[0x390] is always 0: its only writer is net opcode 0x53. The default throw scatter radius is 16 (FUN_004A4D70).
- battle.md 9 and the effects (Battle, via tools/ghidra/query.sh): the trophies.txt roll FUN_0046FCC4/FUN_0046FBF1 has exactly one caller, FUN_00494FCD at 0x49526D, the monster-death handler. It fires per monster kill when the hero's participation share is over 10%, AFTER the gold rand. The effect table is initialised .data at 0x5078A0: stride 0x150, 25 rows {pad, id, ..., five counters at +0x14}. No code writes it. Ghidra's `+= 0x54` walk and the 0x50B504 bound are artefacts. Counter 0 (which blocks flee, the fear roll and wandering) is set for effect ids -4, -7 and -11. Counter 1 (magic only) is set for -4, -6, -7 and -11.
- Encrypted ints (orchestrator, xd://ghidra): FUN_0049B70F reads an anti-cheat value, verifying three scaled doubles (factors 2.1459/1.4142/0.0123 at 0x4D0D38/40/48, read from the image with pefile), and consumes no rand. Set FUN_0049B71B, clear FUN_0049B734/0049B75D and add FUN_0049B73F each call FUN_0049B6C7, which writes the three doubles and then draws 4 rand() keys; nothing ever checks the keys. On a mismatch the reader sets DAT_004E709C, which is also the guard on the 20 ms idle gate. Of the 1432 rands the original draws while booting to the title, 1408 are 352 of these objects being constructed (FUN_00401125, via array constructors). The rest are 13 idle ticks (0x428996), 4 from FUN_00456B87 (a timed schedule: `(rand() % (n*2)) * 1000` ms), 4 from FUN_0042B4E0, 1 at 0x48E1CF (FUN_0048E19A) and 1 at 0x426B27. The original's idle rate depends on how often its message queue is empty: about one pass per 180 ms at the title under the oracle, not 50 Hz.

## Port Progress
| Subsystem | Status | Files |
|-----------|--------|-------|
| Platform (SDL2): video, input, case-insensitive files, WAV mixer, optional MIDI | done | src/platform/ |
| Engine: framebuffer, BMP/JPEG, 8x8 font, UI, INI, replay/log, RNG, screens | done | src/engine/ |
| World data: quest.txt + #include, tables, scene index, map binaries, filmstrip sheets | done | src/game/world.c |
| Front end: title, menu, world select, Well, New Soul, save/load (.wsh) | done | src/game/front.c, hero.c |
| Map mode: walking, terrain, links, random encounters, panels | done (no detour pathfinder) | src/game/mapview.c |
| Scene VM: dialog, actors, conditions, tokens, cookies, timers, arithmetic, FIGHT, shops | done (WEATHER/FX visuals unsupported) | src/game/scene.c |
| Battle: physical + spell combat, rewards, level-ups, training gain | done | src/game/battle.c |
| Spells (hero, monster AI, weapon binding), items/equipment, shops, training, music.ini playlists, detour pathfinder | done (spell visuals are flashes) | battle.c, hero.c, panels.c, scene.c, audio.c, mapview.c |
