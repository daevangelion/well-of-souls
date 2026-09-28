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

### 2.1 The tick is driven by a 100 ms WM_TIMER, not by an empty message queue

**A naive reading of the oracle trace says the 20 ms gate never fires.** Its
`gate=` read of `DAT_004DD510` is 0 for a whole run while 13 `rand()` calls appear at
`0x00428996` inside `FUN_0042895C`. It does not never fire, and the reason matters.

`FUN_0042895C` has **exactly one caller in the whole binary**: `0x0040A815`, inside
`FUN_0040A7C7` itself, immediately preceded by `mov %eax,0x004DD510` at `0x0040A810`. So the
13 draws prove the gate ran 13 times and the stamp was written 13 times — a hook reading it
as 0 throughout is seeing **its own virtualised `GetTickCount` return 0**, not a missing tick.
(I once mis-encoded this address when byte-scanning the image and wrongly declared the
`_DAT_` names fictional. They are real: `0x004DD510` occurs twice in the image,
`0x004DD0F0` six times, `0x004E48CC` twice. The retraction was mine to withdraw.)

There are exactly **two** callers of `FUN_0040A7C7`:

| VA | in | what |
|---|---|---|
| `0x0040A90F` | `FUN_0040A8D9` (Run) | the idle path, when `PeekMessageA` returns nothing |
| `0x00428CAF` | `FUN_00428C8F` | **the main frame's 100 ms `WM_TIMER` handler** |

```c
void __fastcall FUN_00428C8F(CWnd *self) {
    if ((DAT_004E48A0 == 0) && (DAT_004E709C == 0)) {   /* re-entrancy, then the cheat flag */
        DAT_004E48A0 = 1;
        FUN_0040a7c7();          /* the 20 ms gate */
        DAT_004E48A0 = 0;
    }
    CWnd::Default(self);
}
```

`FUN_00428360` arms it: `SetTimer(hwnd, 0x16, 100, NULL)` at `0x00428803`, slot `this+0x114`,
after calling `FUN_004094E7` first. **The world tick rides that 100 ms timer.** This
reconciles everything: the queue genuinely is almost never empty on a repainting screen, the
gate still fires anyway, and the 13 draws are 13 timer-driven passes. `DAT_004E48A0` is a
re-entrancy guard set around the call in both paths.

**Port rule:** arm `clock_set_timer(frame, 0x16, 100, ...)` at the original's point and have
its handler do `crt_rand()` (FUN_0042895C's discarded draw) then the 25 ms
`clock_gate(..., 25)`. The port's `clock_dispatch_timers` loop already prevents the
re-entrancy `DAT_004E48A0` guards. The other live timer at the title — id 2 on the
"Book of Tactics" dialog, from `0x00455F6D` — is online-only unless proven otherwise.

Because the tick is timer-driven rather than queue-driven, the port's world step runs at the
same rate on a busy screen as on a quiet one, which is what makes `rng.calls` comparable
between the two sides in map mode and everywhere else. The earlier concern that idle rate was
"host-dependent and therefore not comparable" is withdrawn: it was based on reading the gate
as clock-driven.

**Port rule.** The port arms the main frame's id-0x16 100 ms timer and runs `crt_rand()` plus
the 25 ms world gate inside its handler. `clock_idle_due()` / `clock_20hz_next()` remain for
callers that want the 20 ms limiter, but the world's tick is timer-driven, which is what makes
it match the original on a screen that never goes idle.


