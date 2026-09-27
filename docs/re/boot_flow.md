# Boot flow, front end, hero save (Souls.exe)

## Summary

Mapped the complete SOLO/offline startup + front-end flow of Souls.exe (WoS A96): CRT entry → MFC CWinApp::InitInstance at 0x408C00 (doc template CSoulsDoc/CMainFrame/CSoulsView) → a 12-state CView state machine FUN_0041b891 with `DAT_004df8a4` as the state (0 title, 1 main menu, 2 world list, 3 "Choose Your World", 4 story, 5 in-world/Well, 6/7 scenes, 8 death, 0xB web), the "clickable-text hotspot" menu system (array at 0x5339F8, stride 0xBC, 100 slots) used for every front-end screen, the Pick-a-Soul window (custom CWnd, not a dialog resource) that lists `Save\<World>\savedHeroes\*.her`, the New-Soul dialog (resource 138 / 0x8A) with class list + gender combo + name edit and its validators, the exact .her file format (fixed 0x16CC-byte record, checksum at +0x16C8, no magic/version header), the save paths, the button-bar layout (art\button*.bmp, 10 slots of 48x48 at 51 px pitch, right-aligned at the top-right), and the whole view layout (256 px right column, 64 px bottom chat strip, 256x256 hero panel, 2-row splitter). Also established that a fresh Evergreen hero starts on MAP 0 ("Evergreen") at link 0, driven by levels.txt `START_LOCATION` (absent in Evergreen → 0,0,0), NOT by quest.txt SCENE 0.

## Architecture

The port should reproduce four layers, in this order.

(1) BOOT (InitInstance, 0x408C00): GetModuleFileName → strip to DAT_005384D0 (install root) → SetCurrentDirectory(root) → mkdir root\temp, root\temp\sceneCache, root\save → GetVersionEx → WSAStartup(AfxSocketInit) → Enable3dControls/LoadStdProfileSettings → AddDocTemplate(CSoulsDoc/CMainFrame/CSoulsView) → SetRegistryKey("Synthetic Reality") → GetProfile* for sound prefs and currentThemeName → ParseCommandLine/ProcessShellCommand (creates the MFC frame) → LoadCursor(RT_CURSOR 0x8B,0x8C,0x91,0x92,0xA8,0xBE,0xC9,0xD3,0xDF,0xEC). ExitInstance is FUN_00409368 → WinHelp + ExitInstance.

(2) FRONT-END STATE MACHINE (CSoulsView, 0x41B891): one function FUN_0041b891(view, state) sets everything for a state; DAT_004df8a4 holds it; the timer tick FUN_0041bdb4(view) drives each state. States 0–4 draw a background JPEG (art\title.jpg, beg.jpg, where.jpg, chapter.jpg, death.jpg) and register animated text hotspots; states 5–7 show the game UI. A single generic "hotspot" system (0x405000 register, 0x405153 draw, 0x405765 hit-test) powers every menu, with coordinates in per-mille of the client rect so the UI scales.

(3) WORLD/DOC: FUN_00479… (0x479A02) loads `<root>\worlds\<World>\quest.txt` + all #includes, then maps.txt, monsters.txt, items.txt, spells.txt, levels.txt, trophies.txt, groups.txt, then per-map .obl/.mon/.ter, slots.ini, config.ini, gender.ini, missions.ini. CRC-1 is accumulated over the bytes read, CRC-2 over the parsed tables; both are compared against the values in the world's `…/world.ver`/update manifest. FUN_0041E421(world, frame, mapIndex, loadNew) is the map loader: it reloads quest.txt, reads ~12 config.ini keys, loads objects.obr, `<map>.obl`, `<map>.mon`, `<map>.ter`, builds the 256 KB BIG_MAP, and sets the scroll centre (DAT_00539388/84 = width/2, height/2).

(4) HERO: a fixed 0x16CC-byte record in a 100-slot table (DAT_0067FBF8, stride 0x16CC). Saved verbatim to `Save\<World>\savedHeroes\<Name>.her` with a checksum at +0x16C8. FUN_00420240 is "incarnate": save, bump counters, refresh the HUD, send 0x46A(link,map) to the frame to change map, tick the quest engine, refresh the view, and finally prompt "Place Yourself On Gaiea" if the class had no START_LOCATION.

(5) VIEW LAYOUT (FUN_0041B476 + FUN_0041B2DE): everything is derived from the client rect — 64 px bottom chat strip, 20 px right margin, 256 px button column, 256x256 hero/equipment panel at top-right, a 2-row CSplitterWnd between the map view and the chat log (row 0 = (width-296)*256/360, min 280 px; row 1 = the remainder, min 12 px). Button bar slots are 48x48 at 51 px pitch, laid out right-to-left from (client.right-48, client.top+8) by FUN_004787B2; slots 0–4 fit exactly in the 256 px column.

# WoS A96 — SOLO / offline startup & front-end flow

All VAs are Ghidra VAs in the Souls.exe image (ImageBase 0x400000). Every claim below is cited by VA/line in `work/decomp/all.c` or by file:line in `extracted/`. `[UNVERIFIED]` marks inference.

---

## 0. Entry chain

| Step | VA | What happens |
|---|---|---|
| `entry` (CRT `mainCRTStartup`) | 0x004C5AF0 | `__set_app_type(2)` (GUI), `initterm(&DAT_004dc000,&DAT_004dc0d0)` (static ctors), then `_WinMain_16` 0x004C5D40 → `AfxWinMain` |
| `CWinApp::InitInstance` | ~0x00408C00 (body ends `LAB_00408c47`) | see below |
| `CWinApp::ExitInstance` override | 0x00409368 | `FUN_004670b4(); WinHelpA(0,"wellofsouls.hlp",2,0); CWinApp::ExitInstance` |

### `InitInstance` (0x408C00), in order
1. `time(&DAT_0053866c)`; `_DAT_004dd0f0 = GetTickCount()` (session start tick, used later for the 5-minute autosave at 0x21290: `if (DVar4 - _DAT_004dd0f0 < 0x493e1) …` = 300000 ms).
2. `GetModuleFileNameA(0, &DAT_005384d0, 0x104)`; `strrchr('\\')` truncated → **`DAT_005384d0` = install root** (no trailing `\`). Then `SetCurrentDirectoryA(&DAT_005384d0)` — *all* later paths are relative to CWD.
3. `CreateDirectoryA` for `"%s\temp"`, `"%s\temp\sceneCache"`, `"%s\save"` (0x408BE0–0x408C10).
4. `GetVersionExA(&DAT_00538438)`; `_DAT_004dd0bc = (dwPlatformId==2)`; `_DAT_004dd0c0 = (dwMajorVersion>5)` → Win9x/2K/XP branches.
5. `AfxSocketInit`; on failure `AfxMessageBox(0x68,...)`.
6. `AfxEnableControlContainer`, `CWinApp::Enable3dControls`, `LoadStdProfileSettings(0)`.
7. `CSingleDocTemplate(0x80, CSoulsDoc, CMainFrame, CSoulsView)` → `AddDocTemplate`; `SetRegistryKey("Synthetic Reality")`.
8. `FUN_00466ef3()` (preferences dialogs / theme).
9. Profile reads (`GetProfileStringA`/`GetProfileIntA`, section `Preferences`/`Debug`): `bleeper`, `dunceMute`, `shoutMute`, `heartEavesdrop`, `myCommChannel`, `languageMuteLevel`, `currentThemeName` → `DAT_004e18d0`, `TraceMask` → `DAT_004e70d0`, `Filters\allCaps` → `DAT_004e7094`.
10. `CCommandLineInfo`; `ParseCommandLine`; `ProcessShellCommand` → **creates frame + view**. Special case: argv[0] == `"/setup"` runs `SRNet` setup helper (0x408C60).
11. After success it loads 10 cursors from RT_CURSOR: IDs 0x8B,0x8C,0x91,0x92,0xA8,0xBE,0xC9,0xD3,0xDF,0xEC into `DAT_004dd0c4..0x4dd0ec`, plus IDC_ARROW 0x7F00. Then returns 1.

The doc template is the only window system used; the frame is a plain MFC `CFrameWnd` whose size comes from the `IDR_MAINFRAME` resource in `.rsrc` (**I could not parse `.rsrc` here** — the `read` PE backend rejected the path and I have no shell, so the exact default frame rect is not recoverable from the decomp. See §6 for the client-rect-derived layout, which is what actually matters).

---

## 1. The front-end state machine — `FUN_0041b891(view, state)`

`CSoulsView` is global `DAT_004e483c`; the frame is `DAT_004e4840`. State is global **`DAT_004df8a4`** (0 = title, -1 = "no state"). Per-frame tick: `FUN_0041bdb4(view)` (0x41BDB4).

| state | art loaded (`FUN_0048a316(view, view+0x34A8, "art\X.jpg")`) | drawn by | notes |
|---|---|---|---|
| 0 | `art\title.jpg` | `FUN_0041d01e` (0x41D01E) | "Well"/"Souls"/copyright/synthetic-reality.com, all as sliding hotspots. `FUN_0041bdb4` case 0: `if (13000 < GetTickCount()-view[0x13F4]) FUN_0041b891(1)` — **auto-advances to the main menu after 13 s**; any mouse-down also advances (`FUN_0041c1cd` case 0 → `FUN_0041b891(1)`). Plays `intro.mid`. |
| 1 | `art\beg.jpg` | `FUN_0041d155` (0x41D155) | the **main menu**; `MainMenu.wav` |
| 2 | `art\where.jpg` | `FUN_0041d31f` (0x41D31F) → title text only; the list itself is painted by `FUN_0041d3cc` | **the world-selection screen** |
| 3 | `art\chapter.jpg` | `FUN_0041d717` (0x41D717) | "Choose Your World" / "World In Progress" + full world list |
| 4 | (none) | — | `STORY` scroller; `view[0x1388] = GetTickCount()`, `DAT_004df8a8 = 1`; on end → "Story Over" and `PostMessage(frame, 0x478)` |
| 5 | — | — | in-world; `FUN_004789d5(0)` = **Well/camp button bar** |
| 6 | — | — | scene entered by link: `FUN_00436ffc(scene,&name,1)` + `FUN_00456d2f(theme)`; `FUN_004789d5(1)` |
| 7 | — | — | scene entered via `FUN_0042052d` (join-scene); `FUN_004789d5(2)` |
| 8 | `art\death.jpg` | — | death screen; click → `FUN_0041b891(5)` |
| 9,10,11 | — | — | transitions; 11 = web browser view (`FUN_00421b81`) |

`FUN_0041bd7b()` (0x41BD7B) reports "in game": true only when state ∈ {5,7} **and** a soul exists (`DAT_004e17fc && hero[0xC] != 0`).

### Solo vs online
`DAT_004e6910 = _SRNGetNetworkType_0()` (0x42A…). `DAT_004e6910 == 0` ⇒ **solo channel** (`strcpy(&DAT_004e4dd0,"Solo Channel")`, 0x42AE). `FUN_0041d717` (state 3) shows the extra link `"--- or Create Your Own World ---"` (msg 0x620) **only when `DAT_004e6910 == 0`**; when it is 4 it shows `"--- or Check On Line for New Worlds ---"` (msg 0x620 as well). There is no separate "Play Solo" button — offline is the default/no-network path.

---

## 2. The clickable-text menu system (every front-end screen)

A global array of 100 entries at **`DAT_005339f8`**, stride **0xBC** (188 bytes). Layout, offsets relative to the entry base:

```
+0x00 int  state   0=free, 1=live, 2=animation finished, 3=dead/unclickable
+0x04 int  layer   (FUN_004055a2(layer) frees all entries of a layer)
+0x08 uint flags
+0x0C int  t_start (GetTickCount at registration)
+0x10 int  t_len   ms; when elapsed the entry flips to state 2 (or 3 if !flags&4)
+0x14 char* text   (heap, strdup)
+0x18 char  fontName[0x100]
+0x68 HWND  target window
+0x6C UINT  message
+0x70 LPARAM lparam
+0x74 RECT  last drawn rect (hit-test uses this)
+0x84 int   font size (per-mille)
+0x88 int   x_from     +0x98 int x_to      (lerped → +0xB0 current)
+0x8C int   y_from     +0x9C int y_to      (lerped → +0xB4 current)
+0xA4 CFont* cached font
+0xA8 int   last font size used
+0xAC int   current font size
+0xB8 int   colour
```

API: `FUN_00405000` register, `FUN_004050f8` free, `FUN_004055a2(layer)` free-by-layer, `FUN_004056e7(...)` = find-free-slot + register, `FUN_00405153` draw, `FUN_004054e8(idx,tick)` advance animation, `FUN_004057d3()` count active, `FUN_00405765(x,y)` hit-test → `PostMessage(hwnd, msg, slotIndex, lparam)`, `FUN_00405878(view,w,h)` stores the scale in `view+0x44` / `view+0x40`.

Coordinates are **per-mille of the client rect**: `x_px = current_x * view[0x44] / 1000`, `y_px = current_y * view[0x40] / 1000`; font px = `min(w*0.80, h) * fontSizePerMille / 1000` (0x405153). The menu items slide in from the right (x_from > x_to).

Wrapper signature (0x4056E7), 15 args:
`FUN_004056e7(layer, flags, tLenMs, text, fontName, colour, fontSize, xFrom, yFrom, xTo, yTo, yTo2, hwnd, msg, lparam)`

### Main menu, state 1 (`FUN_0041d155`, 0x41D155)
| text | msg | flags / tLen | x from→to | y |
|---|---|---|---|---|
| `"Check On Line for New Worlds"` | **0x498** | 0x2EE / 0x7D | 0x426→0x3E | 0x7D |
| `"Play now, it's free!"` (or `"Play now (Golden Soul)"` if `FUN_00409624()!=0`) | **0x46B** | 1000 | 0x464→0x7C | 0xFA |
| a random string from `PTR_s_Thank_you__Golden_Soul__004df8f8` (FUN_0041d121) | **0x46C** | 250 | 0x4A2 | 0x177 |
| `"Read the attractive help file"` | **0x46D** | 1500 | 0x4E0 | 0x5DC |
| `"Visit synthetic-reality.com"` | **0x483** | 2000 | 0x51E→0x136 | 0x271 |
| `"Depart this realm"` | **0x46E** | 2000 | 0x55C→0x174 | 0x2EE |

State 1 also plays `tos.rtf`; if the user refuses the terms it posts 0x46E then WM_CLOSE (0x41B891 case 1, 0x41098).

**0x46B is the "Play now" handler → `FUN_0041f699` (0x41F699):**
```
if (FUN_004097d5() == 0) { FUN_0040930d();  /* modal dialog ID 100 = "loading/registration" */ if (FUN_004097d5()==0) return 0; }
DAT_004e17f8 = 1;
FUN_00427d89();                       // clear hotspot layer / stepper
FUN_0041b891(2);                      // → world-select screen
if (SendMessage(frame, 0x46F, 0, 0) == 0) FUN_0041b891(1);   // 0x46F,wParam 0 = query "world loaded?"
else { FUN_0041d374();  /* draws "--- Scanning ---" */ FUN_00429c9c("MainMenu.wav"); }
```
`0x46F` with wParam 1/2/3/4 is posted by the registration dialog (0x40A3C2) to mean "ok / retry / needs serial / gold".

### World selection
* State 2 title: `"Where Do You Want To Play Today?"` (`FUN_0041d31f`, 0x41D31F).
* State 3 init `FUN_0041d717` (0x41D717): enumerates `worlds\*` subdirectories with `FUN_0040f5d5` (FindFirstFile, skips `.`/`..`), copies into `s_The_Very_First_World_004df930` (33-byte slots), `qsort` with `LAB_0041d705`, forces the current world first, and if the current world isn't in the list shows `"Ooops! The first person to enter …"`. Sets `view[0x13E8]=0` (scroll index), `view[0x13E4]=view[0x13E0]=1000`, `view[0x13DC]=1`, `DAT_004e09b0 = count`.
* `FUN_0041d3cc` (0x41D3CC) draws the visible rows as hotspots:
  * **0x472** = previous page (text `DAT_004e204c`)
  * **0x473** = next page (text `DAT_004e2048`)
  * **0x474** = a world row, `wParam` = index into the list
  * **0x620** = `"--- or Create Your Own World ---"` (solo) / `"--- or Check On Line for New Worlds ---"` (online)
* Choosing a world copies the name into `DAT_004e0bd0` and the solo stepper `FUN_00438e8e` (0x438E8E) runs:
  * state 0 → timeout branch creates a default `hero2` soul and enters
  * state 1 → `"The World has been chosen: %s"` → state 2 → `FUN_0041b891(3)` (list of souls in that world)
  * `FUN_00428d43` (0x428D43) is the "abort / back to title": `DAT_004df8a4 = -1; FUN_0041b891(3)`.

---

## 3. Hero slot selection, creation, load

### 3a. The "Pick a Soul" window — **not** a dialog resource
Class ctor `FUN_004766ee` (0x4766EE) builds a bare `CWnd` (heap size **0x638**); its OnInit is `FUN_004769bf` (0x4769BF). Members: `CListCtrl +0x5A8` (ctrl id **0x531**, one column `"Available Souls"` width 0x8C), `CButton +0x5F0` (id **0x464**, label `"Purge Soul"`, disabled initially), `CBrush +0x630` = `CreateSolidBrush(0x404040)`, font at `+0x5E8` (Tempus Sans ITC).

Painting `FUN_00476b58` (0x476B58) loads `art\pickSoulLabel.jpg` (`s_pickSoulLabel_jpg_004df600`) and `art\redButtons.bmp` (`s_redButtons_bmp_004f8cfc`). The image is scaled into `rect(+0x74)`; the "NEW SOUL" button is `rect(+0x84)` = image rect inflated by (+8,+8) then top row forced to 0x48×0x20; the "INCARNATE THIS SOUL" button is `rect(+0x94)` = the same offset down by 0x28 (0x48×0x20). It is shown only if a list row is selected.

Click `FUN_00476f4c` (0x476F4C):
```
if (PtInRect(view+0x84, pt))  msg = 0x489;   // NEW SOUL
else if (PtInRect(view+0x94, pt)) msg = 0x486;  // INCARNATE selected soul
PostMessage(frame->0x20, msg, 0, 0);
```

Refresh `FUN_00477060` (0x477060): `FUN_00460962()` builds `Save\<World>\savedHeroes\*.her`, `FUN_0040f688` lists it, each name is truncated at the **last `.`** and inserted into the list.

Selection changed `FUN_004771c5` (0x4771C5): loads the row via `FUN_004181a2(DAT_004e0bd0, name, DAT_0067fbf8)` (§4). On success `*DAT_0067fbf8 = 4` (loaded-but-not-incarnated). If the skin name at hero+0x35 equals the world name (a "default" soul) it regenerates the class body via `FUN_00417add`. Empty-list hint strings: `"Press NEW SOUL button"` / `"Pick a Soul, then INCARNATE"` (`s_Press_NEW_SOUL_button_004f907c`, `s_Pick_a_Soul__then_INCARNATE_004f9060`).

### 3b. The Well button bar → the handler
`FUN_004789d5(panel, mode)` (0x4789D5) is the button-bar config. It calls `FUN_00478673(slot, bmpName, nImages, enabled, parentHwnd, msgId)` (0x478673), which stores into a global bar object: bitmap name at `+0x1A4 + slot*0x105` (261 bytes), state `+0xC00+slot*4`, `+0xC50+slot*4` = parent HWND, `+0xC78+slot*4` = message id. `FUN_004787b2` computes slot rect = `(client.right-0x30, client.top+8, client.right, client.top+0x38)` then `OffsetRect(-3,0)` and `OffsetRect(slot * -0x33, 0)` → **48×48 px, 51 px pitch, right-aligned, top row**. `FUN_00478c6f(bar, slot)` (0x478C6F) fires the slot: `SendMessage(parentHwnd, msg, 0, 0)`.

Mode 0 (the Well):
| slot | bmp | msg |
|---|---|---|
|0|`buttonIncarnate.bmp`|**0x486**|
|1|`buttonHaunt.bmp`|**0x48F**|
|2|`buttonNew.bmp`|**0x489**|
|3|`buttonRestore.bmp`|**0x488**|
|4|`buttonMap.bmp`|**0x487**|
|5..9|empty|id 0x0B|

Mode 1 (alive, in camp/map): `buttonItems` 0x47E, `buttonSpells` 0x47D, `buttonEquip` 0x47C, `buttonStats` 0x485, `buttonMap` 0x484, `buttonCamp` 0x482 (mode 1) / `buttonExit` 0x480 (mode 2), slot 7 `buttonWell` 0x48B.
Mode 2 (in a scene): `buttonCurse` 0x490, `buttonBless` 0x491, `buttonStats` 0x485, `buttonMap` 0x484, `buttonWell` 0x48B.
Contextual extras: `buttonShop.bmp` 0x48A, `buttonMission.bmp` 0x53C, `buttonHunt.bmp` 0x4CB, `buttonGame.bmp` 0x493, `buttonFlee.bmp` 0x47F. All bitmaps are resolved through `FUN_00424526`/`FUN_00486690` (i.e. `art\…`), palette `DAT_004e48a8`.

`DAT_004e18a4` = which of the 8 modeless game dialogs is open (`FUN_00421563`, 0x421563): 1→ID 0xA7, 2→0xB7, 3→0xBF, 4→0xC0, 5→0xC4, 6→0xC5, 7→0xC6, 8→custom `CWnd` titled `"Quadris"` created 400×430 (`CWnd::CreateEx(...,0x80C80000,0,0,400,0x1AE,...)`, 0x421E2). Sound: `sfx\openDialog.wav` / `sfx\closeDialog.wav`.

### 3c. The Well command handler — `FUN_0042095e(view, cmd)` (0x42095E)
This is where 0x486/0x488/0x489/0x48F land (`cmd` 0/1/2/3/4).

* **cmd 0 — Incarnate** (0x486). If no soul: `"My Child, how can you incarnate before you have a soul?"` / title `"The Blind Sage Remarks"` (`FUN_00458343`). Otherwise `FUN_004978d5(hero,1)` (resurrect/recharge), `*hero = 1`, `hero[1] = DAT_004dd20c` (serial), `hero[0x1B1] = DAT_004fa95c` (world CRC-1), `hero[0x1AA] = 0xA97`, then `FUN_0041f89c(serial)` (add to the on-screen soul list) and `FUN_00420240()` → **incarnate**.
* **cmd 3 — New soul** (0x489). Shows **dialog resource 138 (0x8A)** modally. On OK:
  ```
  FUN_00418178(hero, 0x16CC)         // no-op (see §4)
  FUN_00416a3f(hero); FUN_00420132(hero);   // reset record, keep serial+name
  strcpy(local_38, name);
  FUN_004200ca(hero);                // clear runtime ptrs, hero[0x64]=DAT_004e1020, hero[0x6C8]=0, hero[0x974]=-that, hero[0xEFC]=FUN_004142d8(0), hero[0x69C]=0, hero[0x760]=0
  strncpy(hero+0x14, name, 0x20);            // soul name, 32 bytes
  hero[0x2AE] = FUN_004624d2(hero+0x14);      // name hash
  hero[0x1C] = hero[0x1D] = dlg[0x64];        // current/max HP from class[0x19D7C]
  hero[0x1E] = hero[0x1F] = dlg[0x60];        // current/max MP from class[0x19F10]
  hero[0x18] = dlg[0x6C];                     // CLASS INDEX
  hero[0x1A6] = dlg[0x33C];
  hero[0x1A5] = dlg[0x334];
  hero[0x278] = dlg[0x330];
  hero[0x1A8] = dlg[0x32C];                    // GENDER (combo selection)
  hero[0x2A8] = dlg[0x2FC];
  hero[0x1AA] = hero[0x1D9] = 0xA97;          // 100% health (27.11 fixed point)
  hero[0x1B1] = DAT_004fa95c;
  FUN_0043a99e(hero, 800000);                 // recompute max HP
  FUN_004207bd(hero, hero[0x18]*0x1ACD0 + DAT_004e488C);   // APPLY CLASS TABLE
  if (0 < hero[0x1A8] && hero[0x1A8] < 9) hero[hero[0x1A8] + 0x1BE] = 5000;  // starting element PP
  FUN_00449b76(hero, 0);
  ...
  CDialog::DoModal(dlg2);      // resource 149 (0x95) — the "your soul is ready" / follow-up panel
  FUN_0043422e(1);
  FUN_0042095e(0, 0);          // recurse into Incarnate
  ```
* **cmd 4 — Haunt** (0x48F): picks a ghost from the soul list; sets `*hero = 2`, `hero[0x33] = ghostSerial`; rejects with `"One ghost cannot haunt another…"` / `"Your troubled spirits are evidently…"` / `"Before you can haunt another s…"`.

`FUN_004207bd` (0x4207BD) — "apply class defaults": copies class+0x1AA1C..+0x1AA2C (5 abilities) into `hero+0x680..0x690` and sets `hero[0xCCC`]=their sum]; takes the max of 8 values from class+0x1AA44 into `hero+0x6CC`; max of 8 from class+0x1AA84 into `hero+0x6FC`; 8 starting spells from class+0x1ABEC; 8 starting items from class+0x1AC2C (`"%s\..\%d"`-style names, `FUN_00484E72`); 0x1000-entry flag block from class+0x1AC0C; then `FUN_00449AA2(hero,0)` and `FUN_00449B16(hero,0)`.

`FUN_00420240` (0x420240) — **incarnate**:
```
time(&local_8); hero[0x2A3] = hero[0x2AA] = local_8; hero[0x2A9]=0; hero[0x3BA]=0;
FUN_0042b041(3);  FUN_00416a64();          // save the hero
hero[1] = DAT_004dd20c;  hero[0x1CC]++;   // incarnation count
hero[0x1AA] = 0xA97;
FUN_004078c4(); FUN_00407213();
FUN_004306f6(0x33/0x55/0x48);              // MIX: "I am telling my name", bio, etc.
FUN_0041e200(DAT_004e0ddc);               // (re)build the current map
FUN_00420714(1);                          // close overlays, → state 6
FUN_00461a07(0,0x8000,0x8000);
FUN_004a43aa();
strcpy(s_unincarnated, hero+0x14);
FUN_00431f69(serial, "lastIncarnationName", s_unincarnated);
FUN_00422712(); FUN_004624ef(DAT_004fa95c);
SendMessage(frame, 0x46A, hero[0x19F], hero[0x24]);    // <-- goto (link, map)
if (hero[0x336] == 0) { if (MessageBox("You have not yet specified y…", "Place Yourself On Gaiea", 4) == 6) FUN_00434f95("earth"); }
FUN_004306f6(0x73);
FUN_0041fd46(serial); … FUN_0043c668(); FUN_00455f7d();
```
`0x46A` is "change map": `wParam` = link index, `lParam` = map number (see `FUN_00463853`, 0x463853, and the MIX handlers at 0x438155/0x438E76 which do `SendMessage(frame, 0x46A, <map>, <scene>)`). `0x475` is "join scene" (`SendMessage(frame,0x475,serial,scene)`), `0x476` = "leave scene".

### 3d. Dialog 138 (0x8A) — "New Soul"
Ctor `FUN_0045fda9` (0x45FDA9): `CDialog::CDialog(this, 0x8A, parent)`, 0x348 bytes.
Members: `CString +0x7C` (name), font obj `+0x98`, `CComboBox +0xAC` (**gender picker**, custom vtable), `CStatic +0xEC` (class description), `CButton +0x12C`, `CButton +0x16C`, `CStatic +0x1AC/+0x1EC/+0x22C/+0x26C` (stat labels), **`CListBox +0x2AC` (class list)**, `CButton +0x2EC` (OK), **`CEdit +0x32C` (name entry)**. Results: `+0x60` maxMP, `+0x64` maxHP, `+0x68` PK flag, `+0x6C` class index, `+0x70` magic ratio, `+0x74` hand ratio, `+0x78` start element.

`OnInitDialog` `FUN_004603b4` (0x4603B4):
* `art\shopBorder256.bmp` as the dialog background.
* Fills the class listbox: for `i` over the class table (base `DAT_004e488C`, stride **0x1ACD0**, up to `0x951450` → **89 classes**), skipping any class with `class[0x1ABE0] != 0` (HIDDEN_CLASS):
  `sprintf(buf, "%s - %d (magic %d, hand %d)", class+4, class[0x1A560], class[0x1A6F4])`, `LB_ADDSTRING(0x180)`, `LB_SETITEMDATA(0x19A) = i`. Then `LB_SETTOPINDEX(0x186)`.
* Fills the gender combo (`+0xAC`) for `i = 0..3` with `FUN_0047c519(i, "menu", buf)` — i.e. the `menu=` key of section `[i]` of `worlds\<World>\gender.ini` — skipping empty values and the literal `"."`. Sets `CB_SETCURSEL(0x14E, 0)`.
* `SetWindowText(static +0xEC, class[0x1AAC4])` (the class `DESCRIPTION`).

`OnOK` `FUN_00460a7e` (0x460A7E):
1. `GetWindowText(edit +0x32C)` → `TrimLeft/TrimRight` → `CString +0x7C`.
2. `FUN_00460765` (0x460765) — sanitise: strip leading/trailing spaces, and replace every byte ≥ 0x80, plus `"`, `'`, backtick, `(`, `)`, `*`, `,`, `-`, `.`, `/`, `:`, `;`, `<`, `>`, `?`, `[`, `]`, `|`, `~` with `'_'`.
3. `FUN_00460805` (0x460805) — validate. Failure messages: `"Too Short!"` (<2 chars), `"Too Few Letters!"` (<2 alphabetic), `"Too Much Punctuation!"` (if `letters+4 > len-letters`), `"Too Many Spaces!"` (>1 space), `"Too Naughty!"` (`FUN_0049d015` bad-word filter), `"Reserved Name"` for `"Samsyn"`/`"Uncle Dan"` when serial != 1.
4. `FUN_00460962(buf, name, DAT_004df644)` builds the `.her` path; `_access != -1` → `"A soul already exists in this world"` / title `"Duplicate Soul Name"`, option 6 → `FUN_00477461(name)` (delete it).
5. PK checkbox at `+0x30C`: if checked → `"You have chosen to be a Player Killer"`, else `"You have chosen NOT to be a Player"`; title `"Review Your PK Choice"`; any answer other than 7 aborts.
6. `LB_GETCURSEL(+0x2CC)` → class index `c`; `c*0x1ACD0 + DAT_004e488C`; then fills `+0x64 = class[0x19D7C]`, `+0x60 = class[0x19F10]`, `+0x6C = c`, `+0x70 = min(class[0x1A560],100)`, `+0x74 = min(class[0x1A6F4],100)`, `+0x78 = min(class[0x1A888],8)`, `+0x68 = (checkbox==1)`.
7. `CDialog::OnOK`.

Empty name → `"Please choose a name for your so…"` / `"Missing Soul Name"`.

### 3e. Follow-up dialog 149 (0x95)
Ctor `FUN_00448e0f` (0x448E0F): `CDialog::CDialog(this, 0x95, parent)`, 0x510 bytes, 5 sub-objects, `+0x140` state. Shown modally right after a soul is created, before incarnating.

---

## 4. Hero save file (`.her`)

### Path
`FUN_00460962` (0x460962) — the only hero-path builder:
```
sprintf(out, "%s\\save\\%s\\savedHeroes", DAT_005384d0 /*root*/, DAT_004e0bd0 /*world*/);
// also (dead code) sprintf(local,"%s\\worlds\\%s\\savedHeroes",root,world);
create <root>\save ; <root>\save\<world> ; <root>\save\<world>\savedHeroes
if (name && *name) { strcat(out, "\\"); strcat(out, name); strcat(out, "." /*DAT_004de650*/); strcat(out, ext); }
```
`ext` is `DAT_004df644` for heroes ⇒ **`Save\<World>\savedHeroes\<SoulName>.her`**. Evidence it is `.her`: the picker lists `savedHeroes\*.<that ext>` and strips everything from the last `.` (0x477060, 0x477078) — the same code path is reused for `tactics.ini` (`FUN_00441bb5` at 0x441BB5 builds `%s\save\%s\savedheroes\%s` + `"tactics.ini"`), and `Save\readme.txt` calls them "hero files". `[UNVERIFIED]` only in the sense that the literal itself lives in `.rdata` at 0x4DF644 and I could not dump `.rsrc`/`.rdata` strings.

Other per-soul files: bio INI at `%s\bio\%s\%08X_%s.ini` (section `s_…004eb538`, keys `serNum`, `className`, `levelName`, `worldLocation`, `skin`) — `FUN_00438c05` (0x438C05). Tactics: `Save\<World>\savedHeroes\<soul>\` + `tactics.ini`.

### Format
**Fixed-size binary record, 0x16CC (5836) bytes, no magic, no version header.** Written/read with a single `fwrite`/`fread` of `0x16cc` bytes.

* Save: `FUN_00417f1b` (0x417F1B)
  * copies 33 ints (0xD83219-0xD831F8) from global `DAT_00D831F8` into `hero+0x738`
  * `hero[0x1CD] += (GetTickCount()-_DAT_004e1800)/1000`; `hero[0x3C0] = FUN_004142d8(hero[0x1CD])`
  * `hero[0x279]++`; `FUN_0040fe3b(path, n)` (bio header), `FUN_0049923b(path, n)`
  * `hero[0x1DA] = 1`; `hero[0x5B2] = FUN_00416abb(hero)` (checksum)
  * `fwrite(hero, 1, 0x16CC, fopen(path,"wb"))`; on short write → `PostMessage(frame, 0x4DF, 3, 0)` and a fatal-cheat flag `DAT_004e709c = 1` (`FUN_004a8664(0x417)`, `FUN_00449a17`)
  * guarded by: online mode off (`DAT_004e61e4 == 0`), world-editor off (`DAT_004e4890 == 0`), `FUN_00417a21() == 0`, `DAT_004e709c == 0`, `*hero != 2` (not a ghost)
* Load: `FUN_004181a2` (0x4181A2)
  * `FUN_00416a3f(hero)` (detach pet), `memset(hero, 0, 0x16CC)`, `fread(hero, 0x16CC, 1, fopen(path,"rb"))`
  * **validity check** — rejects (returns 0) unless all of:
    `FUN_00416abb(hero) == *(uint32*)(hero+0x16C8)`, `fread` returned 1, `hero[0x68] == 0`, `hero[0x6C8] == 0`, `hero[0x760] == 0`, `hero[0x69C] == 0` (those four are RAM-only pointers, so they must be zero on disk)
  * then clears the RAM-only fields `+0x0C, +0xCC, +0x5C, +0x58, +0x6C0, +0x978`; if `hero[0x9E8] == 0` computes max HP as `FUN_0043a99e(hero, ((hero[0x64]*0x640)/100 + 800) * 1000)`
  * `FUN_00418178(hero, 0x16CC)` is called and its result checked — **it is a no-op**: the loop is `b = *p; *p = ~b; *p = b;` (`FUN_00418178`, 0x418178). The former XOR/encrypt is gone; there is **no obfuscation**
  * copies `hero+0x738` (33 ints) back into `DAT_00D831F8`
  * `FUN_00416896(hero)`; `FUN_0040fbfd(path)`; `FUN_004990b4(path)` (tactics.ini)
  * if `hero[0x9E0] == 0` then `hero[0x9E0] = 100 - hero[0x694]`

### Checksum — `FUN_00416abb` (0x416ABB), over bytes `[0, 0x16C8)`
```c
uint32 h = 0x379ADE;
if (hero[0x768] == 0)              /* old / "sign extended byte" variant */
    for (i = 0; i < 0x16C8; i++) { h ^= (int)(int8)hero[i]; if (hero[i]) h <<= (i & 1); }
else                               /* current variant, selected by hero[0x1DA] */
    for (i = 0; i < 0x16C8; i++)  h ^= (uint32)(uint8)hero[i] << (i % 0x18);
```
The same rotate-xor is used for all other binary blobs as `FUN_00416b1f(buf, len)` (0x416B1F) with the checksum stored in the **last 4 bytes** of the blob:
* `DAT_004e4878`, 0xC5A4 bytes, sum at +0xC5A0 (0x40FD8C)
* `DAT_00d307d8`, 0x4A8 bytes, sum at +0x4A4
* `DAT_00d30c80`, 0x44C bytes, sum at +0x448
* `DAT_004e487c`, 0x7C8 bytes, sum at +0x7C4
* timing.cfg, 0x94 bytes (`FUN_004098b9`/`FUN_00409ac4`), with a separate 8-bit-rotate integrity value `DAT_005385dc` from `FUN_00409816`
* map `*.obl` records are 800 bytes each (`FUN_00463989`/`FUN_0046393f`, `memset(dst,0,n*800)`)

### Field layout (`hero = DAT_0067FBF8`, stride 0x16CC, byte offsets)
The hero table is `hero[i]`, up to `0x5B300/0x16CC` = **100 records**, each exactly the on-disk size.

| offset | size | field | evidence |
|---|---|---|---|
|0x0000|4|`inUse` (>=1). 1=alive, 2=ghost, 4=loaded/unincarnated|FUN_0041f832, 0x42095E|
|0x0004|4|**serial** (`playerID`), `DAT_004dd20c`|0x41F832 `piVar2[1] != param_1`|
|0x0008|4|—| |
|0x000C|4|RAM-only (forced 0 on load)|0x4181A2|
|0x0014|0x21 (33)|**soul name**, NUL-terminated; `strncpy(...,0x20)`|0x4209F7, 0x42714E|
|0x0035|0x1F (31)|**skin file name** (no `.bmp`)|0x438C05, 0x47083|
|0x0058|8|RAM-only: pet CWnd* + aux|0x416A3F, 0x4181A2|
|0x0060|4|`num_hostClass` (script var `num_hostClass`)|0x4FBCE4 @0x90628|
|0x0064|4|**level**|0x4A8F5A `levelName(class, hero[0x64])`|
|0x0068|4|RAM-only, must be 0 in a file|0x4181A2|
|0x006C|4|RAM-only, must be 0|0x4181A2|
|0x0080 / 0x0084|4 / 4|level / level-name id (display)|FUN_0049BFB3 args 3–4|
|0x0090|4|**current map index**|0x438155, 0x41F465|
|0x009C|4|—| |
|0x00CC|4|RAM-only: pointer to the owning/other hero (recalled-from)|0x416A3F, 0x4DF0EC|
|0x0100|4|`maxWallet`? (used with class record)|0x438C5C|
|0x011C / 0x0124|4 / 4|scene id / scene name|0x4FC450|
|0x01A0..0x01A4|5×4|**base abilities STR,WIS,STA,AGI,DEX** (all set to 100 at creation)|0x42927A, 0x478840|
|0x01A7|4|derived ability (HUD)|0x478840|
|0x01A8|4|**gender** (0..3)|0x420A3F|
|0x01B0|4|scene-start tick|0x4309D6|
|0x01B1|4|**world CRC-1** (`DAT_004fa95c`)|0x4201BC|
|0x01B2|4|`= -hero[0x1A]` bonus|0x42927A|
|0x01BE + 8·e|8×4|**starting elemental PP** (set to 5000 for the class's element)|0x420A62, 0x4DBA0F|
|0x01CD|4|seconds played|0x417E91|
|0x01CE|4|last-incarnation name id|0x417F1B|
|**0x01CF**…| |see note||
|0x0272|0x400 (1024)|inventory bitmap, 1 byte per item id 0..1023|FUN_0045FC2B `param_1 + 0x272 + id`|
|0x067C|4|**current link index** (0 = link 0)|0x463853|
|0x0694|4|used as `100 - x`|0x4181A2|
|0x069C|4|RAM-only, must be 0|0x4181A2|
|0x06A4|4|—|0x41F465|
|0x06AC|0x8C8 (4×0x1000)|RAM-only extra flag blobs|FUN_0045FC2B|
|0x06BC|8|current HP (fixed point)|0x4181A2, 0x4F1E23|
|0x06C0|4|RAM-only|0x4181A2|
|0x06C8|4|RAM-only, must be 0|0x4181A2|
|0x06CC / 0x06DC / 0x06FC|3×8|per-element / per-hand max PP|FUN_004207BD|
|0x071C|4|—| |
|0x0734|4|seconds played (working copy)|0x417E91|
|**0x0738**|**0x84 (33 ints)**|**"where I am" block, mirrored to/from `DAT_00D831F8`**|0x417F1B, 0x4181A2|
|0x0760|4|RAM-only, must be 0|0x4181A2|
|0x0768|4|**checksum algorithm selector** (0 = legacy, 1 = current)|0x416ABB|
|0x07BC|4|—| |
|0x0978|4|RAM-only|0x4181A2|
|0x0994|4|—| |
|0x09E0|4|`100 - hero[0x694]` if 0|0x4181A2|
|0x09E8|4|if 0 → max HP from level|0x4181A2|
|0x09F4|0x100|name shown in the soul list|0x41F89C|
|0x0A34|4|—|0x50FBA9|
|0x0A4C|4|save counter|0x417F1B|
|0x0A68|4|house / guild id|0x4AF82C|
|0x0AA0|4|class / house slot|0x47C681|
|0x0AC8|4+|`0x6CC` … `0x780` (8-element / 8-hand groups) ||
|0x0CDC|4|skin id|FUN_0045FC2B users|
|0x0EEC / 0x0EE8|4|pet ids||
|0x0EFC|4|`FUN_004142d8(seconds)` = fatigue/energy|0x417F00|
|0x0F00|4|`FUN_004142d8(hero[0x734])`|0x417E91|
|0x0FBC|4|ditto for the level-based value|0x4201A2|
|0x16C8|4|**checksum** (written as `hero[0x5B2]`) — file size is 0x16CC so this is the last 4 bytes|0x417F1B|

> Note: offsets 0x19C–0x1AB (0x64–0x6B as ints = 0x190–0x1AC) carry the six creation-dialog values written at 0x420A3F: they are the four HP/MP values (0x1C/0x1D/0x1E/0x1F) plus 0x1A5/0x1A6/0x1A8. The exact semantics of 0x1A5, 0x1A6, 0x2A8 and 0x2FC (and of the record regions I did not cross-reference) are `[UNVERIFIED]`.

### Class table (levels.txt) — base `DAT_004e488C`, stride **0x1ACD0** (109 520), 89 entries
| offset | field |
|---|---|
|+0x0000|class number (n×100)|
|+0x0004|`char[0x100]` class name, `'\|'`-separated per gender (4)|
|+0x19BE8 + 4·L|`startHP[level]` (101)|
|+0x19D7C + 4·L|`maxHP[level]` (101)|
|+0x19F10 + 4·L|`maxMP[level]` (101)|
|+0x1A564 + 4·L|second interpolated max array (101)|
|+0x1AA1C|5 ints `START_ABILITY str,wis,sta,agi,dex`|
|+0x1AA30|5 ints `MAX_ABILITY` (clamp 0..255)|
|+0x1AA44|8 ints `START_ELEMENT_PP`|
|+0x1AA84|8 ints `START_HAND_PP`|
|+0x1ABCC|`START_LOCATION` arg1 = **map number**|
|+0x1ABD0|`START_LOCATION` arg2 = **link number**|
|+0x1ABD4|`START_LOCATION` arg3 = **dropIn / y**|
|+0x1ABD8|`NO_GIFTS` flag|
|+0x1ABDC|`MAX_WALLET` (clamp, `-1` if < 1)|
|+0x1ABE0|`HIDDEN_CLASS` flag (1 = not offered in the new-soul dialog)|
|+0x1ABE4|`HIDDEN_CLASS` start level|
|+0x1AAC4|`char[0x103]` `DESCRIPTION`|
|+0x1AC0C|0x1000 ints starting inventory flags|
|+0x1AC2C|8 ints `START_SPELLS` (clamp 0x2FF)|
|+0x1AC4C / +0x1AC6D / +0x1AC8E / +0x1ACAF|`char[0x1F]` `DEFAULT_SKIN` for gender 0/1/2/3|
|+0x1A560 / +0x1A6F4|`MAGIC_RATIO` / `HAND_RATIO` (0..100)|
|+0x1A888|`arg7` = starting element index (0..7)|

Parser: `FUN_00483…` region (0x4841C0–0x4844DF). It computes the per-level HP/MP curves with a semi-log interpolation plus a 5000-per-step cap (`local_3c = 5000`).

---

## 5. First map & position for a fresh hero in Evergreen

**Answer: map 0, link 0. Not SCENE 0.**

* `maps.txt` line 1: `0, evergreen.jpg, evergreen, "Evergreen", 131072`. Map 0 is the main world map (quest.txt:221 "Map 0 is always the main world map").
* `levels.txt:176-181` documents `START_LOCATION <map>, <link>, <dropIn>`: *"Lets you set where a newly created character will appear on their first incarnation, based on class. **Otherwise, they will appear above link 0 on map 0.**"*
* **None of the Evergreen classes 100..1100 declare `START_LOCATION`.** The only `START_LOCATION` in the whole file is on class `8800` (Piano-User): `START_LOCATION 2, 13, 1` (levels.txt:619).
* The class-record default is 0, so on soul load (0x42095E):
  `hero[0x24] (=+0x90, current map) = class[0x1ABCC] = 0`, `hero[0x19F] (=+0x67C, current link) = class[0x1ABD0] = 0`, `hero[0x336] (=+0xCD8, dropIn) = class[0x1ABD4] = 0`.
* `FUN_00420240` then sends `0x46A(link=0, map=0)` and, because the dropIn is 0, shows `"You have not yet specified your position"` / `"Place Yourself On Gaiea"`; answering Yes runs `FUN_00434f95("earth")`, the manual "place yourself" map picker.
* `SCENE 0 temple, WELL, "Well of Souls", 2, 7` (quest.txt:1425) is **not** the start point. quest.txt:1412-1414 says verbatim: *"SCENE 0 must be primary well of souls. It is only used for switching souls (saved games), not for general resurrection and recharge."* The Well is the camp/incarnate screen you sit in (state 5, `art\beg`-era button bar with Incarnate/New/Restore/Map), not a map the hero spawns onto.
* Map table in memory: `DAT_00CE2B58 + map*0x40` (name string `+0x45`, 0x100 bytes ⇒ `DAT_00CE2B9D + map*0x100`), per-map 0x100-byte record at `DAT_00CE2C20 + map*0x100`. Map flags are `int`; 131072 = `0x20000` = `NO_PKREZ` for map 0 (quest.txt:255). Loaded by `FUN_0041E421(world, frame, mapIndex, loadNew)` (0x41E421) which clamps `mapIndex` to 0 if `map<0 || map>999 || table[map]==0`, then reloads `worlds\<World>\quest.txt` into `DAT_004e0cd8`, reads ~12 `config.ini` `[General]` keys into `DAT_004E0FF0..0x1020` (via `FUN_0047c5c5`, `%s\worlds\%s\config.ini`; one of them is `startingGP=500`), loads `art\objects.bmp`/`.jpg`/`.obr`, then `<map>.obl`, `<map>.mon`, `<map>.ter`, and sets the view scroll centre `DAT_00539388 = mapWidth/2`, `DAT_00539384 = screenH/2`.
* `FUN_00416b55` (0x416B55) is the "is this Evergreen?" test (`_stricmp("Evergreen", DAT_004e0bd0)`) — a few behaviours are hard-coded for it.

---

## 6. Main window layout

All numbers below come from `FUN_0041b476` (0x41B476, layout on every resize) and `FUN_0041b2de` (0x41B2DE, `OnSize` → `FUN_00405878(view, w, h)`).

Given `GetClientRect(hwnd, &c)`:

| region | rect (offsets from `view`) | notes |
|---|---|---|
| bottom status/chat strip | `left, c.bottom-0x40, right, c.bottom` | **64 px** tall; child at `view+0x5A48` is moved into it |
| right column | `right-0x14-0x100 … right-0x14` | 20 px margin + **256 px** column = button bar |
| left margin strip | `c.left, c.top, c.left+0x14, c.bottom-0x40` | 20 px, only when `c.width > 0x280` (**640**) |
| **map view rect** | `view+0x138C..0x1398` = `(c.left, c.top, c.right-0x14-0x100, c.bottom-0x40)` | the 256 KB BIG_MAP blit target |
| hero/equipment panel | `view+0x3508` = `(c.right-0x14-0x100, c.top, c.right-0x14, c.top+0x100)` | **256×256**, filled with `ImageList_Draw(view+0x9C, …)` |
| splitter | `view+0x13FC`, `CSplitterWnd::CreateStatic(view, 2 rows, 1 col, 0x50000000, 0xE900)` | top pane = map view, bottom pane = chat |
| splitter rows | `SetRowInfo(0, h, 0xAA)`, `SetRowInfo(1, total-h-top, 0x0C)` with `h = min( (width-296)*0x100/0x168, height-0x5A )`, further capped at `height-0x96` when `width > 0x2D4`, floor **0x118 (280)** | |
| chat log | `CListCtrl view+0x424C`, ctrl id **0x52C**, moved to `(0x13CC.left, 0x13D0, 0x13D4, 0x13D8)` | 9 columns, `InsertColumn` widths 0x7D,0x41,0x37,0x37,0x78,0x82,0x78,0x5A,0x5A |
| full-rect child (`view+0xD8C`) | whole client | tree/HTML view |
| sheet overlays | `view+0x1378..0x1384` = the map rect | child dialogs at `view+0x428C` (items), `+0x4848`, `+0x53F8`, `+0x5770`, `+0x5928`, `+0x4E80`, `+0x566C` |
| 20 px scrollbar-ish rect | `view+0x64` = `(right-0x14-0x114, top, right-0x14-0x100, bottom)` | |
| button bar | 10 slots, 48×48, pitch 51, `y = c.top+8`, right-aligned, laid out right→left | `FUN_004787B2` |

`CSoulsView::OnCreate` (0x41AD72) creates, all initially hidden: `CSplitterWnd +0x13FC`, and dialogs `0xB1`→`+0x3288`, `0x82`→`+0x3518`, `0x84`→`+0x3738`, `0x85`→`+0x39A4` (m_linkEdit), `0x96`→`+0x408C`, `0xA3`→`+0x6710`, `0xEF`→`+0x2CA8` (global `DAT_004df8c8`), `0xC4`→`+0xBEC`. Also two image lists (`CImageList::Create(0xA6, 0x10, 1, 0x808000)` at `+0x98` and `0xB9` at `+0xA0`) and a 256×256 `misc32.bmp` bitmap.

`CSoulsView::OnDestroy` = 0x41B2AC.

Fonts: `Tempus Sans ITC` everywhere (`s_Tempus_Sans_ITC_004dc5b0`), created with `CFont::CreatePointFont(n, name, 0)` where `n = FUN_004281c6(x)`. Dialog list controls use `SendMessage(hwnd, 0x1024 /*LVM_SETTEXTCOLOR*/, 0, 0xFFFFFF)` + `0x1001/0x1026 /*LVM_SETBKCOLOR/* ` = `0x404040` (the game's dark-grey chrome).

---

## 7. Dialog / window resource IDs (from the decomp, not from `.rsrc`)

**Modal / front-end (constructed with `CDialog::CDialog(this, ID, parent)`):**

| ID | dec | ctor | what |
|---|---|---|---|
|**100 / 0x64**|0x408FE7|0x408FE7|registration/loading progress box; `CEdit +0xE4`, statics `+0x64/+0xA4/+0x124`; `FUN_0040938A` OnInit; shown by `FUN_0040921B`/`FUN_0040930D`|
|**138 / 0x8A**|0x45FDA9|0x45FDA9|**New Soul** (name edit, class list, gender combo, PK check). OnInit 0x4603B4, OnOK 0x460A7E|
|**149 / 0x95**|0x448E0F|0x448E0F|post-creation follow-up panel|
|141 / 0x8D|0x5A534|—|generic|
|135 / 0x87|0x54188|—|list-style|
|136 / 0x88|0x56244|—||
|139 / 0x8B|0x55112|—||
|142 / 0x8E|0x42B26E|—|item-edit sheet|
|143 / 0x8F|0x499D0A|—||
|145 / 0x91|0x49CAEE|—|HP/MP sheet|
|146 / 0x92|0x4C6B7C|—|spells sheet|
|147 / 0x93|0x4BA69F|—|equip sheet|
|**148 / 0x94**|0x40A463|0x40A463|status/“tracking” panel (`FUN_00421364`)|
|150 / 0x96|0x4B1DC7|0x4B1DC7|file-browser style|
|151 / 0x97|0x46AE14|—||
|152 / 0x98|0x4B5F12|—||
|153 / 0x99|0x4B40A1|—||
|154 / 0x9A|0x4C0D07|—||
|155 / 0x9B|0x4C74B6|—|property page w/ `CAMP_STYLE` column, sub-dialogs 0xA0/0x93/0xBD|
|156 / 0x9C|0x40A4FF|—||
|157 / 0x9D|0x49E4FC|—||
|158 / 0x9E|0x4B8C56|—||
|159 / 0x9F|0x49AF1C|—||
|160 / 0xA0|0x4B6A2D|—||
|161 / 0xA1|0x4906B9|—||
|162 / 0xA2|0x490C0F|—||
|**163 / 0xA3**|0x411BC3|0x411BC3|child of the main view (`view+0x6710`)|
|164 / 0xA4|0x4AF27E|—||
|**165 / 0xA5**|0x4A7A1F|—|the “Quadris” tactics window (created as a raw CWnd 400×430, 0x421E2)|
|166 / 0xA6|0x4A05F1|—||
|**167 / 0xA7**|0x4D0FDE|0x4D0FDE|game dialog slot 1 (`DAT_004e18a4==1`)|
|168 / 0xA8|0x4B5B9D|—||
|169 / 0xA9|0x4AD0B5|—||
|170 / 0xAA|0x4A4FE6|0x4A4FE6||
|**171 / 0xAB**|0x4AE465|—||
|172 / 0xAC|0x4A2357|—||
|173 / 0xAD|0x4058CA|0x4058CA|5 buttons + 2 statics, field `+0x64 = 300`|
|174 / 0xAE|0x42A825|0x42A825||
|175 / 0xAF|0x4A8FA0|0x4A8FA0||
|176 / 0xB0|—|—||
|177 / 0xB1|0x40955B|0x40955B|child of main view (`view+0x3288`)|
|178 / 0xB2|0x4B679D|—||
|179 / 0xB3|0x40A3EF|0x40A3EF|created on `DAT_004e4840`|
|180 / 0xB4|0x4A40A1|—||
|181 / 0xB5|—|—||
|**182 / 0xB6**|—|—||
|**183 / 0xB7**|0x405651|0x405651|game dialog slot 2|
|**184 / 0xB8**|0x4A05B0|—||
|185 / 0xB9|—|—||
|**190 / 0xBE**|0x4A9459|—|game dialog slot 3|
|**191 / 0xBF**|0x4A8FD0|0x4A8FD0|game dialog slot 3|
|**192 / 0xC0**|0x4045B0|0x4045B0|game dialog slot 4|
|193 / 0xC1|0x4B41A2|—||
|194 / 0xC2|—|—||
|195 / 0xC3|0x4B0D6E|—||
|**196 / 0xC4**|0x41589F|0x41589F|chat/comm (`view+0xBEC`)|
|**197 / 0xC5**|0x49CB4A|0x49CB4A|game dialog slot 6|
|**198 / 0xC6**|0x49A57C|0x49A57C|game dialog slot 7 (tactics)|
|199 / 0xC7|—|—||
|200 / 0xC8|—|—||
|201 / 0xC9|—|—||
|202 / 0xCA|—|—||
|203 / 0xCB|—|—||
|**204 / 0xCC**|0x49B125|0x49B125||
|205 / 0xCD|0x4A2804|—||
|206 / 0xCE|0x491FD6|—||
|207 / 0xCF|0x49B00B|—||
|208 / 0xD0|—|—||
|209 / 0xD1|—|—||
|210 / 0xD2|0x4945A6|0x4945A6|generic `MessageBox`-alike|
|211 / 0xD3|—|—||
|212 / 0xD4|0x4010A9|0x4010A9||
|213 / 0xD5|0x40409B|0x40409B||
|214 / 0xD6|0x49B68A|0x49B68A||
|215 / 0xD7|0x4A0F39|0x4A0F39||
|216 / 0xD8|0x49B1D1|0x49B1D1||
|217 / 0xD9|—|—||
|218 / 0xDA|0x49B49D|0x49B49D||
|219 / 0xDB|0x49BC45|0x49BC45||
|220 / 0xDC|—|—||(0xDC/0xDD are bitmaps, not dialogs)|
|221 / 0xDD|—|—||
|222 / 0xDE|0x49F73F|0x49F73F||
|223 / 0xDF|—|—||
|224 / 0xE0|—|—||
|**225 / 0xE1**|0x49236D|0x49236D|(small 2-object dialog)|
|226 / 0xE2|0x4A15E9|0x4A15E9|CImageList member|
|227 / 0xE3|—|—||
|228 / 0xE4|—|—||(0xE4 is RT_ICON)|
|229 / 0xE5|—|—||
|230 / 0xE6|—|—||
|**231 / 0xE7**|0x4A0301|0x4A0301|centered on the frame (`FUN_00421461`)|
|232 / 0xE8|—|—||
|**233 / 0xE9**|0x4A0A2B|0x4A0A2B||
|234 / 0xEA|0x401515|0x401515||
|235 / 0xEB|—|—||
|236 / 0xEC|—|—||(0xEC is RT_CURSOR)|
|**237 / 0xED**|0x458F02|0x458F02|mission picker (`FUN_004213E8`)|
|238 / 0xEE|—|—||
|**239 / 0xEF**|0x4A4B3D|0x4A4B3D|child of main view (`view+0x2CA8`, global `DAT_004df8c8`)|
|240 / 0xF0|—|—||
|234 / 0xEA|0x401515|0x401515||

Bitmaps also loaded from resources: `AfxFindResourceHandle(0xDC..0xDD, RT_BITMAP)` at 0x4A8455 (button bitmaps), `LoadIconA(0xE4, RT_ICON)`, `LoadIconA(0x80, RT_ICON)` for the app icon.

**Non-dialog custom windows:**
* `Pick a Soul` — raw `CWnd` (registered class, `CWnd::CWnd` + custom vtable `&PTR_LAB_004cfe40`, 0x638 bytes), 0x4766EE.
* `"Quadris"` — raw `CWnd::CreateEx(..., "Quadris", 0x80C80000, 0,0,400,0x1AE, parent, NULL, NULL)`, 0x421E2.
* Web browser view — dialog `0xE9` created with `CWnd::CreateControl(..., IID_IWebBrowser2, ...)` at 0x4A6C7B (0x453xxx region), 0x2F4 bytes.

---

## 8. Things a C port must reproduce (checklist)

1. Root-relative paths with `SetCurrentDirectory(installRoot)`; create `temp`, `temp\sceneCache`, `save` at boot; create `save\<world>` and `save\<world>\savedHeroes` lazily in `FUN_00460962`.
2. `.her` = 5836 raw bytes + a 4-byte checksum at 0x16C8 (alg 1: `h=0x379ADE; h ^= (u8)b[i] << (i % 24)`). No magic, no compression, no XOR. The four RAM-pointer fields 0x68/0x6C8/0x760/0x69C must be written as 0 and rejected on load if non-zero.
3. Two CRCs per world (bytes-read CRC and parsed-table CRC), compared against the update manifest — needed to accept a world at all.
4. Front-end UI is per-mille-positioned animated text over a full-screen JPEG; port as a hotspot list, not as dialogs.
5. State machine with the exact 13 states; 13 s title dwell; `state != 4` triggers the per-frame tick `FUN_0041bdb4`.
6. Fresh hero: class index → apply class table (abilities, element/hand PP, items, spells, skin) → `START_LOCATION` or (0,0) = map 0 link 0 → prompt to place yourself.
7. Layout: 64 px bottom strip, 20 px side margins, 256 px right button column, 256×256 hero panel, 48×48 buttons at 51 px pitch.
8. Solo mode is the *absence* of a network type; nothing special to switch on except gating the "Create Your Own World" link on `DAT_004e6910 == 0`.

## 9. Gaps I could not close
* `.rsrc` parsing (dialog captions/control ids, `IDR_MAINFRAME` default size, bitmap resources) — the `read` PE backend refused the path (`Path element starting with '.' is not permitted`) and no shell is available, so the "quick python pefile script" could not be run. Dialog IDs above are all recovered from `CDialog::CDialog`/`CDialog::Create` in the decomp, which is the same information, but captions are not.
* Semantics of hero offsets 0x1A5, 0x1A6, 0x2A8, 0x2FC and of the record regions 0x0AC8–0x0CDC, 0x0EEC–0x0FBC (0x5A/0x5B/0x5C were listed by offset only).
* The exact meaning of `DAT_00D831F8[0..32]` (the 33-int "position block" mirrored into hero+0x738); the first 8 entries are used as map-slot/recall indices by the world-editor map page (0x50BD8C), the rest were not cross-referenced. `[UNVERIFIED]`
* Exact text of the 12 `config.ini` keys read at map load (`FUN_0047C5C5` call sites pass the strings as raw addresses in the decompiled output).

## Sources

- `work/decomp/all.c`: Ghidra decompilation dump (138k lines) — every VA below is from this file
- `work/decomp/functions.tsv`: VA → name → size table used to bound functions
- `extracted/worlds/Evergreen/quest.txt`: World script: +MAPS (includes maps.txt), +STORY, +SCENES incl. `SCENE 0 temple, WELL, "Well of Souls", 2, 7`
- `extracted/worlds/Evergreen/maps.txt`: Map table: id,jpg,obl/ter root,display name,flags — map 0 = evergreen / "Evergreen", flags 131072
- `extracted/worlds/Evergreen/levels.txt`: Class/level table: 100,200,… class rows + DESCRIPTION/START_ABILITY/AUTO_MAX/DEFAULT_SKIN/START_LOCATION directives
- `extracted/worlds/Evergreen/gender.ini`: 4 genders [0]..[3], each with `menu=` and `adventurer=` skin name
- `extracted/worlds/Evergreen/config.ini`: [General] world overrides read at map load (startingGP=500, cookieProtection, pkTrophy, maxPKAttackAdvantage, …)
- `extracted/worlds/Evergreen/world.ini`: [world] name/version/wos=A96
- `extracted/Save/readme.txt`: Confirms Save\<WorldName>\ holds hero files, and that heroes cannot move between worlds
- `extracted/art/`: title.jpg, beg.jpg, where.jpg, chapter.jpg, death.jpg (front-end backgrounds) + button*.bmp (button bar) + pickSoulLabel.jpg, redButtons.bmp, shopBorder256.bmp
- `extracted/skins/`: Skin .bmp pairs (Foo.bmp / Foo2.bmp) named by levels.txt DEFAULT_SKIN and by gender.ini adventurer=
