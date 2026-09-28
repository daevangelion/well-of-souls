# The original's timing model (`extracted/Souls.exe`, WoS A96)

Owner: Core. Companion to [rng_calls.md](rng_calls.md) (RNG census), [script.md](script.md)
(quest VM) and [oracle.md](oracle.md) (the differential harness).

Every VA below is a Ghidra VA in the `Souls.exe` image (ImageBase `0x400000`).

**How the facts here were established.** Two independent sources, and where they
disagree the binary wins:

* `work/decomp/all.c` (Ghidra 12.1.3), and
* the PE itself: I parsed the import directory, resolved each import's IAT slot,
  and grepped a full `objdump -d` of `.text` for `call *<slot>`.

The second source matters. `all.c` shows **six** `SetTimer` call sites; the binary
has **eight**. Ghidra merged or failed to split the two functions at `0x428803` and
`0x4a1e18`, so the decomp is incomplete for this question. (Note for anyone
reproducing the grep: `objdump` prints the IAT slot with the image base added a
second time, so the call at the slot for `SetTimer` appears as
`call *0xd8b704` for the real `0x0098b704`.)

---

## 1. There is no frame loop. The driver is a `PeekMessage` pump.

`FUN_0040a8d9` (0x0040A8D9) is `CWinApp::Run`. It contains **no `GetMessageA`, no
`DispatchMessageA` and no `TranslateMessage`** — all three are absent from the
import table (MFC calls the window proc through `CWnd::DefWindowProc`
indirectly). It is a bare `PeekMessageA(PM_NOREMOVE)` pump:

```c
/* 0x0040A8D9, reduced. Names are the MFC vtable slots it uses. */
do {                                        /* outer */
    c = FUN_00449a49();
    for (;;) {                              /* inner: IDLE, only on an EMPTY queue */
        if (PeekMessageA(&msg, 0, 0, 0, 0)) break;
        OnIdle(++n);                        /* vtable + 0x68 */
        if (FUN_0040a7c7() && FUN_00467312(0x1a)) break;
        /* foreground-window tracking into DAT_004dd50c */
        if (!foreground_is_us) break;
    }
    for (;;) {                              /* DISPATCH, while a message exists */
        if (msg.message == WM_KEYDOWN || msg.message == WM_MOUSEMOVE)
            _DAT_004dd0f0 = GetTickCount();          /* last user input */
        else if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN)
            DAT_004dd51c++;                          /* click counter */
        /* > 1000 ms since the last click burst and > 30 clicks: PostQuitMessage */
        if (c) PostQuitMessage(0);
        if (!IsIdleMessage() || DAT_004dd524) { ExitInstance(); return; }
        if (PreTranslateMessage(&msg)) n = 0;
        if (!PeekMessageA(&msg, 0, 0, 0, 0)) break;
    }
} while (1);
```

Three consequences, and they are the whole model:

1. **Order is INPUT -> TIMER -> IDLE/PAINT.** A message in the queue pre-empts idle
   entirely; idle runs only on an empty queue.
2. **Nothing runs "per frame".** There is no fixed step. A piece of state advances
   when the original's code runs: on a message, on a `WM_TIMER`, or on the 20 ms
   idle gate. Every rule that matters is guarded by a stored `GetTickCount()` and a
   millisecond comparison.
3. **The port's loop must be `input -> timers -> idle/paint` in that order**, and
   in headless mode the step count must be a function of the script alone.

### 1.1 Tick sources actually present

| Source | In the import table? | What it drives |
|---|---|---|
| `GetTickCount` (IAT `0x0098ae24`) | yes | **the only rule tick source**, ~200 call sites |
| `timeGetTime` | **no** | — (the shared plan's assumption was wrong; there is no such import) |
| `time` (MSVCRT) | yes | 20 sites, **none** gates a rule: record stamps, chat times, the two `srand` seeds, and the SRNet anti-tamper check |
| `QueryPerformanceCounter` / `-Frequency` | yes | 2 sites, both in the SRNet perf graph `FUN_0042895c` |
| `GetMessagePos` | yes | 2 sites, menu/drag geometry only |

So the port has exactly one tick source: `clock_ms()`.

---

## 2. The 20 ms idle gate — `FUN_0040a7c7`

```c
/* 0x0040A7C7, reduced */
uint FUN_0040a7c7(void) {
    int due = 0;
    if (DAT_004e4844 != 0 && DAT_004e709c == 0) {          /* app exists, not tainted */
        if (GetTickCount() - _DAT_004dd510 > 0x13) {        /* > 19 ms => 20 ms */
            due = 1;
            _DAT_004dd510 = GetTickCount();
            FUN_0042895c();                                 /* SRNet perf graph */
            FUN_00416cd3();                                 /* heap probe         */
            FUN_00401f8d();                                 /* sound poke         */
            FUN_0047397b(1);                                /* Lag-O-Meter sample */
            /* time() anti-tamper: if the wall clock jumps, flag and Sleep(5000) */
        }
    }
    return due;
}
```

`_DAT_004dd510` is the global at **VA `0x004DD510`**. The period is **20 ms** —
not 16.7 ms and not 60 Hz. This is the closest thing the game has to a periodic
tick, and it is a *gate on an idle pass*, not a scheduler.

**What the 20 Hz work actually is.** None of the four callees is a solo-play rule:

| Callee | What it is | Solo? |
|---|---|---|
| `FUN_0042895c` (0x42895C) | SRNet ping/perf graph. Has its own 25 ms sub-gate, and an **unconditional discarded `rand()`** at the top of every call (the call site is **VA 0x00428996**) | no, but it **consumes RNG** — see rng_calls.md §2.1 |
| `FUN_00416cd3` (0x416CD3) | a 100000-byte `malloc` probe on a few app states | no effect |
| `FUN_00401f8d` (0x401F8D) | a sound poke when `DAT_004e6910 == 4` | no |
| `FUN_0047397b` (0x47397B) | accumulates one sample into the Lag-O-Meter's 0x2EE8-byte slots | no |

### 2.1 The idle rate is NOT 50 Hz -- it is however fast the queue drains

Measured, not inferred. The Oracle's hook read `DAT_004DD510` -- the very global
`FUN_0040A7C7` re-stamps -- from the **running original** under Xvfb, and it read **180**
while the clock read 1000. The gate was 800 ms behind: over 2500 ms of scripted idle the
game ran **13** idle passes (13 `rand()` calls, all from `0x00428996` inside
`FUN_0042895C`), i.e. about one per 180 ms, not one per 20 ms.

The cause is the pump, not the gate. `FUN_0040A7C7` is only reached from the idle half of
`FUN_0040A8D9`, which runs **only while `PeekMessageA` returns nothing**. On the title
screen the original repaints continuously, so the queue is rarely empty and `OnIdle` is
skipped on most 20 ms boundaries. The idle rate is therefore a function of how fast the
app can drain its own message queue, which is a property of the host and the screen, not a
constant in the program.

**Two consequences the differential harness has to respect:**

1. `rng.calls` is **host-dependent and therefore not a comparable key** at the title
   screen, or in any state where the original is continuously repainting. Comparing it
   there compares two hosts, not two implementations. It becomes comparable only once both
   sides are in a fixed game state whose loop rate is driven by the script rather than by
   repaint. Until then it is a diagnostic, not an assertion.
2. The port's `clock_idle_due()` is called once per loop iteration, and in `--script` mode
   the queue is empty except on the iterations that deliver an op, so the port will fire
   the 20 ms tick **more often than the live original does on a repainting screen**. That
   is the correct choice for a script-driven replay -- it makes the port's tick count a
   function of the schedule instead of the host -- but it must not be read as a claim that
   the original ticks at 50 Hz. It does not.

**Port rule.** `clock_idle_due()` returns 1 at most once per 20 ms. The port
**re-arms the stamp from the previous boundary** (`last += 20`), never from the
observed time, so that jumping the clock by *N* ms still costs `floor(N/20)` idle
ticks. Re-stamping from `now` would collapse a 1000 ms jump into one tick and
change the tick *count*, which changes nothing for ms-guarded rules but changes
everything for a `GetTickCount`-bounded loop such as the Quadris splash
(`FUN_0042198f`, rng_calls.md §4 A.5). The Oracle's harness reads the original's
own `0x004DD510`, so both sides land on the same boundaries.

`clock_20hz_next()` exposes the boundary so the script loop can step exactly onto
it; the loop must not be able to skip one.

---

## 3. Painting is self-driven: the main window invalidates and updates itself

`FUN_0041bdb4` (**0x0041BDB4**) is the main game window's paint handler. It is a
state machine over the app state `DAT_004df8a4`, and **every branch ends in
`InvalidateRect` followed by `UpdateWindow`**, which synchronously delivers
`WM_PAINT` and re-enters. That is the original's animation driver: a busy repaint
loop, not a timer.

Its per-state work:

| State | What it does |
|---|---|
| 0 (title) | `GetTickCount() - [this+0x13F4] > 13000` -> `FUN_0041b891(1)`; then invalidate |
| 1, 3 | invalidate |
| 2, 8 | `FUN_004057d3()` gate, then invalidate |
| 4 | `FUN_00439f7a(4, "About to do extra timer stuff in ...")` ... `FUN_00456aa1()` ... `"Skipping extra timer stuff in st..."`; `FUN_00439f7a(4, "Story Over")`, `FUN_0042198f(1000,0,100)`, `PostMessageA(main, 0x478, 0, 0)`, then `thunk_FUN_00488010()` **three times** |
| 5 | `FUN_00418416()`, `FUN_00477012()`, `FUN_00462958()` (map movement) |
| 6, 9, 10 | `FUN_00462958()`; if the actor moved, invalidate `[this+0x3508]` |
| 7 | `FUN_00462958()` + the same dirty test |
| 0x0B (web view) | `FUN_00462958()` then the splitter panes |

Then **in every state** (the `else` of the `bVar2` test):

```
FUN_00471245();  FUN_004a45bf();  FUN_0044ab10();  FUN_00478840();
FUN_00456aa1();  FUN_004246b6();
DVar4 = GetTickCount();
if (DVar4 - _DAT_004dd0f0 < 0x493e1) {        /* < 5 min since the last key/mouse */
    DAT_004df8d4 = 0;
} else {
    /* SRNet idle path: FUN_00417e91(hero), DAT_004df8d4 = 1, FUN_00434f95 */
    _DAT_004e1800 = GetTickCount();
}
```

`FUN_00478840` is the **stamina/HP-MP regen** and runs on every paint pass in every
state. `FUN_004246b6` and `FUN_00471245` are the other two per-pass rules.

**Port rule.** `screen->update()` + `screen->render()` run once per loop
iteration, exactly as here. Because this loop is unthrottled, the iteration *rate*
is free in the live game; in headless mode the port runs a fixed, documented
number of iterations per 20 ms boundary so the count is deterministic. **This is
safe only because every rule on the path is `GetTickCount`-guarded** — except where
rng_calls.md §4 flags a `GetTickCount`-bounded loop, which must be driven by
`clock_ms()`.

---

## 4. Timer table: every `SetTimer` / `KillTimer` in the binary

Eight `SetTimer` call sites, all `TIMERPROC = 0` (so `WM_TIMER` to the window),
all of the shape "store the returned id in a member, re-arm only if it is 0".
`KillTimer` is always `KillTimer(hwnd, m_nTimerId)`.

| # | `SetTimer` VA | id | interval | owner (member slot) | `WM_TIMER` handler | What it is |
|---|---|---|---|---|---|---|
| 1 | `0x428803` | 22 (`0x16`) | 100 ms | `FUN_00428360`, `+0x114` | `FUN_00428c8f` (0x428C8F) | calls `FUN_004094e7` at `0x4287ee` first |
| 2 | `0x448c0e` | 2 | 100 ms | `FUN_004483a6`, `+0x1FAC` | `FUN_00448cfc` (0x448CFC) | `GetPrivateProfileStringA` on `BIO_INFO` / `serNum` / `rating` — the registration dialog |
| 3 | `0x44931f` | 4 | 1000 ms | `FUN_00449144`, `+0x140` | `FUN_0044989e` (0x44989E) | 5-row chooser (`Tempus Sans ITC`, five rects, `EndDialog(1)` when empty) |
| 4 | `0x455f6d` | 2 | 100 ms | `FUN_00455f56`, `+0x60` | `FUN_00456197` (0x456197) | WoS Tactics dialog `OnInitDialog` |
| 5 | `0x4722a7` | 2 | 1000 ms | `FUN_00472292`, `+0x60` | `FUN_0047231b` (0x47231B) | the **Lag-O-Meter** ("LagOMeter", 6 columns, `SerNum`); torn down by `FUN_004722b2` / `KillTimer` at `0x4722d1` |
| 6 | `0x47430e` | 2 | **50 ms** | `FUN_004742f0`, `+0x60` | `FUN_004743e8` (0x4743E8) | minigame/animation dialog; `OnInitDialog` is `FUN_004742da`, its class owns the 9-rand `FUN_0047437e` |
| 7 | `0x49f02d` | 2 | 100 ms | `FUN_0049ef7b`, `+0xB8` | `FUN_00499b12` (0x499B12) | the object editor ("Select an object to edit") |
| 8 | `0x4a1e18` | 4 | 100 ms | `FUN_004a1b52`, `+0x68` | `FUN_004a1e4c` (0x4A1E4C) | torn down by `KillTimer` at `0x4a1e40` |

**The handlers were recovered from the MFC message maps, not from `all.c`.** There
is no `case 0x113` anywhere in the decompilation. In this binary
`AFX_MSGMAP` is **24 bytes with `pfn` as the last dword** (verified on the
Blackjack dialog: the `WM_TIMER` record at `0x4c76d0` resolves to `0x0040bcf7`,
which is the 3-second dealer-reveal handler). Scanning `.rdata` for the dword
`0x113` and reading `+20` yields **17** `WM_TIMER` entries; the eight above are
the ones reachable from a `SetTimer` site. The other nine
(`0x406666 0x4126df 0x428c8f 0x453c23 0x465b87 0x46691e 0x46a161 0x46e07e
0x4a11e2`) have no `SetTimer` that arms them and are effectively dead; treat any
behaviour in them as unreachable unless a `SetTimer` is found.

**Blackjack's timer is a real one with no `SetTimer` call.** `FUN_0040b1e7` (the
dialog's `OnInitDialog`) stores the id at `this+0x77c` at `0x40b20c` and
`FUN_0040b1ac` kills it at `0x40b1c3`, but the arming `SetTimer` is not among the
eight — Ghidra and the IAT both show it is genuinely absent, so the id stays 0 and
`WM_TIMER` never fires. `FUN_0040bccf` (via the handler `FUN_0040bcf7`) is the
3-second reveal check: `if (state == 3 && GetTickCount() - [this+0x780] > [this+0x788])`
with `[this+0x788] = 3000` set by `FUN_0040b9e6(3)`. The port implements the
**3000 ms** rule (MiniGames owns it) and does not need a timer for it.

### 4.1 One timer-like handler that changes rules

`FUN_0044a63a` (0x0044A63A) has **no `SetTimer`** but is named by its own assert
string `"Status::OnTimer freshening monsters"` and refreshes monsters when
`1000 < GetTickCount() - [this+0x44]`. It is solo-reachable and **changes battle
contents**. Implement it; do not treat it as cosmetic.

---

## 5. `Sleep`

| VA | Argument | Context |
|---|---|---|
| `0x40a8bc` | 5000 | the `time()`-jumps anti-tamper penalty inside `FUN_0040a7c7` |
| `0x40b743` `0x40b7df` `0x40bae6` `0x40bb12` `0x40bb43` `0x40bb6f` | 250 | the Blackjack card-deal animation: `InvalidateRect` + `UpdateWindow` + `Sleep(250)` per card |
| `0x4267ef` | 100 | a 30 s wait loop at boot |
| `0x43a833`, `0x44d41d`, `0x4683c9`, `0x4693d7`, `0x48a170`, `0x48a18e`, `0x498949` | 100 | progress/polling loops |
| `0x4a1c..` (`0x41370x` region) | 3600000 | the SRNet idle hour |
| `0x4a5xxx` (`Sleep(50)` at all.c:101648/101651) | 50 | a minigame cadence loop |

The port replaces the 250 ms deal sleeps with `clock_advance(250)` under the
virtual clock so a `.dsc` script reproduces them exactly; the 5000 ms penalty and
the 3600000 ms hour are SRNet-only.

---

## 6. Seeding — the four `srand` sites

| all.c | VA | Function | Seed | Port |
|---|---|---|---|---|
| 27953 | 0x4269AF | `FUN_004269af` (the `CWinApp`-derived global-constructor class) | `srand((uint)time(NULL))`, then **one `rand()`** feeding `DAT_004e4870` (the hero record base) | `crt_srand(clock_time_s()); crt_rand();` |
| 27987 | 0x4269AF | same function, 34 lines later, after `FUN_00427d89()` | `srand((uint)time(NULL))` — **this one wins** | `crt_srand(clock_time_s());` |
| 48593 | 0x443929 | `FUN_00443929` (random-elevation world generator) | `local_10 = rand(); srand(param_1);` — the draw happens **first** | `crt_rand(); crt_srand(seed);` |
| 48643 | 0x443929 | same, exit path | `srand(local_10)` — reseeds with the *drawn value*, so the post-call stream is **not** the pre-call stream | `crt_srand(saved);` |

`time()` is ONE source in the original and ONE in the port. `--time E` **pins** it (the
whole run reads E); without it the port follows the host clock, which is what the original
does. `clock_time_s()` and `plat_time_s()` are the same value by design: the original
derives none of its `time()` reads from `GetTickCount`, so neither does the port. `--seed N`
overrides the value handed to the two boot `crt_srand` calls only, for replay debugging;
it does not touch the date check.

---

## 7. Module mapping — "your handler runs here"

This is the table module owners asked for. "Port" is what to do in the module.

| Module | Original work | Original driver | Port |
|---|---|---|---|
| **MapView-2** (`mapview.c`) | `FUN_00462958` (0x462958) — the per-pass actor/movement step | called from `FUN_0041bdb4` states 5, 6, 7, 9, 10, 0x0B; **not** a timer | run from your `update()` on the paint path |
| **MapView-2** | `FUN_004610d9` (0x4610D9) — idle auto-wander | called from `FUN_00461138` (0x461138) at `0x4613de`, i.e. **the map paint handler** | same |
| **MapView-2** | `FUN_0046260e` (0x46260E) — the random-encounter roll | called from `FUN_00462958`; two `rand()` calls each independently short-circuited by a **5 s** `GetTickCount` cooldown (all.c:70925/70927/70929) | `clock_ms() - t0 > 5000`, and the short-circuit must consume the `rand()` anyway — this is rng_calls.md §4 A.1, the most fragile ordering in the file |
| **SceneVM-2** (`scene.c`) | `FUN_0049454f` / `FUN_00494592` — the TIMER/COUNTDOWN start-tick and length | `GetTickCount()` stored, compared in ms | `clock_ms()`; delete every per-frame divisor |
| **SceneVM-2** | WAIT (stores a tick at `scene+0x3E038`, `ftol(atof(arg))` at `+0x3E03C`), COUNTDOWN (`DAT_004FA850` tick, `DAT_004FA854` = n*1000) | `GetTickCount` | `clock_ms()` |
| **SceneVM-2** | `FUN_00412d2f` region (all.c:13411-13440) — `sin(GetTickCount()*k)` ambient wobble | a **100 ms** self-gate, not a frame | `clock_ms() - t0 >= 100` |
| **Battle-2** (`battle.c`) | `FUN_00490723` — end of round | `GetTickCount() - _DAT_005006cc > 2000` | `clock_ms()` |
| **Battle-2** | `FUN_0048f913` — a monster's flee/wander | `GetTickCount() - rec[0x45C] > 1999` | `clock_ms()` |
| **Battle-2** | `FUN_0048b17a` — hero may act | 500 ms after round start | `clock_ms()` |
| **Battle-2** | `FUN_0048fe80` — attack duration | 1000 ms physical / 4000 ms spell / 1250 ms for the -3/-4/-5 specials | `clock_ms()` |
| **Battle-2** | `FUN_00478840` — HP/MP regen gauges | `min(100, (now - regenStart) * (str/2+100) / 10000)` | `clock_ms()`; **runs on every paint pass in every app state** (§3) |
| **Battle-2** | `FUN_0044a63a` — monster refresh | 1000 ms, no `SetTimer` | `clock_ms() - [this+0x44] > 1000` |
| **Battle-2** | `FUN_004246b6`, `FUN_00471245` | every paint pass | every paint pass |
| **FrontHero-2** (`front.c`) | `FUN_0041bdb4` state 0 | `GetTickCount() - [this+0x13F4] > 13000` | `clock_ms()`; the stamp is written by `FUN_0041ba9f` (`GetTickCount()` then `InvalidateRect`+`UpdateWindow`) |
| **FrontHero-2** | `FUN_0041c15c` — the 20 s scrollbar auto-repeat | `GetTickCount() - _DAT_004df8d8 < 20000` | `clock_ms()` |
| **Panels-2** (`panels.c`) | items / equipment / shop / training / stats | **no timer, no `WM_TIMER`** — plain modal-dialog input | none |
| **Panels-2** | the 5-row chooser (table row 3) and the Tactics dialog (row 4) | `SetTimer` id 4 @ 1000 ms and id 2 @ 100 ms | `clock_set_timer(owner, 4, 1000, ...)` / `(owner, 2, 100, ...)` |
| **MiniGames** | Blackjack reveal | 3000 ms, **no** `SetTimer` (see §4) | `clock_ms()` |
| **MiniGames** | the 50 ms animation dialog (table row 6) | `SetTimer` id 2 @ 50 ms | `clock_set_timer(owner, 2, 50, ...)` |
| **Editors** | the object editor (table row 7) | `SetTimer` id 2 @ 100 ms | `clock_set_timer(owner, 2, 100, ...)` |
| **MissionsHtml** | HTML/mission pages | no tick source of their own | none; they are message-driven like the panels |

**Nobody uses `clock_set_timer` for a solo-play rule.** All eight timer sites are
dialogs. That is the headline: the game world is 20 Hz-idle and
`GetTickCount`-driven, and any module that needs a timer for a rule has invented
one the original does not have.

---

## 8. The port's event loop

`src/game_main.c`, replacing the old 60 Hz fixed step:

```c
while (!quitting) {
    /* INPUT */
    input_begin(&input);
    while (plat_poll_event(&event)) input_event(&input, &event);

    if (script) {                       /* .dsc: the clock is the schedule */
        for (;;) { op = dscript_take(script, clock_ms()); if (!op) break;
                   apply_script_op(op, &input); }
        if (script->has_end && clock_ms() >= script->end_ms) break;
        next = dscript_next_time(script, clock_ms());
        if (next == UINT32_MAX) break;
        target = min(clock_20hz_next(), next);          /* land on the boundary */
        if (target > clock_ms()) clock_advance(target - clock_ms());
    } else if (replay) {                 /* legacy .rpl: 1000/60 ms per frame */
        status = replay_step(...);
        phase += 1000; clock_advance(phase / 60); phase %= 60;
    }

    clock_dispatch_timers();             /* TIMER */
    if (clock_idle_due()) idle_tick();   /* IDLE, 20 ms */
    scene_tick();                        /* PAINT: the self-driving repaint */
    screen->update(&input);
    screen->render(&fb);
    plat_present(...);
}
```

* **Headless / `.dsc`**: `clock_ms()` is a pure function of the script timeline. It
  moves only via `clock_advance()`/`clock_set_now()`, never from the host, so a
  run is bit-for-bit reproducible.
* **Interactive**: `clock_attach_realtime()` makes `clock_ms()` track
  `plat_ticks_ms()`, and the loop sleeps to the next 20 ms boundary.
* **`--replay`** keeps the legacy 1000/60 ms per-frame virtual step so the existing
  `.rpl` acceptance tests are bit-identical.

### Timer emulation

`clock_set_timer(owner, id, interval_ms, fn, user)` follows Win32: setting a live
`(owner, id)` replaces it, `uElapse < 10` is clamped to `USER_TIMER_MINIMUM`, and
`clock_kill_timer` matches on the pair. `clock_dispatch_timers()` fires the
due timer with the smallest deadline (ties by registration order), moves the
virtual clock **forward** to that deadline, and re-arms with
`deadline += interval`. So an overdue timer walks forward one interval per
message and never replays a backlog; and because the clock never rewinds, a
handler that is already late sees `clock_ms()` at the present, exactly as
`GetTickCount()` does. This is the rule the Oracle agreed to and documents in
docs/re/oracle.md.

### 8.1 Flags

| Flag | Meaning |
|---|---|
| `--data DIR` | retail data root (required) |
| `--save DIR` | save directory; defaults to `<data>/Save` |
| `--headless` | no window |
| `--script FILE` | a `.dsc` schedule; the deterministic differential mode |
| `--replay FILE` | the legacy `.rpl` format, still supported |
| `--dump FILE` | write every registered module dump as `key=value` lines |
| `--log FILE` | structured `EVT ...` lines |
| `--time EPOCH_S` | **pins** `time()` to EPOCH_S for the whole run (default: follow the host) |
| `--seed N` | pin the `time()` value for the two boot `srand` calls |
| `--max-frames N` | iteration cap; exit 3 when hit |
| `--shot-every N DIR` | periodic BMP screenshots |

`--replay` and `--script` are mutually exclusive.

---

## 8.2 Where the boot's 1432 rand() calls go

Measured by the Oracle's hook with the trace armed in `DllMain`, so the pre-boot calls are
included. Every call in a run of `tests/replay/offline_probe.dsc`, by caller VA:

| calls | caller | in | kind |
|---|---|---|---|
| 1408 | `0x0049B6F7` `0x0049B6FC` `0x0049B701` `0x0049B706` | `FUN_0049B6C7` — 4 `rand()` per call, **352 calls** | one boot-constant table: 352 objects x 4 draws, in the order int[+4], int[+16], int[+32], int[+48], after three scaled-double writes |
| 13 | `0x00428996` | `FUN_0042895C` | the 20/25 ms idle path, above |
| 4 | `0x00456BCC` (call at `0x00456BC6`) | `FUN_00456B87(obj, id, count)` | a **128-entry scheduled-event table**: appends at `obj+8`, cap `0x80`, **0x610 bytes per entry**. Writes `id` at `+0x0C`, `GetTickCount()` at `+0x20C`, `count` at `+0x40C`, `(rand() % (count*2)) * 1000` at `+0x60C` — a random delay in whole seconds; returns -1 when full, so it saturates and stops consuming. **The offline seeder is `FUN_00456C51`: exactly four appends, ids 0x0E..0x11 = 14..17, count 0x1E = 30.** Driven by `FUN_00456D2F`, which early-returns unless the enableSFX / enableEnvironmentalSounds / enableSoundCard options are all non-zero -- all default 1, so it runs | **implemented: src/game/sched.c** |
| 1 | `0x0048E1CF` | — | singleton |
| 4 | `0x0042B4EA` `0x0042B4F5` `0x0042B4FC` `0x0042B50E` | one function | same 4-rands-per-object shape at n=1 |
| 1 | `0x00426B27` | the `0x00426xxx` global-ctor neighbourhood | seed neighbourhood, next to `FUN_004269AF` |

`FUN_0049B6C7` is a constructor-shaped `CWnd`-derived class: `FUN_0049B769` is its ctor
(vtable `PTR_LAB_004D0E28`), and `FUN_0049B70F/4B71B/4B734/4B75D` are its accessors, each
calling `FUN_0049B6C7`; `FUN_0049B665` is the free-slot search. It is instantiated from
three sites — `FUN_00415EC4` (`0x00415F5B`), `FUN_00449EA3` (`0x00449EE5`) and
`FUN_004766EE` (`0x00476731`). I earlier read `0x004D5B30` and `0x004D9270` as
`CRuntimeClass` headers because both contain `0x19930520`; that was wrong -- the Oracle
read both structures properly and `m_nSchema` is 0 at each with an empty name CString, so
the magic sits elsewhere in them. The class name is not beside the vtable either: the
+/-0x600 bytes around `PTR_LAB_004D0E28` (file offset 0xD0028) contain no ASCII at all.
Where the 352 objects come from is still open; see section 8.3.

**`rng.calls` in map mode is pending the pump model, not permanently incomparable.** The
encounter roll `FUN_0046260E` is MapView-2's only rand site and it runs **per map tick** — 40
times a second at the 25 ms sub-gate while the hero is moving — drawing 1 or 2 rands per tick
depending on which clause short-circuits. So it is sensitive to the idle rate in 2.1, and a
host that idles more often than the port burns more of that stream. **That is a bug to fix,
not a property to accept:** identical behaviour is the goal, so the idle rate has to be ONE
deterministic model that both sides share. The Oracle is measuring what keeps the original's
queue non-empty — which messages, when — and when that model lands it gets implemented in the
port's pump so both sides idle on the same boundaries. Until then `rng.calls` is a
diagnostic in map mode and an assertion everywhere the pump is quiescent.

**Port rule.** The trace is byte-identical across runs, so this is a **boot constant, not a
per-frame rule**: the right shape is 352 EncInt constructions drawing four each in
construction order, not 1408 burned rands.

### 8.2a The CRT initialiser table — the construction ORDER

Entry point VA `0x004C5AF0`. It makes **two** `_initterm` calls, both through the thunk
`0x004C5CB0` (`jmp *0x00D8B570`). cdecl pushes right-to-left, so read the FIRST pushed value
as `first`:

| site | first | last | entries |
|---|---|---|---|
| `0x004C5B8F` | `0x004DC0D4` | `0x004DC0D8` | 1, and its slot holds `0x00000000` — effectively empty |
| `0x004C5BC8` | `0x004DC000` | `0x004DC0D0` | **52**, the real table |

The 52, **in call order** (this order is what `rng.state after boot` depends on):

```
 0 00000000  (null)                     26 0041DC1C
 1 004C5D59                              27 0041DC61
 2 00401101  <-- the 99-element EncInt vector ctor   28 0041DCA6
 3 00404FC5                              29 0041DCEB
 4 00405EC2                              30 0041DD30
 5 004089FF                              31 0041DD75
 6 0040AD26                              32 0041DDBA
 7 0040FADB                              33 0041DDFF
 8 0040FB0D                              34 0041DE22
 9 0040FB51                              35 0041DE67
10 0040FB51                              36 00421EBD
11 00410851                              37 00427D89
12 00410D9A                              38 0043BB8A  <-- calls FUN_0043BBD1, the 7-EncInt group
13 00415786                              39 00441534
14 00416CD3                              40 00441B5A
15 00419584                              41 0045ACA3
16 0041AD11                              42 00463258
17 0041D717                              43 00463604
18 0041D984                              44 004676D4
19 0041D9BF                              45 0046BAF9
20 0041DA04                              46 0046BAF9
21 0041DA49                              47 00472B74
22 0041DB00                              48 0048282E
23 0041DB2B                              49 0049E230
24 0041DB70                              50 004A3813
25 0041DBB5                              51 004A383E
```

Two things follow immediately, and the first is the important one:

* **The 99-element EncInt array is initialiser #2** — the second thing the CRT constructs,
  before essentially every other object in the program. So 396 of the 1408 boot draws happen
  almost at the very front of the stream, not wherever the owning module's initialiser happens
  to sit. The port cannot get this right by making each module build its own EncInts in its
  own initialiser; the order is global.
* The 7-EncInt group `FUN_0043BBD1` is initialiser **#38** (the thunk `0x0043BB8A` sits in
  `FUN_0043BBAE` and calls it directly at `0x0043BBC3`).

Two of the other groups are **not** in the table at all, which corrects my own earlier
framing of them as boot costs:

* `FUN_00413181` (the 10-EncInt pets group) is called from `0x00484FDB`, inside
  `FUN_00484E72` — the hero message-log routine — with zero direct callers of its own. So it
  is reached by **virtual dispatch at run time from the log path**, not by the CRT. Those 40
  draws are therefore *not* part of a fixed boot sequence; they happen when that code first
  runs, which may be during boot (it logs) or during play.
* `FUN_00444D64` (the 14-EncInt group, the largest) is called from `0x0043482C` inside
  `FUN_004347D6`, which also has zero direct callers and is not in the table — also runtime.

So the honest split is: **the 99-element vector array and the `FUN_0043BBD1` group are ordered
boot costs and are now placed; `FUN_00444D64` and `FUN_00413181` are runtime costs whose
trigger point is a virtual call I have not identified.** The boot total of 352 cannot be
reconciled against a static sum until the runtime ones are separated out, and I am not going
to assert a number I cannot show.

### 8.2b The 352 constructions: counts, and what is still missing

The count is solid. The **order is not**, and `rng.state after boot` depends on the order, so
this is the live blocker. What the binary gives:

* `FUN_00401125` is the `eh vector constructor iterator` element thunk (calls `FUN_0049B75D`,
  returns the pointer). One call site, `0x0040111A`, inside the MSVC thunk at `0x00401101`
  (`jmp 0x401106`) in `FUN_00401051`: base `DAT_0052C978`, count `0x63` = 99, stride `0x120`
  = 288 bytes. `FUN_00401137` memsets `0x7080` = 100 x 288 at the same address, so the region
  is one element larger than the 99 constructed. The element is a 288-byte class holding the
  EncInt plus ~232 bytes of other fields.
* The three `new` sites are **single constructions, not loops**: `FUN_00415EC4` (`0x00415F5B`),
  `FUN_00449EA3` (`0x00449EE5`), `FUN_004766EE` (`0x00476731`). 3 objects.
* The bulk is **unrolled groups**, not arrays. All 33 direct `FUN_0049B75D` call sites, by
  containing function: `FUN_00444D64` 14, `FUN_00413181` 10 (pets), `FUN_0043BBD1` 7,
  `FUN_00401125` 1, `FUN_0043380F` 1. **Note that the two largest, `FUN_00444D64` and
  `FUN_00413181`, are runtime rather than CRT costs** — see 8.2a.

99 + 3 = 102 accounted for; the other **250 come from those five routines being called more
than once each**, and the multipliers live in the callers, which dispatch through tables and
jumps rather than counted loops. **Not read, and not guessed.** Two routes remain, and the
second is the cheap one:

1. Walk the CRT initialiser table from the entry point (VA `0x004C5AF0`, `_initterm` at IAT
   `0x00D8B570`) and read the construction order directly.
2. Detour `FUN_0049B6C7` itself and log its first argument (the object pointer, ECX under
   `__fastcall`) on every call. It is in `Souls.exe`'s own `.text` with a fixed prologue, so
   it is patchable — it is not an export, which is why IAT patching cannot reach it. One run
   yields the 352 object addresses **in construction order**, confirms the 99/0x120 array from
   the live process, and turns each routine's multiplier from inferred into counted. Two
   cautions: the trampoline must execute the original prologue, and the trace handle must be
   opened in `DllMain` on an absolute path, because `InitInstance` does
   `SetCurrentDirectory(install root)`.

**Until the order is known, the port cannot get this right by construction, only by copying a
sequence I would be guessing at.** I would rather record that than ship a boot loop that draws
1408 numbers in an invented order and then reads as a match.

### 8.2b Boot rands: implemented, and what is still open

`src/game/sched.c` now carries two of them, because both run in offline solo play and
identical behaviour is the goal whatever the code was written for:
`sched_boot()` / `sched_append()` (the 128-entry table and the four boot appends, ids
14..17, count 30) and `srn_mix()` (the five-draw fold, looping while the fold is zero).
Verified in the log: `EVT sched_boot entries=4` and `EVT srn_mix draws=5`.

**Open, and I am not going to close it by guessing:**
* `FUN_00456D2F` calls `FUN_00456BE1` (0x00456BE1) immediately after `FUN_00456C51` — eight
  more appends, ids 5..12, same count 30. The offline trace shows **four** calls, not
  twelve, so either `FUN_00456BE1` did not run in that run or the trace predates it. Adding
  the eight would break rng parity; omitting them would break it the other way. I implemented
  the four the trace proves and flagged the eight rather than pick.
* `FUN_00456EC1` (0x00456EC1), the consumer reached from `FUN_00456D2F`, is **not read**.
  `sched_due()` implements the shape the writer implies — an entry is due when
  `clock_ms() - tick_ms` has passed `delay_ms` — and that is labelled as a known-open item,
  not a parity claim. The global table is `DAT_00CE2C54`, indexed `[DAT_004e0ddc * 0x40]`.
* The boot ORDER between `srn_mix()` (called from `0x004096E7`) and the first
  `FUN_00456D2F` is not established, so the port's two calls are adjacent rather than
  correctly interleaved.

The remaining table; none of these VAs belongs to any module or to the
harness (`tools/oracle/**` has never touched a game VA in `src/`).

| calls | caller | what it is | owner |
|---|---|---|---|
| 5+ | `0x0042B4EA` `0x0042B4F5` `0x0042B4FC` `0x0042B50E` (+1) | `FUN_0042B4E0` | the **SRN warm-up mixer**: `do { 5 x rand(); GetTickCount(); } while (fold == 0)`, `fold = ((((iVar1<<4 ^ uVar2)<<4 ^ uVar3)<<4 ^ iVar4<<16 ^ GetTickCount() ^ uVar6) & 0x3FFFFFFF)`. 5 draws per iteration, repeating while the fold is exactly zero -- almost always once, **not a fixed count**. One caller, `0x004096E7`. It mixes the clock and the RNG, so it must fold `clock_ms()`, not the host clock | **implemented: `srn_mix()` in src/game/sched.c** |
| 4 | `0x0042B4EA` `0x0042B4F5` `0x0042B4FC` `0x0042B50E` | one function, four `rand()` in a row — the same 4-rands-per-object shape as the EncInt seal at n=1 | **unowned** |
| 1 | `0x0048E1CF` | singleton | **unowned** |
| 1 | `0x00426B27` | `FUN_004269AF`, the same function as **both** `srand` sites, so the seed neighbourhood | **mine, placed** |
| 99 | the 288-byte-element array at `0x0052C978` | stride `0x120`, count `0x63` | **unowned** |
| 14 / 10 / 7 / 1 | `FUN_00444D64` / `FUN_00413181` / `FUN_0043BBD1` / `FUN_0043380F` | unrolled EncInt construction groups | `FUN_00413181` is Panels-2's; the rest **unowned** |

**Caveat on that table, from Panels-2's disassembly of `FUN_00413181` (0x00413181).** The 10
`FUN_0049B75D` constructions are only part of the cost. There are also **five
`FUN_0049B71B` sets**, so a pet spawn is **10 + 5 = 60 draws, not 40 and not 64**. The first
five ctors (`0x413191`-`0x4131BD`) seal the stack slots `EBP-0x128`, `EBP-0xF0`, `EBP-0xB8`,
`EBP-0x80`, `EBP-0x48`, and the five sets (`0x4132A7`-`0x41331D`) have ECX pointing at exactly
those same five slots, loading `[EDI+0xEC]`, `[EDI+0xF4]`, `[EDI+0xF8]`, `[EDI+0xFC]`,
`[EDI+0x100]` — the monster's five stat fields. **So the sealed set is `str`, `sta`, `agi`,
`dex`, `wis`; `level`, `hp` and `max_hp` are plain ints.** (I had it as the other way round
and was wrong; sealing the three plain ones would have cost 12 extra draws per pet and pushed
`rng.calls` away from the original.) The 40 unconditional draws happen **before** the
monster-id validation, so a rejected pet still burns them.

**Consequence for the 33-site table:** it counts *constructors* correctly but is the wrong
basis for any per-invocation total, because SET sites are not in it. The same error will
apply to the other unrolled groups until their bodies are read the same way.

**Not EncInt**, recorded as a negative result so nobody re-checks them: the trophy bag words
(`FUN_0046F726` / `FUN_0046F779`) are a plain XOR `0x1D43E217`, and items.txt plus heroes'
inventories are plain bytes.

## 8.3 The EncInt object, and the 352 that are still unnamed

`FUN_0049B6C7` is the seal half of the anti-cheat "encrypted int" object. Layout, from its
integer arithmetic, is 13 ints with three checksum doubles interleaved:

    +0  int32 v      the value          +24 double d1 = v * 1.4142   (_DAT_004d0d40)
    +4  int32 k0     rand draw 1        +32 int32  k2  draw 3
    +8  double d0 = v * 2.1459 (_DAT_004d0d38)   +40 double d2 = v * 0.0123 (_DAT_004d0d48)
    +16 int32 k1     rand draw 2        +48 int32  k3  draw 4

**`sizeof(EncInt)` is 56, not 52** (FrontHero-2 measured it): the doubles make the struct
8-aligned, so the last field ends at byte 52 and the tail pads to 56. The stride between the
ten sealed objects in `FUN_00413181` is `0x38` = 56, which confirms it against the binary. Any
offset table computed by summing the field offsets is 4 bytes short per object, and any struct
embedding several EncInts carries the same padding. `encint.h` has a `_Static_assert` on it.

**Construction direction is a property of the site, not a universal rule.** The boot array
thunk `FUN_00401101` walks ASCENDING. But `FUN_00413181` walks each of its two five-object
groups from the **highest offset downwards**: ECX runs `-0x128, -0xF0, -0xB8, -0x80, -0x48`,
then `-0x808, -0x7D0, -0x798, -0x760, -0x728`, stride `0x38` inside each group. Getting it
wrong shifts the whole session's stream, so `enc_construct_array()` now takes an explicit
`EncOrder` and the direction is not assumed.

`FUN_0049B665` is the verify half: it re-derives all three doubles from `v` and compares
them, and on a mismatch sets `DAT_004E709C`, calls `FUN_004A8664(0x424)` and `FUN_00449A17`.
**`k0..k3` are write-only -- nothing ever checks them.** The value is what is protected; the
keys exist to be part of the memory pattern a cheat tool has to recognise, and they still
cost four `rand()` per set. The seals are `FUN_0049B70F` get (0 rands), `FUN_0049B71B` set
(4), `FUN_0049B734` clear (4), `FUN_0049B73F` add (a get then a set, so 4), and
`FUN_0049B75D` is the constructor wrapper around clear.

`DAT_004E709C` is also the `== 0` guard on `FUN_0040A7C7`'s 20 ms gate, so a tampered object
stops the game's tick entirely. That coupling is worth stating: the anti-cheat flag and the
tick are the same variable.

**Still open:** which of the three instantiation sites
(`FUN_00415EC4` / `FUN_00449EA3` / `FUN_004766EE`) loops 352 times, and what the 352 objects
are. The Oracle's hook counts the calls exactly (352 x 4, byte-identical across runs) but
cannot see which object each call touched, because `FUN_0049B6C7` is game code with no IAT
slot. The remaining lead is the CRT initialiser table and the `eh vector constructor
iterator` entries that pass `FUN_00401125`.

## 9. Corrections to other documents

* **The shared plan and rng_calls.md both say six `SetTimer`/`KillTimer`.** The
  binary has **eight** `SetTimer` sites (§4). The extra two, `0x428803` and
  `0x4a1e18`, are invisible in `all.c` because Ghidra merged their functions.
  rng_calls.md's §3.4 table rows and its summary sentence need the correction.
* **The shared plan's "timeGetTime shares it" is moot**: there is no `timeGetTime`
  import. `clock_ms()` is the only tick.
* **`docs/re/battle.md` says the regen divisor is `/600` over frames**; the
  decomp says `/10000` over milliseconds (`FUN_00478840`), and `FUN_00478840` runs
  on the **paint** path in every state, not on a 20 Hz tick.
* **`FUN_0042c1d0` / 0x41370x `Sleep(3600000)`** and the `Sleep(5000)` anti-tamper
  penalty are both inside SRNet paths and are out of solo scope.

## 10. Still unknown

* The bodies of the nine unreachable `WM_TIMER` handlers (§4) — they may have been
  armed by a `SetTimer` in a code path the installer never executes.
* `FUN_00428360` (timer row 1) and `FUN_004a1b52` (timer row 8) are identified
  only by their neighbours; neither has an identifying string. Their handlers are
  named in §4 but their purpose is not established.
* Whether the original re-arms a WM_TIMER from its deadline or from the dispatch
  instant after a long stall is not decidable from the binary (Windows documents
  neither). We picked "from the deadline", both sides implement it, and §8 states
  it, so the two harnesses agree by construction rather than by evidence.
