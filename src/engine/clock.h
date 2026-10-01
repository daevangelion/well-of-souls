/* The original's timing model, emulated.
 *
 * Souls.exe (MSVC6 + MFC42) has NO simulation frame loop. Its driver is
 * CWinApp::Run() at 0x0040a8d9, which is a bare PeekMessage pump:
 *
 *   do {                                   // outer: restart from the idle path
 *     c = FUN_00449a49();
 *     for (;;) {                           // inner: idle while the queue is EMPTY
 *       if (PeekMessageA(&msg,0,0,0,PM_NOREMOVE)) break;
 *       CWinApp::OnIdle(++n);              // vtable+0x68
 *       if (FUN_0040a7c7() && FUN_00467312(0x1a)) break;   // 0x0040a7c7 -> 0x00467312
 *       ... foreground-window tracking (DAT_004dd50c) ...
 *       if (!foreground_is_us) break;
 *     }
 *     for (;;) {                           // dispatch while a message is present
 *       ...0x0040a99e tick bookkeeping, PostQuitMessage, IsIdleMessage,
 *          PreTranslateMessage (vtable+0x6c)...
 *       if (!PeekMessageA(&msg,0,0,0,PM_NOREMOVE)) break;
 *     }
 *   } while (1);
 *
 * So the order is exactly: INPUT -> TIMER -> IDLE/PAINT, and idle only runs
 * when the message queue is empty. That is the semantics this module provides.
 *
 * The original imports GetTickCount (IAT 0x0098ae24) but NOT timeGetTime
 * (absent from the PE import table: verified by parsing the import directory,
 * which also shows GetMessageA/DispatchMessageA/TranslateMessage absent).
 * The tick sources that exist are GetTickCount, QueryPerformanceCounter
 * (0x0042895c, netgraph only) and time() (MSVCRT, anti-tamper only).
 * Virtual ms therefore backs all of them; see docs/re/timing.md.
 *
 * Owner: Core. */
#ifndef WOS_CLOCK_H
#define WOS_CLOCK_H

#include <stdint.h>

/* Virtual GetTickCount(): milliseconds since the port's t0. Wraps like Win32. */
uint32_t clock_ms(void);
/* The original's time(NULL): the WALL clock in seconds, i.e. the same value as
 * plat_time_s(), pin included. It is deliberately not derived from the virtual
 * millisecond clock: FUN_00409722's date check and the two boot srand calls read
 * time(), and the differential harness pins it. */
uint32_t clock_time_s(void);
/* Real host milliseconds, for the interactive pacing only. Never used for rules. */
uint32_t clock_real_ms(void);

/* --- driving the clock -----------------------------------------------------
 * headless/script:  clock_advance(ms) to the script's next event time.
 * interactive:      clock_attach_realtime() makes clock_ms() track the host. */
void clock_advance(uint32_t ms);        /* now += ms; also moves clock_time_s() */
void clock_set_now(uint32_t now_ms);    /* absolute; ignored if it moves backwards */
void clock_attach_realtime(void);       /* clock_ms() follows the host clock again */
/* A blocking call inside a handler (the original's Sleep): the clock jumps `ms` ahead in
 * either mode, so timers come due late and coalesce exactly as after a real stall. */
void clock_stall(uint32_t ms);
void clock_set_time_base(uint32_t epoch_s); /* time(0) at t0; 0 by default */
void clock_reset(void);                 /* t0=0, epoch base 0, all timers killed */

/* --- Win32 SetTimer/KillTimer emulation ------------------------------------
 * The original has exactly eight SetTimer call sites (docs/re/timing.md table
 * 2); Win32 semantics: setting a live (owner,id) replaces it and returns the
 * SAME id, so a module only ever needs the constant. */
typedef void (*ClockTimerFn)(void *owner, void *user);
void clock_set_timer(void *owner, int id, uint32_t interval_ms, ClockTimerFn fn, void *user);
void clock_kill_timer(void *owner, int id);
int  clock_timer_active(void *owner, int id);

/* Deliver every WM_TIMER that is due at the current virtual time, oldest
 * deadline first and ties broken by (owner,id). Coalescing: a timer that is
 * k intervals overdue fires ONCE and its next deadline becomes
 * deadline+interval, never replayed k times. Returns the number fired.
 * The virtual clock is moved FORWARD to each timer's deadline as it fires, so
 * an early timer sees the clock at its own deadline. It never rewinds: a timer
 * that is already overdue fires with the clock at the present, exactly as
 * GetTickCount() reports now rather than the message's timestamp. */
int  clock_dispatch_timers(void);

/* The 20 Hz idle gate of FUN_0040a7c7 (0x0040a7c7): `GetTickCount() - last > 19`,
 * i.e. the idle tick fires at most once per 20 ms of virtual time. Returns 1
 * and consumes one tick when an idle tick is due.
 *
 * The stamp is re-armed with the OBSERVED time, exactly as FUN_0040a7c7 does
 * (`_DAT_004dd510 = GetTickCount()`), so a stall of N ms costs ONE idle tick, not
 * floor(N/20). The original calls GetTickCount twice per idle pass and never
 * replays the boundaries it slept through, and the differential harness must
 * match that even on a spin-wait path where the virtual clock jumps.
 * Determinism under a script comes from the loop, not from this gate: game_main.c
 * steps the clock onto each 20 ms boundary with clock_20hz_next(), so the gate
 * sees now - last == 20 and fires exactly once per boundary. */
int  clock_idle_due(void);
/* Virtual ms of the next 20 ms idle boundary (last stamp + 20). */
uint32_t clock_20hz_next(void);
/* The same boundary WITHOUT catching the stamp up: last stamp + 20, or now when the gate
 * is already due. This is the oracle pump's t_idle; the stamp only moves when the gate
 * fires, so `now - last > 19` sees the boundary exactly. */
uint32_t clock_idle_next(void);
/* Earliest deadline of a live timer with no message outstanding; UINT32_MAX when none. */
uint32_t clock_next_timer_deadline(void);

/* A private rate gate: the `if (GetTickCount() - last < N) skip;` idiom the
 * original uses everywhere. Returns 1 exactly once per N ms of virtual time
 * and re-arms from the previous boundary, so a clock jump of M ms costs
 * floor(M/N) ticks rather than one. (The original re-stamps from the observed
 * tick instead; the two agree whenever the loop does not jump, and this one
 * keeps the tick COUNT deterministic under a script.)
 *
 * FUN_0042895C's gate is the one every module needs: `N == 25`. It sits between
 * the 20 ms idle tick and the whole game world, so the world step is 40 Hz, not
 * 60 Hz and not 20 Hz. See docs/re/timing.md section 2.1. */
int  clock_gate(void *owner, int id, uint32_t interval_ms);
/* The same gate with the original's own re-stamp, `last = GetTickCount()` (NetGraphTick
 * 0x4289B7 stores the observed tick into _DAT_004e48cc). Polled from the 20 ms idle gate
 * it passes every 40 ms, not every 25: the oracle's world stamps are 300, 340, 380, ... */
int  clock_gate_restamp(void *owner, int id, uint32_t interval_ms);

#endif
