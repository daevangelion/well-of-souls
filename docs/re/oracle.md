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

## 0. Status, and the one thing that still blocks a run (measured 2026-09-28)

`tools/oracle/run.sh <dsc> <outdir>` still produces **no dumps**. Everything below is
from live runs of the injected original, with the evidence in the log lines quoted.

**What now works** (all measured, not inferred):

* The hook loads, and 59 IAT patches land across `Souls.exe`, `MFC42.DLL`
  (0x5F400000), `SHELL32.dll` (0x7A820000), `COMCTL32.dll`, `msvcrt.dll` and
  `SRNET.dll` (0x10000000 — it imports `rand`, `time`, `GetTickCount` and
  `MessageBoxA` too, so an SRNet-only call site is *not* evidence about the game).
  The module list is dumped by `log_modules()` at every start.
* The MSVC6 LCG is intercepted and counted (`rng=1419` calls by the time the game
  reaches its first frame).
* `WOS_DETOURS` now selects groups; the default is the core set
  (`pump,timer,key,tick,wall,clock,rng,help,modal,watch`). `probe` (the live
  options-table dump) and `trace` (failing `_access`/`fopen`/`CreateFileA`, now
  with the caller's VA) are opt-in, because they cost seconds of wall time per run.
* **Wine's `DialogBox` is not the game's loop, and that was the first blocker.**
  Under Wine the modal loop of a dialog lives *inside* user32 and is entered on
  win32u's `NtUserGetMessage`; no import slot of any game module is on that path,
  so IAT patching cannot see it. Worse, MFC42 does not even import
  `DialogBoxIndirectParamA` — its 195 user32 imports contain
  `CreateDialogIndirectParamA` + `EndDialog` + `GetMessageA` and *no*
  `DialogBox*`. The hook now owns the loop: `DialogBoxParam[IndirectParam][AW]`
  and `EndDialog` are replaced with the same call Win32 makes (disable owner,
  `CreateDialogIndirectParam`, `GetMessage`/`IsDialogMessage`/`Translate`/
  `Dispatch` until `EndDialog` or `WM_QUIT`), with `pump_step()` first on every
  iteration so the virtual clock still only moves while the queue is empty. It
  also reads the live `DLGTEMPLATE`/`DLGTEMPLATEEX` and logs the resource id, the
  caption and every control id, which is a better source for `.dsc` `dialog` ops
  than the `.rsrc` scan.
* Message boxes are logged verbatim and answered `IDOK`. A headless run cannot
  survive one, and the text is the evidence.
* `run.sh` no longer tries to `rm` a `Z:\` path (it could never work, which is why
  the hook log looked stale), and it prints the tail of the hook log when a run
  produces nothing.

**What is still blocking, precisely:**

1. At boot, before the first frame, the game puts up
   `MessageBoxA(owner=00010374, type=0x30) caption="Souls" text="Z:\...\wos-oracle-run\<N> was not found."`,
   where **N is the elapsed run time in seconds** (15, 20, 30, 40, 45, 60, 90, 240
   and 300 have all been observed, each in the run whose timeout was that many
   seconds). The file open is `CreateFileA(..., 0x80000000)` (a *read*), issued
   from **MFC42+0x10040B, which is `CFile::Open`**, from an MFC wrapper that first
   does `GetModuleFileNameA` + `strrchr('\\')` to resolve the name against the
   install root. Nothing named `<number>` exists in a retail install directory,
   so this is a duration-named file the game expects to read, and it is the last
   thing the game does before it stops.
2. After that box is answered, the game **never reaches its message loop**: the
   watchdog thread reports `steps=0 peeks=0 gets=0 modal=0 ready=0` for the whole
   run. No `PeekMessageA`, no `GetMessageA`, and not even one `GetTickCount` call
   reaches a hook, although all of those IAT slots are patched in `Souls.exe`
   itself. The main thread is asleep in one syscall (`/proc/<pid>/task/*/wchan` =
   `anon_pipe_read`, `State: S`, one thread, 0% CPU) and the `+relay` trace shows
   its last activity as `NtUserMessageCall`/`NtUserPeekMessage`/`NtUserGetMessage`
   called from inside the 64-bit user32 — i.e. it is parked in a Wine-internal
   wait that no IAT patch can reach, and the harness is not the thing driving it.
3. The `MessageBoxA` call chain, filtered to real return addresses
   (`looks_like_ret`), is only two frames deep: `5F409CCB` (MFC42) and
   `7BF1C628` (ntdll). **There is no game frame above it**, so the box is raised
   by MFC on behalf of a caller that has already returned, or by a path that never
   comes from `Souls.exe` at all — `SRNET.dll` imports `MessageBoxA` and is the
   remaining suspect. That is the next thing to settle.

The practical consequence for the diff scripts: `tests/diff/*.dsc` and
`tests/diff_oracle.sh` are **not** written, because a script that cannot get past
boot cannot produce a label to compare, and inventing twelve scripts against a
harness that emits nothing would be twelve untested guesses. `cmp_dump.py` also
still compares the oracle's `hero.hex.XXXX` form against the port's
`hero.record`; the port emits the whole 0x16CC record as one lowercase hex key
(`src/game/hero.c:1001`), so the oracle must emit `hero.record` in that exact form
and keep the 64-byte lines as a debug extra.

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

### 3.4 The measured pump model at the title screen

Everything in this section is measured, not inferred. `WOS_MSGLOG=1` (with
`WOS_MSGLOG_MS=<virtual ms>` to bound it) makes the hook log **every** message the app's
own pump takes — id, target window, window class, the remove flag — plus every
`PostMessageA`/`SendMessageA`/`SetCursorPos` with the caller's return address, and every
move of the 20 Hz stamp.

**The app's loop is a Peek/Get pair, not a PeekMessage idle loop.** The stream is
literally `Peek(PM_NOREMOVE)` then `Get(PM_REMOVE)` for every message, and
`PeekMessage` **never returns FALSE** — not once in a whole run. The idle counter in the
log stays at 0 from the first step to `WM_QUIT`.

That is the answer to "why does the 20 ms gate stamp lag". It is not that the queue is
busy: it is that **the original has no PeekMessage-based idle loop on this path at all**,
so the queue being empty is never *observed*. `FUN_0040A7C7` is entered from an idle
handler, the idle handler is only reached from a `PeekMessage` that returned FALSE, and
that never happens. The live proof is in the log itself: with `WOS_MSGLOG=1` the
`gate=` field the pump prints is **`00000000` for every step of the run**. `_DAT_004DD510`
is never written once. The stamp does not lag the clock; it never moves, and the 20 ms
work runs once per message the app happens to be given, not once per 20 ms of time.

**The messages that exist at the title**, in the order they arrive, with no input at all
(`at 0 dump clock` / `end 1500`):

| virtual ms | id | window | class | what |
|---|---|---|---|---|
| 0 | `0x036A` | `00010070` | `Afx:400000:b:1004e:6:1005a` "Well of Souls" | MFC42 private message, `SendMessage` from `0x5F40DF1A` |
| 20 | `0x0362` | `00010070` | same | MFC42 private message, `wParam=0xE001` |
| 60 | `0x0200` `WM_MOUSEMOVE` | `0001007A` | `AfxFrameOrView42` | one real pointer motion, `lParam=0x00ED017A` (493,378) |
| 100 | `0x0113` `WM_TIMER` | `000101AE` | `#32770` "Book of Tactics" | **live timer, id 2, 100 ms** |
| 1500 | `0x0113` `WM_TIMER` | `00010070` | main frame | **live timer, id 0x16, 100 ms** |
| 1500 | `0x0012` `WM_QUIT` | — | — | posted by the harness at `end` |

`oracle.timers_live=2` and `oracle.next_deadline=100` in every dump, which is the same
fact from the other side. **The two live 100 ms timers are the ones the front end arms at
boot, and they belong to `FUN_0040A8D9`'s owner, i.e. Core/FrontHero-2 — not to any
module:**

* id `0x16` (22), 100 ms, on the **main frame** `00010070`, armed by `SetTimer` at
  **`0x00428803`** in `FUN_00428360` (which calls `FUN_004094E7` first; the timer slot is
  `this+0x114`).
* id `2`, 100 ms, on the **"Book of Tactics" dialog** `000101AE` (class `#32770`), armed
  by `SetTimer` at **`0x00455F6D`** in `FUN_00455F56`, the WoS Tactics dialog's
  `OnInitDialog` (slot `this+0x60`).

**Is the model real Windows or a harness artefact?** It is real, with one caveat that
does not change the model. The `0x036A`/`0x0362` pair and the `Peek`-then-`Get` shape are
MFC42 and Wine, not Wine-specific behaviour: MFC's `CWinApp::PumpMessage` tests with
`PeekMessage(PM_NOREMOVE)` and then takes with `GetMessage`, and it returns FALSE from
`PumpMessage` only when the *test* fails — and under this path the test never fails
because nothing in the loop ever lets the queue be observed empty. The one message that
is genuinely the harness's is the single `WM_MOUSEMOVE` at (493,378): it is posted by
Wine from the X11 pointer, and it happens once. The port must not model it as a
repeating event.

**What the port therefore implements.** At the title, in one virtual second: two MFC
private messages, one real mouse move, and then nothing until the two 100 ms timers fire.
The 20 ms gate work runs **once per message the pump delivers**, never on a wall-clock
schedule, and `_DAT_004DD510` stays at 0. `rng.calls` is therefore a function of the
message count, and the port's `clock_20hz_next()` / `clock_idle_due()` must be driven by
the same thing — a delivered-message counter — not by elapsed virtual time. Over *N* ms
of a *quiet* title the original delivers 0 messages, so the correct answer is 0 ticks,
not `floor(N/20)`.


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
| `at <ms> dump <module>` | write `<outdir>/<module>.txt` and `<outdir>/<module>.bmp` |
| `at <ms> dump <module>@<tag>` | the TAGGED form: same keys, written to `<outdir>/<module>@<tag>.txt` / `.bmp`, with `label=<module>@<tag>` |

The tagged form exists because a script may need to checkpoint the same module several times in
one run. Without a tag the file names collide, the later dump overwrites the earlier one, and the
diff compares the last occurrence while appearing to succeed — the failure mode where a regression
is invisible precisely because the check ran. The split is at the **last** `@`; the module is the
prefix and must be one of the entries in `src/game_main.c`'s dump table; the tag is
`[A-Za-z0-9_.-]{1,32}` and cannot contain `@`. The emitted `label` key is the whole token, so one
string names both the file and the event, which is what `tests/diff_oracle.sh` matches on. The
oracle also emits `oracle.tag=<tag>` for a human reading the file. `dump <module>` with no `@` is
unchanged and is not a second convention.
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

### 5.4.1 The first module key with an oracle source

`battle.spell_success_percent`, read from **`_DAT_004E0FF8`** in the running original.
Live value measured in a boot run: **100**, which is the "no scaling" case — the original's
consumer `FUN_004A7456` compares it against 100 at `0x4A750C` and skips the multiply.

It is registered here rather than left implicit because it is the first key of a shape the
rest of the module dumps will have: **a value the original holds constant during a fight but
the port applies dynamically.** The original has per-spell fizzle flags at
`spell[0x124]/[0x128]`; the port's `SpellDef` has no columns for them and substitutes the
global. So a `.dsc` that casts a spell compares equal on the hero record, the world dump
and `rng.state`, and can still diverge in the roll. Emitting the value on the battle label
is what makes that divergence visible *at the moment it happens* instead of arriving later
as an unexplained `cast_success` difference that reads as replay flakiness.

**Do not resolve it by ignoring the value in `cast_success()`.** A silent constant in place
of a modelled field is the same defect with the evidence removed. Port the two flag columns
or record the substitution in `docs/architecture_port.md`'s deliberate-deviations table;
owner is the spell panel, not the oracle.
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

The key lists below are the ones the hook can actually read out of the running original,
with the address each is read from. **A key the port emits that is not in this table is
PORT-ONLY**: the original has no counterpart for it, and the reason is given. That is a
different statement from "not implemented yet", and it is the honest one to make, because
giving a port-only key a plausible-looking original value is how a modelling gap survives
a diff.

#### 5.4.0 Two indirections, both of which are easy to read wrong

* **`DAT_0067FBF8` is a POINTER to the hero table, not the table.** `FUN_004269AF` at
  `0x426B9A` does `DAT_0067fbf8 = DAT_004e4870 + 0x1560a5c`, where `DAT_004e4870` is a
  `malloc(0x15C0170)` block plus `(rand() % 0x3FFF & ~0xF)`. Every record-relative address
  is therefore `*(u32*)0x0067FBF8 + offset` — **dereference once, then add**. Reading the
  pointer's own address as the record is the mistake that produced a `hero.record` full of
  zeros. `hero.base_offset` is that per-run offset and is a real parity key, because it is
  that `rand()` draw: a mismatch there means the boot LCG stream diverged even when
  `rng.calls` happens to agree.
* **`DAT_004E4874` is a POINTER to the scene block**, re-pointed to a fresh random
  16-aligned offset inside a `malloc(0x7C140)` block on every scene load (`0x426BE0`).

#### 5.4.1 `map.*` — the walk state is in hero record 0

| key | read from | note |
|---|---|---|
| `map.fx`, `map.fy` | `hero+0x94`, `+0x98` | the LIVE position, written every tick by `FUN_0046230E` (`0x46230E`) and latched on arrival by `FUN_00461948`. **24.8 fixed** — five sites shift by 8. |
| `map.x`, `map.y` | the same words `>> 8` | derived; the original stores only the fixed value. |
| `map.tx`, `map.ty` | `hero+0x9C`, `+0xA0` | the walk TARGET, written by `FUN_004620F3` (`0x4620F3`). |
| `map.speed`, `map.duration` | `hero+0xAC`, `+0xB4` | |
| `map.id` | `hero+0x90` | **the MAP, not the link.** `START_LOCATION` is `<map>, <link>, <dropIn>` (`extracted/worlds/Evergreen/levels.txt:619`), the new-soul path writes args 1/2/3 to `+0x90`/`+0x67C`/`+0xCD8` (all.c:24389-24391), and the use site `SendMessageA(frame, 0x46A, +0x67C, +0x90)` is `0x46A(link, map)`. |
| `map.facing_raw` | `hero+0x88` | the original's own encoding, `fy*4+fx` with a 5→9 remap (`0x46230E`) — **not** the port's `fy*3+fx`. Compared as the raw word. |
| `map.path_count`, `map.path_cursor` | `DAT_004F2168`, `DAT_004F216C` | the polyline itself is 2000 nodes × 3 ints at `0x00679E28`. |
| `map.nearest`, `map.hit`, `map.latched` | `DAT_004F2240`, `+0x44`, `+0x2C` | |
| `map.enc_a`, `map.enc_b`, `map.enc_grace` | `_DAT_004F2220`, `_DAT_004F2224`, `DAT_004E70A8` | `GetTickCount` stamps. |
| `map.no_monsters` | `DAT_004F2228` | |
| `map.wander`, `map.wander_legs` | `DAT_004F2190`, `_DAT_004F219C` | |
| `map.music` | `DAT_004E70B8` | |
| `map.waypoints` | `DAT_006840D0[7]` | option 7, "Enable automatic Way Point calculations". **Default 1 — ON in retail.** |

#### 5.4.2 `battle.*`

The fight's `this` is the CWnd sub-object at `pane+0xBC`, where the pane is
`CSplitterWnd::GetPane(*(void**)0x004E483C + 0x13FC, 0, 0)`. `0x004E483C` is the
**CSoulsView pointer, live for the whole game** — it is not a "fight is open" flag and it
is not a minigame flag either, which is the trap two separate audits fell into. The
`GetPane` chain leaves the image through the MFC thunk `FUN_004C52EE` (`jmp [0xD8B1EC]`),
so the hook cannot follow it and the fight words are read from the scene block instead.

The combatant records are `rec(i) = scene + 0x128 + i*0x6E0`, 144 slots. The sealed
(anti-cheat) fields are **not** encrypted at rest: `FUN_0049B71B` stores the plain dword
and `FUN_0049B70F` returns it. `rec+0x5B8` is current HP, `rec+0x2A8` is MAX HP — the same
current/max split as the hero's `+0x70`/`+0x74`. Slot x/y are **plain logical units** in
`0..360 × 0..256` (`FUN_0048B13C` returns 360 and `0x4806CE` adds it straight to a
`rand()%32`), so a `>>8` on the oracle side would emit `1` where the port emits `210` on
every monster.

#### 5.4.3 `items.*` and `panels.*`

The trophy bag is 128 words at `hero+0x0CE0` — `FUN_0046F726` reads, `FUN_0046F779` writes
`(count<<8 | id<<16) ^ 0x1D43E217` — and the geometry word at `hero+0x0EE0`
(`w<<16 | h&0xFFFF`, split by `FUN_0046F60F`). The pet pen is its own block: base
`DAT_004E4878`, stride `0x608`, 32 slots (`FUN_0040FBFD`'s fixup loop). The pet's stat
EncInts are at `+0xD8/+0xE0/+0xE8/+0xEC/+0xF0`.

`hero+0x0EE8` is the pet's live id and is read **only as `!= 0`**. `hero+0x0EEC` is **not**
a second pet id: it is a `GetTickCount()` stamp written by `FUN_0043B8E7` at `0x42151` and
lazily re-stamped at `0x42177`. `+0x6BC`/`+0x71C` are the **cheat-point** counter and its
negative mirror, not the pet and not the equipped right-hand item.

#### 5.4.4 PORT-ONLY keys: the original has no counterpart

These are emitted by the port and **deliberately not** emitted by the oracle, because
inventing a value for them would compare two different things and call it parity.

| key | why there is no original global |
|---|---|
| `panels.kind`, `panels.selected`, `panels.count`, `panels.first`, `panels.selling`, `panels.training_kind`, `panels.ability`, `panels.offers`, `panels.message` | The original has **no panel-kind word**. It has N separate `CDialog` children at fixed offsets inside the CSoulsView, and the list cursor and the message text live in stack frames (`CDialog local_184[324]` inside `FUN_004A6353`, all.c:121570). `DAT_00D64CC8`, cited as "the message line", is an RGB palette entry passed as a colour argument. |
| `items.attr_pool` | A `CDialog+0x74` on a **stack** dialog. `DAT_004EEFA4` is only the opening snapshot, read once to detect overspend. |
| `items.throw_armed` | There is no armed-item word. The cited `FUN_004A4D70`'s `param_2` is the arg16 **attack-path grammar** (cases 1..0xB set `0x20`/`0x40`/`2` and add sin/cos jitter), not an item class. The throw resolves from the equipped right-hand slot. |
| `minigame.open` | There is no "a minigame window exists" flag. The original keys off the dialog's own window; `DAT_004E483C` is the CSoulsView pointer. `minigame.armed` **is** real: `DAT_004E18A4`. |
| `minigame.result`, `minigame.label`, and every `minigame.<game>.*` | Per-game state lives in the dialog objects and in window handles, keyed by dialog resource id (`0xA7/0xB7/0xBF/0xC0/0xC4/0xC5/0xC6`) and window class `"Quadris"`. |
| `scene.*` (all of them) | The port's scene VM state — program counter, condition codes, push depth, the cookie set — is not a set of globals in the original. The original's opcodes act on the script's own locals. `scene.timer.N.length/left` and the countdown come from the VM's timer table, not from a fixed address. |
| `options.named.*` | Read through `GetProfileInt`, so the value is in the registry, not in the image. The 33 numbered ones **are** real: `DAT_006840D0`, initialised `.data`. |

### 5.4.1 The first module key with an oracle source

`battle.spell_success_percent`, read from **`_DAT_004E0FF8`** in the running original.
Live value measured in a boot run: **100**, which is the "no scaling" case — the original's
consumer `FUN_004A7456` compares it against 100 at `0x4A750C` and skips the multiply.

It is registered here rather than left implicit because it is the first key of a shape the
rest of the module dumps will have: **a value the original holds constant during a fight but
the port applies dynamically.** The original has per-spell fizzle flags at
`spell[0x124]/[0x128]`; the port's `SpellDef` has no columns for them and substitutes the
global. So a `.dsc` that casts a spell compares equal on the hero record, the world dump
and `rng.state`, and can still diverge in the roll. Emitting the value on the battle label
is what makes that divergence visible *at the moment it happens* instead of arriving later
as an unexplained `cast_success` difference that reads as replay flakiness.

**Do not resolve it by ignoring the value in `cast_success()`.** A silent constant in place
of a modelled field is the same defect with the evidence removed. Port the two flag columns
or record the substitution in `docs/architecture_port.md`'s deliberate-deviations table;
owner is the spell panel, not the oracle.


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

## 6.1 The differential run, and what it says

`tests/diff_oracle.sh` runs every `tests/diff/*.dsc` on both sides and diffs per label. Two
properties of the driver are load-bearing and worth stating, because getting either wrong makes
the suite report success:

* **The port is invoked WITHOUT `--dump`.** `src/game_main.c`'s `run_dumps()` writes the whole
  dump table once, at exit, so a `--dump` file holds the **end-of-run** value for every label in
  it. Diffing per label against that file reports the final state N times and calls it agreement.
  The per-op values come from the event log (`dump_one()` -> `log_emit`), reassembled by
  `tools/oracle/split_port_log.py`. `DIFF_PORT_DUMP` is refused outright for this reason.
* **A label only one side produced is a result, not a skip.** Oracle-only is reported as
  `PORT-ONLY` and counted in the failure total, because an unreachable target is exactly the
  failure the suite exists to catch.

Measured 2026-09-28, six scripts, 19 labels, `labels=19 equal=1 mismatched=18`:

| script | label | result | first differing keys |
|---|---|---|---|
| `boot_only` | `rng@boot` | MISMATCH | `rng.calls` 1414 / 1419, `rng.state` |
| `boot_only` | `clock@boot` | MISMATCH | `rng.calls` 1414 / 1419, `rng.state` |
| `boot_only` | `rng@mid` | MISMATCH | `rng.calls` 1415 / 1426, `rng.state` |
| `boot_only` | `rng@late` | MISMATCH | `rng.calls` 1415 / 1432, `rng.state` |
| `boot_click` | `rng@before` | MISMATCH | `rng.calls` 1414 / 1419, `rng.state` |
| `boot_click` | `clock@before` | MISMATCH | `rng.calls` 1414 / 1419, `rng.state` |
| `boot_click` | `rng@after` | MISMATCH | `rng.calls` 1416 / 1422, `clock.ms` 430 / 400 |
| `boot_click` | `front@after` | MISSING | port has no `front` module |
| `boot_menu` | `front@title`, `front@menu` | MISSING | port has no `front` module |
| `boot_newsoul` | `clock` | MISMATCH | `rng.calls` 1414 / 1418, `clock.ms` 150 / 0 |
| `boot_newsoul` | `rng` | MISMATCH | `rng.calls` 1416 / 1426, `rng.state` |
| `boot_newsoul` | `hero` | MISMATCH | `hero.valid` 1 / 0, `hero.record`, +10 named keys oracle-only |
| `boot_newsoul` | `map` | MISMATCH | `map.id` 0 / -1, `map.enc_a`, `map.enc_b`, `map.enc_grace`, `map.no_monsters` 1 / 0 |
| `front_hotspots` | `front@*` (x4) | MISSING | port has no `front` module |
| `front_hotspots` | `options` | **OK (34 keys)** | — |

`hero.base_offset` agrees at every checkpoint (1168 both sides), and `clock.ms` agrees at every
checkpoint that follows no input (150/150, 800/800, 1400/1400).

**The one green label is real.** `options` compares equal on all 33 numbered options read live out
of `DAT_006840D0` in the running original, including option 7 (waypoints, default **1** = ON) and
option 9 (no record in the table, so it stays 0).

**The shape of the `rng.calls` gap is the useful result.** `boot_only.dsc` has no input at all, so
it separates boot from runtime:

| virtual ms | oracle | port | gap |
|---|---|---|---|
| 0 | — | 1418 | — |
| 150 | 1414 | 1419 | 5 |
| 800 | 1415 | 1426 | 11 |
| 1400 | 1415 | 1432 | 17 |

The original takes **one** draw across 1250 ms of idle virtual time and then **zero** across the
next 600. The port takes 7 and then 6. So there are two independent problems — a constant boot
offset and a per-idle-window excess — and the original's steady state is *zero*, not a smaller
constant, so the port's per-window draws have no counterpart to be tuned towards.

---

## 7. Known gaps and honest limits

* **Eleven of the twelve required scripts do not exist yet, and the reason is one
  measurement, not eleven problems.** Everything up to the main menu works; getting past it
  does not, and the cause is now known. The front end's buttons are entries in a hotspot
  table (`DAT_005339F8`), not child windows, and `FUN_00405765`'s hit test is a `PtInRect`
  against a rect that `FUN_004054A8` **animates** from a "from" to a "to" position over the
  entry's `t_len`. So the same button is at a different pixel at a different moment:

      t=1200  "Play now" rect = 591,120,864,162   (mid slide-in, right of the client)
      t=2200  "Play now" rect = 194,120,467,162
      t=2600  "Play now" rect =  79,120,352,162   (settled, fully on screen)

  and the animation only advances on a **paint**, so a script that never dumps between the
  state change and its click is clicking at coordinates chosen for a rect that has not
  moved. The harness also had to be fixed before any of this was measurable: the client is
  now forced to 640x480 *before* the front end lays itself out, because the layout is
  computed from `GetClientRect` at state entry and a later resize leaves every stored rect
  describing a client area that no longer exists.

  **The animation only advances on a paint, and a `dump` is what forces one.** `FUN_004054A8`
  is driven by the front end's paint, so a script that reaches the menu and clicks without
  dumping in between is clicking at coordinates chosen for a rect that has not moved: with no
  intervening dump, "Play now" is still at its "from" position `(591,120,864,162)` and the
  settled centre `(215,141)` misses it entirely. This is why the shared prelude in the
  translated scripts is `click -> dump -> dump -> click` and not `click -> click`.

  **Past the main menu the original cannot be driven at all by this suite's means.** The
  "Where Do You Want To Play Today?" screen (state 2) registers exactly ONE hotspot, and it
  is a non-clickable label — `state=1`, `rect=39,60,533,102`, `msg=0000`, `clickable=0`,
  label "Where Do You Want To Play Today?". `FUN_00405765` skips any record without the
  `0x400` bit, so there is nothing there to press, and `FUN_0041C1CD`'s dispatch switch
  (`0`, `5`, `6`, `8`, `9`, `10`) has no case for state 2, so a mouse-down does not advance
  it either. Three clicks at y=200, y=300 and y=400 all leave the front state at 1. The
  transition out of state 2 is therefore reached by something this suite has not identified,
  and until it is, the eleven translated scripts stop in the front end.

  Two further facts constrain the route and are worth not rediscovering: `key RETURN` is a
  **port-only** shortcut (four RETURNs leave the original at state 1,1,1,1 — the front
  view's message map has entries for `WM_LBUTTONDOWN`/`UP` and `WM_MOUSEMOVE` and nothing
  else), and `key SPACE` activates "Depart this realm" and **exits the process**.

  So the scripts that exist are the ones the original can actually be driven through:
  `boot_only`, `boot_click`, `boot_menu`, `front_hotspots`, and the pre-existing
  `boot_newsoul`. The other eleven need the world list and the new-soul dialog, which are
  behind this one.
* `scene.*` and `panels.*` have **no oracle source at all**, and section 5.4.4 says why for
  each key: the original keeps that state in `CDialog` children at fixed offsets and in
  stack frames, not in globals. This is a real gap in coverage, not a naming problem.
* `rng.state` differs on every label. That is expected — it is a function of the seed, not
  of the draw count — and it is not counted as a finding on its own. `rng.calls` is the
  comparable one, and it is not equal.
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

### The user-options table: INITIALISED .data, and I was wrong about it

I reported that `DAT_004F2A58` and its count `DAT_004F2BD8` were never written and that the
numbered-option load in `FUN_00466EF3` (0x00466EF3) therefore never runs. **That was
wrong, and the Oracle's live probe of the running original is what settled it**: the count
is 32 and the table is fully populated. Waypoints (id 7) default to **1**, i.e. ON in
retail, not off.

The table is **initialised `.data` in the image, not something a constructor builds**.
Read with pefile:

| | RVA | file offset | value |
|---|---|---|---|
| `DAT_004F2A58` | 0xF2A58 | 0xF0E58 | 32 records of 12 bytes: `{int id; int default; char *label;}` |
| `DAT_004F2BD8` | 0xF2BD8 | 0xF0FD8 | 32 |

My earlier scan missed this because the throwaway section-mapping helper I used in that
shell snippet was itself buggy (it returned `None` for both addresses), so "no references
found" became "never written". The lesson for anyone repeating it: resolve the address with
pefile's `get_offset_from_rva`, and treat a hand-rolled RVA table as unverified. The
byte-pattern search for `58 2a 4f 00` was not wrong about the *code* -- there genuinely is
no store in `.text` -- but I drew the wrong conclusion from it, because the data was never
supposed to be stored by code at all.

**The table, verbatim** (id, default, label). Cross-checked row for row against the live
probe, and the port's `--dump` now reproduces the probe's 33 live values exactly:

    13, 1, "Don't let me use cheat codes."                1, 1, "Use High-Resolution world maps. (very slow)."
    30, 1, "Improve jpeg image quality in scenes and maps. (slowish)."
     2, 1, "Auto-Smooth Low-Resolution world maps (slow)."
     3, 1, "Notify me when other players learn spells."
     4, 1, "Notify me about which spells are cast in fights."
    20, 1, "Notify me when my character changes deciLevel."
     5, 1, "Enable Player Chat Bubbles while in scenes."
    12, 1, "Make non-player character chat bubbles pop faster."
    24, 1, "In wide scenes, center camera on player chat bubbles."
     8, 0, "Don't pick up low-level junk from dead monsters."
    19, 1, "Hide Spells I can't learn yet"
    17, 1, "Show monster radar during hunts. (Golden Soul/Demo)"
     0, 0, "Show Hot-Key popup window while in scenes."
    32, 1, "Show Hot-Key button bar while in scenes."
    21, 1, "Show an icon when NPCs are waiting for an answer."
     6, 0, "Show Pet's Owner Tags"
    25, 1, "Show character index numbers in scenes."
    22, 1, "Show HTML pages in scenes, when scripted."
    14, 0, "Auto-open an IM window when people whisper to me."
    11, 0, "Log all chat to disk (warning - uses lots of disk)."
    27, 0, "Log all battles to disk (warning - uses lots of disk)."
    28, 1, "Log all death sentences to disk."
    10, 1, "My computer is slow, cut animations during dialogs."
    23, 1, "Stop all web page stuff on return to game."
    16, 1, "Remember changes to window size and positions."
    18, 1, "Confirm link images when adding new links."
     7, 1, "Enable automatic Way Point calculations."     <-- ON in retail
    15, 1, "Show 3D outline around button bar buttons."
    26, 0, "Don't use 100% cpu on WoS"
    29, 0, "Don't allow cheat characters when I host scenes."
    31, 1, "Zoom in on WoS Tactics attacks"

**`DAT_006840D0` has 33 slots (ids 0..32) but only 32 records: id 9 has none.** So option 9
is never named, never given a default and never loaded; it stays 0. Any script or harness
that wants option 9 cannot get it.

`FUN_00466EF3` loads each record as `GetProfileInt("Preferences", "option <id>", <default>)`,
so the default applies when the key is absent. The nine **named** scalars are read the same
way, each by its own unguarded call:

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
