# The oracle: running the original under Wine for differential replay

This is the spec both sides of the differential test obey: the **oracle** (the original
`extracted/Souls.exe` running under Wine with `tools/oracle/hook/hook.dll` injected) and
the **port** (`build/wos`, driven by Core's `--script`/`--dump`). Anything in this file
that disagrees with a run is a bug in the harness, not a difference in the game.

Everything below is cited by VA in the Ghidra dump `work/decomp/all.c` (ImageBase
`0x400000`) or by the PE import directory, which is authoritative over the decomp: the
decomp silently drops two `SetTimer` call sites that live inside functions Ghidra failed
to split.

---

## 1. What the oracle is

| piece | where | what |
|---|---|---|
| `tools/oracle/fetch_wine.sh` | new | idempotent, cached fetch of Wine + MFC42 + a copy of `extracted/` |
| `tools/oracle/build.sh` | new | builds the i686 hook DLL and the injector with the cached llvm-mingw |
| `tools/oracle/hook/hook.c` | new | the hook: virtual clock, LCG, timers, pump, script, dumps |
| `tools/oracle/hook/launcher.c` | new | `CreateProcess(SUSPENDED)` + `CreateRemoteThread(LoadLibraryA)` + `ResumeThread` |
| `tools/oracle/run.sh` | new | one script, one outdir, headless under `xvfb-run` |
| `tests/diff_oracle.sh` | new | runs every `tests/diff/*.dsc` on both sides and diffs per label |
| `tools/oracle/hero_mask.txt` | new | volatile byte ranges masked out of the hero hex comparison |

No file under `extracted/` or `assets/` is ever written. The game runs from a copy at
`/mnt/build/wos-oracle-run/`, re-staged from `/mnt/build/wos-oracle/` on every run, so a
stale `.her` or a warm `temp/sceneCache` can never make run *N* differ from run 1.

### 1.1 Wine

* `wine-11.18-staging-amd64-wow64.tar.xz`, Kron4ek/Wine-Builds release 11.18,
  sha256 `15f312effe798838d4877a37126509618b3f98759744b37aeda0c6f55875d99a`,
  unpacked to `/mnt/build/wine/wine-11.18-staging-amd64-wow64/`.
* Prefix `WINEARCH=win64` at `/mnt/build/wine-prefix`. New WoW64, so a 32-bit process
  resolves its DLLs from `drive_c/windows/syswow64`, **not** `system32` — that is where
  MFC42 has to go as well as in `system32`.
* MFC42 is not shipped in `extracted/` and Wine has no builtin for it. Source: the same
  `VC6RedistSetup_deu.exe` winetricks' `vcrun6` verb uses,
  `https://download.microsoft.com/download/vc60pro/Update/2/W9XNT4/EN-US/VC6RedistSetup_deu.exe`,
  sha256 `c2eb91d9c4448d50e46a32fecbcc3b418706d002beab9b5f4981de552098cee7`. It is a
  self-extracting archive; the inner `vcredist.exe` is a PE carrying a CAB in `RCDATA`,
  which `7z` reads directly, so no Wine and no `cabextract` is needed. Only
  `mfc42.dll`, `mfc42u.dll` and `msvcp60.dll` are installed. **The `msvcrt.dll`,
  `oleaut32.dll` and `msvcirt.dll` in the same CAB are deliberately not installed**:
  Wine's own builds are what the rest of the prefix expects.
* Audio: no ALSA device exists on this host. The game plays `MIDI`/`MIX`/`WAV` through
  `mciSendStringA`, `mmio*` and `waveIn*`; those calls fail harmlessly under Wine and the
  game does not check their results for anything the rules depend on. Output is filtered
  rather than "fixed", because there is nothing to fix.

### 1.2 The solo/offline path

`SRNet.dll` is present in `extracted/` and is loaded, but nothing blocks. In the solo
channel `DAT_004e6910 = _SRNGetNetworkType_0() == 0` (boot_flow.md §1), and there is no
"Play Solo" button to press: offline is simply the absence of a network type. On boot
under Wine the game reaches the main menu, writes `temp/sceneCache/948_art_title.jpg`
and `948_art_beg.jpg` (proving it drew the title and the menu) and then sits in the
message loop. No dialog, no network timeout, no `ShellExecute` that blocks. The only
`ShellExecute` is the `tos.rtf` terms-of-service, which the game shows as a
`SW_SHOWDEFAULT` launch and whose result it does not gate on.

---

## 2. How the hooks are installed

The exe's import table has **no** `GetMessageA`, `DispatchMessageA`, `TranslateMessage`
and **no** `timeGetTime`. The message pump lives inside MFC42.DLL (`AfxWinMain`,
`CWinApp::Run`'s MFC cousins, `CWnd::RunModalLoop`), and the game overrides
`CWinApp::Run` itself. Patching `Souls.exe`'s IAT would therefore miss the one function
that drives everything.

So each hook is installed on the **module**, at the export, not on the IAT. Every hooked
function keeps the exact prototype of the original, which means the hook is an ordinary
`__stdcall`/`__cdecl` function and the installed trampoline is six bytes:

```
68 <imm32>   push imm32
C3           ret
```

The original prologue is never executed, so there is no length disassembler, no copied
bytes and no relocation fixups. The module's code section is `VirtualProtect`ed to
`PAGE_EXECUTE_READWRITE` first, which under Wine forces a private copy. `GetProcAddress`
is resolved to the real `PeekMessageA`/`GetMessageA` **before** they are overwritten, and
the hook tail-calls those pointers for every call it does not own.

`launcher.exe` creates the target with `CREATE_SUSPENDED`. At that point the loader has
already mapped every module and resolved every import, so the hook's `DllMain` can detour
`kernel32`, `user32` and `msvcrt` and is fully armed before `Souls.exe` executes a single
instruction of its entry point.

### 2.1 What is hooked

| module | export | replacement |
|---|---|---|
| `user32` | `PeekMessageA`/`W`, `GetMessageA`/`W` | `pump_step()` then the real function |
| `user32` | `SetTimer`, `KillTimer` | the virtual timer table |
| `user32` | `GetKeyState`, `GetAsyncKeyState` | always 0: no key or button is ever held across a step |
| `user32` | `WinHelpA`/`W` | no-op, so `ExitInstance` (0x00409368) cannot open the help file and block |
| `kernel32` | `GetTickCount`, `GetTickCount64` | the virtual millisecond clock |
| `kernel32` | `GetSystemTime`, `GetLocalTime` | the virtual wall clock, UTC |
| `msvcrt` | `rand`, `_rand` | the MSVC6 LCG, counted |
| `msvcrt` | `srand`, `_srand` | the LCG seed |
| `msvcrt` | `time`, `_time64` | `VIRTUAL_EPOCH + clock_ms/1000` |
| `msvcrt` | `localtime`, `_localtime64`, `ctime`, `_ctime64`, `mktime`, `_mktime64` | the same instant, civil math done in-process so the host locale and leap-second table cannot leak in |

`VIRTUAL_EPOCH` is **1234567890** (2009-02-13 23:31:30 UTC), fixed. `TZ=UTC` is exported by
`run.sh`. Both sides must use this epoch, because the incarnation time is written into the
hero record and therefore into the `.her` checksum.

---

## 3. The event loop, and the step rule

The game's loop is the `CWinApp::Run` override at **0x0040A8D9**:
```
for (;;) {
    quit = FUN_00449a49();
    /* IDLE: runs only while the queue is empty */
    do {
        if (PeekMessageA(&msg,NULL,0,0,PM_NOREMOVE)) break;
        OnIdle(nCount++);
        if (FUN_0040a7c7() && FUN_00467312(0x1a)) break;
        ... focus tracking ...
    } while (foreground);
    /* MESSAGES */
    do {
        ... 0x101/0x200/0x201/0x204 bookkeeping, the >1 s quit watchdog ...
        if (quit) PostQuitMessage(0);
        if (!IsIdleMessage(&msg)) { ExitInstance(); return; }
        if (PreTranslateMessage(&msg)) nCount = 0;
    } while (PeekMessageA(&msg,NULL,0,0,PM_REMOVE));
}
```

There is **no 60 Hz game tick and no per-frame simulation.** The per-frame work is
`FUN_0040A7C7` (0x0040A7C7), which fires only when
`GetTickCount() - _DAT_004dd510 > 19` and then calls `FUN_0042895c` (netgraph, which
itself has a 25 ms gate and an unconditional `rand()`), `FUN_00416cd3`, `FUN_00401f8d`
and `FUN_0047397b(1)`.

### 3.1 The step rule

`pump_step()` runs from inside the pump hooks, and **only** when the queue is empty. It is
the single place the virtual clock is allowed to move.

```
T = min( t_input      next .dsc event's virtual ms,
         t_timer      earliest live timer deadline,
         t_20hz       the 20 Hz boundary,
         t_end        the script's `end` ms )

advance the virtual clock to T
then post AT MOST ONE message:
    the input, if t_input <= T          <- input wins ties against a timer
    else the earliest due WM_TIMER
return to the real PeekMessage, which now has something to hand the app
```

When `T == now` (something is already due) the clock does not move and the loop simply
runs another iteration. When no `.dsc` event and no live timer remain, the clock is
advanced to `end` and then `WM_QUIT` is posted once.

`t_20hz` is **read out of the original**: `_DAT_004dd510` is at VA **0x004DD510**, and
`t_20hz = _DAT_004dd510 + 20`. The harness therefore cannot drift from the game, and it
does not hardcode a second copy of the constant. If that global ever reads greater than
the current clock (uninitialised, or the build moved), the harness falls back to
`now + 20` and says so in the log.

**This is the rule Core implements in the port's script loop** (`clock_20hz_next()` plus
`clock_idle_due()`, which re-arms from the *boundary*, not from the observed `now`). The
consequence both sides must honour: over *N* ms of idle the 20 Hz work runs exactly
`floor(N/20)` times. It is *not* one tick per script event — a 1000 ms idle gap is 50
ticks on both sides, because the live original spins the idle loop that many times too.

### 3.2 Ordering at equal timestamps

1. **Input before timer.** An input scheduled at `T` and a timer that falls due at `T`:
   the input is dispatched first, and the timer fires on the very next idle step, at the
   same clock value. This matches the port's `input -> clock_dispatch_timers() -> idle`.
2. **A `.dsc` event is atomic.** `click` is `WM_MOUSEMOVE` + `WM_LBUTTONDOWN` +
   `WM_LBUTTONUP` in one step; `key` is `WM_KEYDOWN` + `WM_KEYUP` in one step. Nothing is
   ever half-pressed across a step, which is why `GetKeyState` reporting 0 is right.
3. **Events at the same `t` fire in file order.**

### 3.3 Timers

There are **eight** `SetTimer` call sites in `Souls.exe` (from the PE import directory;
the decomp shows only six). Every one belongs to a dialog, **none to the game world**:

| id | ms | call VA | function | what |
|---|---|---|---|---|
| 0x16 | 100 | 0x428803 | `FUN_00428360` | calls `FUN_004094e7` first; timer slot `this+0x114` |
| 2 | 100 | 0x448c0e | `FUN_004483a6` | the BIO_INFO / serial-registration dialog; slot `+0x1fac` |
| 4 | 1000 | 0x44931f | `FUN_00449144` | the five-row chooser dialog; slot `+0x140` |
| 2 | 100 | 0x455f6d | `FUN_00455f56` | WoS Tactics dialog `OnInitDialog`; slot `+0x60` |
| 2 | 1000 | 0x4722a7 | `FUN_00472292` | the "LagOMeter"; slot `+0x60`, killed at 0x4722d1 |
| 2 | 50 | 0x47430e | `FUN_004742f0` | `OnInitDialog` = `FUN_004742da`; slot `+0x60` |
| 2 | 100 | 0x49f02d | `FUN_0049ef7b` | the map/object editor; slot `+0xb8` |
| 4 | 1000 | 0x4a1e18 | `FUN_004a1b52` | slot `+0x68`, killed at 0x4a1e40 |

So map walking, scene playback, battle rounds and animation ride the 20 Hz idle gate or a
stored-tick comparison, never a `WM_TIMER`. **A module with no timer and no message
handler in the original does not tick at all in replay.**

Coalescing, as implemented on both sides:

* `SetTimer` → `deadline = now + interval`.
* When the deadline is reached and no `WM_TIMER` for that timer is outstanding, post
  **one** `WM_TIMER` and leave the deadline unchanged.
* When the app *takes* that `WM_TIMER`, `deadline += interval`.

An overdue timer therefore walks forward one interval per message and never replays a
backlog. Re-arming is from the **deadline**, not from the dispatch instant; the two
readings differ by exactly the coalesced backlog, which is the kind of thing that shows
up as a three-second drift in a four-second spell.

---

## 4. The `.dsc` diff script

One statement per line, `#` or `;` starts a comment, blank lines ignored, tokens
whitespace-separated. Times are **virtual milliseconds from the start of the pumped
message loop**, i.e. from the first idle pass, not from process creation. `end <ms>` is
optional; without it the clock runs until the last event and the harness stops.

| statement | meaning |
|---|---|
| `at <ms> click <x> <y>` | `WM_MOUSEMOVE` + `WM_LBUTTONDOWN` + `WM_LBUTTONUP` |
| `at <ms> rclick <x> <y>` | `WM_MOUSEMOVE` + `WM_RBUTTONDOWN` + `WM_RBUTTONUP` |
| `at <ms> down <x> <y>` | `WM_MOUSEMOVE` + `WM_LBUTTONDOWN` only |
| `at <ms> up <x> <y>` | `WM_MOUSEMOVE` + `WM_LBUTTONUP` only |
| `at <ms> move <x> <y>` | `WM_MOUSEMOVE` only |
| `at <ms> key <VK>` | `WM_KEYDOWN` + `WM_KEYUP` to the focus window |
| `at <ms> text <string...>` | one `WM_CHAR` per character to the focus window; a newline becomes `RETURN` |
| `at <ms> dialog <spec>... [ok\|cancel]` | see below |
| `at <ms> dump <label>` | write `<outdir>/<label>.txt` and `<outdir>/<label>.bmp` |
| `end <ms>` | stop the clock here, then quit |

`<x> <y>` are **client coordinates of a 640x480 client area**; `run.sh` forces the main
window's client area to exactly 640x480 (growing the window until the *client* area
matches, because the frame has a menu bar) so both sides share coordinates. The message
goes to the deepest visible window at that point, exactly as a real click would; the
real cursor is moved there too, so `GetCursorPos` callers agree.

`key` accepts `A`..`Z`, `0`..`9`, bare names (`RETURN`, `ESC`, `UP`, `F1`, …) and the
`VK_` prefixed spellings. The port's panels accept the same.

### 4.1 `dialog`

`at <ms> dialog <ctlid>=<value> [<ctlid>=<value>...] [ok|cancel]`

The dialog is identified by **an identifying control id**: the harness waits until a
top-level window of the process contains a child with that id, and operates on it. This
is deliberately semantic — it does not depend on a dialog resource number, on the title
text, or on the order the controls were created in.

| value | action |
|---|---|
| `<text>` | `WM_SETTEXT` |
| `sel:<n>` | `CB_SETCURSEL`, falling back to `LB_SETCURSEL` if the combo rejects it |
| `check:<0\|1>` | `BM_SETCHECK` |
| `click` | `BM_CLICK` |
| `focus` | `SetFocus` |
| *(no `=`, and it is `ok` or `cancel`)* | `BM_CLICK` on `IDOK` (1) or `IDCANCEL` (2) |

A `dialog` op whose window is not up yet **does not advance the clock past the next 20 Hz
boundary**; the event stays pending and is retried on the next idle step, until `end`.
Dialogs are created asynchronously, so this is the normal path, not an error.

The port's in-framebuffer panels accept the same statements, by control id.

---

## 5. The dump key registry

`dump <label>` writes `<outdir>/<label>.txt`, `key=value` lines, UTF-8, LF. The
**port writes the same keys from `--dump`**. Both sides are compared per label.

### 5.1 Harness keys

| key | meaning |
|---|---|
| `label` | the label from the `dump` statement |
| `oracle.seq` | dump index within the run (oracle only) |
| `clock.ms` | the virtual millisecond clock, `GetTickCount` |
| `clock.time_s` | `VIRTUAL_EPOCH + clock.ms/1000` |
| `clock.timers_live` | number of live virtual timers |
| `clock.next_deadline` | earliest live timer deadline, `4294967295` if none |
| `rng.state` | the LCG's `holdrand` |
| `rng.calls` | number of `rand()` calls so far |
| `rng.srand_calls` | number of `srand()` calls so far |

`rng.state`, `rng.calls` and `rng.srand_calls` are the definitive check that both sides
consume the LCG at the same points in the same order. A mismatch here invalidates every
other key in the same dump, so `tests/diff_oracle.sh` reports it first.

### 5.2 Front-end globals (read straight out of the image)

| key | VA | meaning |
|---|---|---|
| `front.state` | 0x004DF8A4 | `DAT_004df8a4`, the front-end state machine (`FUN_0041b891`) |
| `front.serial` | 0x004DD20C | `DAT_004dd20c`, the serial / player id |
| `front.network_type` | 0x004E6910 | `DAT_004e6910`, the SRNet network type; 0 = solo |
| `front.map` | 0x004E0DDC | `DAT_004e0ddc`, the current map number |
| `front.world` | 0x004E0BD0 | `DAT_004e0bd0`, the current world name |

### 5.3 The hero record

`hero = DAT_0067FBF8`, a 100-slot table of `0x16CC`-byte records, slot 0 being the
incarnated hero. The dump writes the individual fields that matter for rules, and then
the whole record as hex, 64 bytes per line, `hero.hex.<offset in hex> = <bytes in hex>`.

| key | byte offset | meaning |
|---|---|---|
| `hero.inuse` | 0x0000 | 1 alive, 2 ghost, 4 loaded-but-not-incarnated |
| `hero.serial` | 0x0004 | player id |
| `hero.name` | 0x0014 | soul name, 32 bytes |
| `hero.level` | 0x0064 | level index |
| `hero.map` | 0x0090 | current map index |
| `hero.gender` | 0x01A8 | 0..3 |
| `hero.link` | 0x067C | current link index |
| `hero.hp` | 0x06BC | 8 bytes, current HP, fixed point |
| `hero.mp` | 0x06C0 | 4 bytes, current MP |
| `hero.invocations` | 0x01CC | incarnation count |
| `hero.seconds` | 0x01CD | seconds played |
| `hero.checksum` | 0x16C8 | the record checksum, `FUN_00416ABB` |
| `hero.hex.XXXX` | — | the full 0x16CC record, 64 bytes per line |

The hex record is masked with `tools/oracle/hero_mask.txt` before comparison, because it
contains RAM-only pointers, the wall-clock incarnation timestamps and the mood fields
that legitimately differ. Named fields are always compared unmasked; the mask only
relaxes the raw hex.

### 5.4 Module dumps

Modules emit `<module>_dump(DumpEmit, void*)` with flat `key=value` lines under their own
namespaces — `battle.*`, `scene.*`, `map.*` — from the same registry. Their key lists are
maintained here as the owning agents land them; the harness passes them through unchanged.

*(to be completed from `work/decomp/all.c` as the module owners land their dumps — the
front-end, map, scene, battle, shop, quest, minigame and training globals.)*

### 5.5 Screenshots

Every `dump` also writes `<outdir>/<label>.bmp`, an uncompressed 24-bit bottom-up BMP of
the 640x480 client area, for debugging only. **It is never compared**: presentation is
explicitly out of scope for parity. Before capturing, the harness issues one synchronous
`RedrawWindow(RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN)`, because Xvfb has no window
manager and nothing would otherwise force a repaint. That extra `WM_PAINT` is part of the
dump step on both sides, so it cannot make a run non-deterministic.

---

## 6. Running it

```sh
tools/oracle/fetch_wine.sh                       # once; idempotent
tools/oracle/build.sh                            # i686 hook + launcher
tools/oracle/run.sh tests/diff/boot_newsoul.dsc /tmp/oracle
WOS_RANDTRACE=1 tools/oracle/run.sh tests/diff/boot_newsoul.dsc /tmp/oracle-trace
tests/diff_oracle.sh                             # both sides, per-label diff
```

`run.sh` restages the game directory every time, so it is safe to run concurrently with
other work. `WOS_ORACLE_TIMEOUT` (default 180 s) bounds the target; the launcher kills it
at the timeout. `WOS_RANDTRACE=1` writes `randtrace.txt` into the working directory, one
`call_index return_address` pair per `rand()`, which is how a call-site mismatch in
`docs/re/rng_calls.md` is pinned to a VA.

---

## 7. Known gaps and honest limits

* The registry below `front.*` / `hero.*` is filled in from the decomp; the module dump
  keys (`battle.*`, `scene.*`, `map.*`) are added as the owners land them. Until then
  `tests/diff_oracle.sh` compares what exists and says what it did not compare.
* `.rsrc` is still unparsed, so dialog control ids come from `FUN_00460765`/`OnInitDialog`
  and the vtables rather than from the resource tree. They are the same information, but
  a caption read off a bitmap is not a caption read off the resource.
* Two runs being byte-identical proves the *harness* is deterministic. It does not by
  itself prove the game is: a key that happens to be constant across a run will also be
  identical twice. The scripts therefore dump after every step that is supposed to change
  something.

## Dialog ids (Core, 2026-09-28)

Control ids read out of the PE with pefile's `DIRECTORY_ENTRY_RESOURCE`, RT_DIALOG
(68 templates). Method: for each template, scan for the `ff ff <cls>` DLGITEMTEMPLATEEX
marker and take the dword four bytes before it as the control id. This is exact for
DLGTEMPLATEEX dialogs and unreliable for plain DLGTEMPLATE ones, which is why the
lists below are the ones that came out clean; the rest still need a proper walk.

**Decoded so far. Use these verbatim in `.dsc` scripts and in the Wine harness.**

| dialog | resource id | controls |
|---|---|---|
| **New Soul** | **138 (0x8A)** | `EDIT 1043` (name), `LISTBOX 1063` (class), `BUTTON 1064` (PK), `COMBOBOX 1230` (gender), `BUTTON 1` = IDOK, `BUTTON 2` = IDCANCEL, `STATIC 1086 / 1087 / 1088 / 1089 / 1126` (labels, 1126 is the radio group) |
| 50 ms minigame/animation | 189 (0xBD) | `BUTTON 1`, `BUTTON 2`, `BUTTON 1007`, `BUTTON 1134`, `BUTTON 1135` |
| items / equipment / shop grid | 170 (0xAA) | `EDIT 1098 1099 1100 1102 1103 1104 1105 1119`, `BUTTON 1007 1108 1109 1179 1 2`, `STATIC 1022 1116 1178 1228` |
| list dialog | 177 (0xB1) | `LISTBOX 1090`, `BUTTON 1`, `BUTTON 2` |
| list dialog | 161 (0xA1) | `BUTTON 1085`, `BUTTON 1216`, `BUTTON 1`, `BUTTON 2` |
| single-field dialog | 180 (0xB4) | `EDIT 1234`, `BUTTON 1235 1145 1007 1`, `STATIC 1051 1153 1191` |
| simple confirm | 178 (0xB2) | `STATIC 1051`, `BUTTON 1`, `BUTTON 2` |
| simple confirm | 147 (0x93) | `STATIC 1059`, `BUTTON 1`, `BUTTON 2` |
| simple confirm | 148 (0x94) | `STATIC 1061`, `BUTTON 1`, `BUTTON 2` |

This **confirms FrontHero-2's member-offset reading** of dialog 138: the class list is
the LISTBOX (1063), the gender combo is the COMBOBOX (1230) and the PK checkbox is the
BUTTON (1064), which is exactly the `+0x2AC` / `+0xAC` / `+0x30C` members they cited.

### The options dialog: NOT yet decoded

`FUN_00466EF3` (0x00466EF3) is the option **loader**, not the dialog. The dialog is the
class whose handler is `FUN_004673ef` (0x004673EF, `CListCtrl::InsertItem` per option)
and whose checkbox handler is `FUN_00467349` (0x00467349, flips
`DAT_006840D0[(&DAT_004F2A58)[row*3]]`, and special-cases row ids 0 and 0x20 by calling
`FUN_0042275d` / `FUN_00422789`). Its `CDialog::CDialog(param_1, <id>, param_2)` resource
id is **not yet identified**; the ctor list in the decompilation does not contain a match
for the class that owns `FUN_004673EF`, which is consistent with Ghidra having split that
class across functions. Until it is, a `.dsc` cannot open the options dialog by id, and
`src/game/options.c`'s `options_dialog_op()` only accepts a control literally named
`option <n>`, which is **not** an original id. Under the orchestrator's rule that the
port carries no path the original does not, that named form is a stopgap: it must be
replaced by the real resource id and checkbox ids once the dialog is found. Do not build
a differential expectation on it.

### The user-options array is never loaded from the profile

Established by scanning the image for the little-endian encodings of both addresses:
`58 2a 4f 00` (0x4F2A58) has 4 occurrences, at VA 0x466F07, 0x4670C8, 0x46737E, 0x467413;
`d8 2b 4f 00` (0x4F2BD8) has 6, at 0x466F00, 0x466F48, 0x4670C1, 0x467106, 0x46740C,
0x467458. **Every one is a read** (`cmp %esi,0x4f2bd8`, `mov $0x4f2a58,%ebx`,
`mov 0x4f2a58(%eax),%ebx`); there is no store to either anywhere in `.text`, and both
sit in the zero-filled tail of `.data` (raw data ends at RVA 0xE0C00; these are at RVA
0xF2A58 and 0xF2BD8). So both are 0 for the whole life of the process.

`FUN_00466EF3` guards its numbered-option load with `if (0 < DAT_004f2bd8)`, so **the
loop never runs** and the original never reads `option N` from the profile.
`DAT_006840D0` therefore starts entirely zero, every numbered option is OFF in retail,
and the only writer is `FUN_0046732B` (0x0046732B) from the dialog's checkbox handler.

**Harness consequence: the Wine prefix must not contain any `option N` key, and the port
must not read one either.** Seeding `option 7=1` in the registry to make a replay's
pathfinder run would be testing a state the original cannot be put into from a file. The
only faithful way is to drive the dialog op, i.e. click the checkbox.

The nine **named** scalars *are* read, each by its own unguarded
`GetProfileIntA("Preferences", <key>, <default>)` in `FUN_00466EF3`:

    worldLocation=0   seanceInProgress=0   enableMusic=1   enableEnvironmentalSounds=1
    enableSFX=1       enableSoundCard=1    askForSkins=1   enableHowDoYou=1  eavesdropEnabled=1

### MFC registry location

`CWinApp::SetRegistryKey(local_14, s_Synthetic_Reality_004dd690)` at all.c:5850, in
InitInstance (`FUN_00408acf` region, the 0x408C00 step), immediately before the call to
`FUN_00466EF3`. The string at **VA 0x004DD690** is `"Synthetic Reality"`.

`m_pszAppName` comes from the string table: **string id 57344 = `AFX_IDS_APP_TITLE` =
`"Souls"`** (RT_STRING, block 3585). No `SetProfileName` call, so the profile is the
REGISTRY, not an INI.

    HKEY_CURRENT_USER\Software\Synthetic Reality\Souls\Preferences

That is the complete path, with the section name `Preferences` (`s_Preferences_004dd668`).
The same section also holds, read in the same InitInstance block: `bleeper`=1,
`dunceMute`=0, `shoutMute`=0, `heartEavesdrop`=0, `myCommChannel`=0,
`languageMuteLevel`=0, plus `Debug`/`TraceMask` (`DAT_004e70d0 = atoi(...)`).

RT_STRING 128 (`AFX_IDS_APP_TITLE` neighbours) also gives the document classes the port
should mirror: `"Souls"`, `"Souls.Document"`, `"Souls Document"`, matching
`CSingleDocTemplate(..., CSoulsDoc, CMainFrame, CSoulsView)` at all.c:5844.
