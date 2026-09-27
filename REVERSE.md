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

## Port Progress
| Subsystem | Status | Files |
|-----------|--------|-------|
| Platform (SDL2) | in progress | src/platform/ |
| Engine infra | in progress | src/engine/ |
