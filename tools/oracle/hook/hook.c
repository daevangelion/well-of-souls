/* tools/oracle/hook/hook.c -- WoS oracle hook DLL (i686 PE, injected into Souls.exe)
 *
 * Runs the original Well of Souls binary deterministically so its state can be diffed
 * against the portable C port.  Nothing on disk is modified: only the live process
 * image is patched, and every patch is undone by exiting the process.
 *
 * What is hooked, and why:
 *   virtual clock   kernel32 GetTickCount/GetSystemTime/GetLocalTime, msvcrt
 *                   time/_time64/localtime/ctime/mktime
 *   the MSVC6 LCG   msvcrt rand/srand, reimplemented here, counted, caller traced
 *   the timer table user32 SetTimer/KillTimer, with Win32 coalescing semantics
 *   the message pump user32 PeekMessageA/GetMessageA: the virtual clock advances only
 *                   while the queue is empty; scripted input and WM_TIMERs are posted
 *                   from there.  user32 GetKeyState/GetAsyncKeyState report "no key".
 *   the exit path   user32 WinHelpA is neutered so ExitInstance cannot block
 *
 * WHY MODULE-LEVEL DETOURS AND NOT IAT PATCHING.  The exe's import table has no
 * GetMessageA/DispatchMessageA/TranslateMessage and no timeGetTime (checked against the
 * PE import directory, not the decomp): the message pump lives inside MFC42.DLL and in
 * the MFC modal loops.  Patching Souls.exe's IAT would therefore miss the one function
 * that drives everything.  Instead each hooked function keeps the exact prototype of the
 * original, so the hook is an ordinary stdcall/cdecl function, and the installed
 * trampoline is six bytes -- push imm32 ; ret.  The original prologue is never executed,
 * so no length disassembler and no copied bytes are needed.  The module's code section
 * is VirtualProtect'ed to RWX first, which under Wine forces a private copy.
 *
 * The event loop this reproduces is CWinApp::Run at 0x0040A8D9, whose idle gate is
 * FUN_0040A7C7 at 0x0040A7C7 (`GetTickCount() - _DAT_004dd510 > 19`).  See
 * docs/re/oracle.md for the step rule and the key registry.
 *
 * Souls.exe addresses used here (ImageBase 0x400000):
 *   0x004DD510  _DAT_004dd510, the 20 Hz stamp (read to get the gate boundary exactly)
 *   0x004DF8A4  DAT_004df8a4, the front-end state
 *   0x004E0BD0  DAT_004e0bd0, the current world name
 *   0x004DD20C  DAT_004dd20c, the serial / player id
 *   0x004E6910  DAT_004e6910, the SRNet network type (0 = solo)
 *   0x004E0DDC  DAT_004e0ddc, the current map number
 *   0x0067FBF8  the hero table, 100 records of 0x16CC bytes
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <tlhelp32.h>

#define IMAGE_BASE     0x00400000u
#define VA_GATE_STAMP  0x004DD510u
#define GATE_PERIOD    20u                 /* FUN_0040a7c7 tests > 19 */
#define IDLE_QUANTUM   20u                 /* fallback when the stamp is unusable */
#define VIRTUAL_EPOCH  1234567890L         /* fixed, so time() is reproducible */
#define MAX_TIMERS     64
#define MAX_EVENTS     4096
#define CLIENT_W       640
#define CLIENT_H       480

/* ------------------------------------------------------------- virtual clock */

static volatile LONG g_ms;
static uint32_t       g_epoch = VIRTUAL_EPOCH;

static uint32_t vnow(void)      { return (uint32_t)InterlockedCompareExchange(&g_ms, 0, 0); }
static uint32_t vtime_s(void)   { return g_epoch + (uint32_t)g_ms / 1000u; }
static void     vadvance(uint32_t target)
{
    if (vnow() < target) InterlockedExchange(&g_ms, (LONG)target);
}

/* days_from_civil / civil_from_days, Howard Hinnant's algorithms, UTC only.
 * The harness runs with TZ=UTC so the original's localtime is reproducible; these
 * replace it rather than trusting the host's locale or leap-second table. */
static int64_t days_from_civil(int y, int m, int d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (int64_t)y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int *py, int *pm, int *pd)
{
    int64_t era, yoe, doy, doe;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    *py = (int)(yoe + era * 400);
    doy = (153 * (doy > 59 ? doy - 2 : doy + 59) + 2) / 5 + 1;
    *pm = (int)((doy > 11 ? doy - 12 : doy + 12) % 12 + 1);
    *pd = (int)(doy);
    if (*pm <= 2) (*py)++;
}
static void tr(const char *fmt, ...);
/* See the DETOUR below for why Sleep is virtual rather than real. */
static VOID WINAPI hook_Sleep(DWORD ms)
{
    if (!ms) { vadvance(vnow()); return; }     /* a yield, not a wait */
    tr("sleep now=%u ms=%lu ra=%p", vnow(), (unsigned long)ms, __builtin_return_address(0));
    vadvance(vnow() + ms);
}

static void vfill_systemtime(SYSTEMTIME *st)
{
    uint32_t t = vtime_s();
    int64_t  days = (int64_t)(t / 86400u);
    unsigned rem = t % 86400u;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    st->wYear         = (WORD)y;
    st->wMonth        = (WORD)m;
    st->wDay          = (WORD)d;
    st->wDayOfWeek    = (WORD)(((t / 86400u) + 4u) % 7u);   /* 1970-01-01 = Thursday */
    st->wHour         = (WORD)(rem / 3600u);
    st->wMinute       = (WORD)((rem / 60u) % 60u);
    st->wSecond       = (WORD)(rem % 60u);
    st->wMilliseconds = (WORD)(g_ms % 1000u);
}

/* ------------------------------------------------------------------- the LCG */

static uint32_t g_holdrand;
static volatile LONG g_rand_calls;
static volatile LONG g_srand_calls;
static void tr(const char *fmt, ...);
static volatile LONG g_rand_trace;
/* One handle, opened once.  Two reasons, both learned the hard way: an fopen per
 * call costs more than the call, and a RELATIVE name lands wherever the CWD
 * happens to be -- the game does SetCurrentDirectory(install root) in
 * InitInstance, so the first 1408 boot rands were writing into the launcher's
 * directory and the rest into the game directory.  WOS_TRACE names the file
 * explicitly; it must be a Windows path. */
static HANDLE g_traceh = INVALID_HANDLE_VALUE;

static void trace_open(void)
{
    char path[512];
    DWORD n = GetEnvironmentVariableA("WOS_TRACE", path, sizeof path);
    if (!n || n >= sizeof path) strcpy(path, "randtrace.txt");
    g_traceh = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    tr("DllMain: randtrace=%d file=%s handle=%p", (int)g_rand_trace, path, (void *)g_traceh);
}
static void trace_line(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    DWORD n;
    if (g_traceh == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = (DWORD)vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n > sizeof buf - 2) n = sizeof buf - 2;
    buf[n] = '\r'; buf[n + 1] = '\n';
    WriteFile(g_traceh, buf, n + 2, &n, NULL);
}

/* ---------------------------------------------- the EncInt seal, FUN_0049B6C7
 *
 * 1408 of the 1420 boot rands are this one function: the anti-cheat "encrypted int"
 * setter, which writes three verification doubles and then draws four key ints.  It
 * lives in Souls.exe's own .text, so it cannot be reached with an IAT hook -- there is
 * no import slot anywhere on the path -- and the caller address in the rand trace says
 * only that "something" called it 352 times, not which of the half-dozen construction
 * routines did it or in what order.
 *
 * The prologue is `SUB ESP,4 ; PUSH ESI ; PUSH EDI` (0x49B6C7..0x49B6CC, six bytes), so
 * the same six-byte push-imm32/ret trampoline the IAT hooks use lands exactly on an
 * instruction boundary, and those three instructions are replayed in the hook before it
 * jumps back to 0x49B6CC.  `this` is ECX (__fastcall); the caller's return address is
 * the word at the top of the stack on entry.
 *
 * The four-deep chain is a STACK SCAN, not an EBP walk: MSVC /O2 omits frame pointers in
 * nearly everything here, so the EBP chain dies at the first leaf.  Anything in
 * [0x401000,0x500000) on the stack is a return address into the game's .text, which is
 * a filter with no false positives worth worrying about -- the only other things on a
 * 32-bit stack in that range are the vtable pointers and constants the game pushes.
 */
#define VA_ENC_SEAL  0x0049B6C7u
#define ENC_SEAL_RESUME 0x0049B6CEu
static volatile LONG g_enc_trace;
static volatile LONG g_enc_n;

static void enc_log(const void *self, const void *ret);
extern uintptr_t g_base;

/* The thunk is emitted rather than written in a top-level asm block: two of its
 * instructions carry a rel32 that is only known once the image base and the compiled
 * addresses are, and building it here keeps the displacement honest without a separate
 * .s file.  Layout, with E the stack pointer on entry (so [E] is the return address the
 * original would have returned to):
 *
 *   83 EC 04            subl   $4,%esp        (E-4)  <- displaced instruction 1, and
 *                                                     the 4-byte alignment the call needs
 *   8B 44 24 04         movl   4(%esp),%eax          the caller's return address
 *   51                  pushl  %ecx          (E-8)  <- `this`, parked where the call
 *                                                     below can still reach it
 *   50                  pushl  %eax          (E-12)
 *   E8 <rel32>          call   enc_log                (cdecl: the ret, then the this)
 *   83 C4 04            addl   $4,%esp        (E-8)
 *   59                  popl   %ecx          (E-4)  <- `this` back: enc_log is ordinary
 *                                                     C, so ECX is caller-saved, and
 *                                                     without this pop the `fild` below
 *                                                     dereferences a garbage pointer and
 *                                                     the game dies at the first seal
 *   56                  pushl  %esi          (E-8)  <- displaced instructions 2 and 3
 *   57                  pushl  %edi          (E-12)
 *   DB 09               fild   (%ecx)                 <- displaced instruction 4
 *   E9 <rel32>          jmp    0x0049B6CE             <- ESP is now exactly what the
 *                                                     original had at 0x0049B6CE
 *
 * The patch is SEVEN bytes, not six.  The push-imm32/ret trampoline is six, and six
 * would end at 0x0049B6CC -- which is the first byte of the `fild`, and therefore the
 * instruction the thunk is about to jump back to.  Displacing the `fild` too and
 * replaying it here costs two bytes and removes the possibility entirely.
 */
static void *enc_make_thunk(void)
{
    static const unsigned char head[] = {
        0x83, 0xEC, 0x04,             /* subl  $4,%esp   */
        0x8B, 0x44, 0x24, 0x04,       /* movl  4(%esp),%eax */
        0x51,                         /* pushl %ecx      -- `this`, saved across the call */
        0x50,                         /* pushl %eax      -- the caller's return address */
        0xE8, 0, 0, 0, 0,             /* call  enc_log   */
        0x83, 0xC4, 0x04,             /* addl  $4,%esp   */
        0x59,                         /* popl  %ecx      -- `this` back in ECX */
        0x56,                         /* pushl %esi      */
        0x57,                         /* pushl %edi      */
        0xDB, 0x09,                   /* fild  (%ecx)    */
        0xE9, 0, 0, 0, 0              /* jmp   resume    */
    };
    unsigned char *p;
    /* the rel32 fields start one past their E8/E9 opcode: call at index 9, jmp at 22 */
    int call_at = 10, jmp_at = 23;
    intptr_t here, target;
    p = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!p) return NULL;
    memcpy(p, head, sizeof head);
    /* the rel32 is measured from the END of the call/jmp, i.e. four bytes past the
     * field's own offset, so `here` is field+4 and not field */
    here   = (intptr_t)(p + call_at + 4);
    target = (intptr_t)enc_log;
    memcpy(p + call_at, &(int32_t){ (int32_t)(target - here) }, 4);
    here   = (intptr_t)(p + jmp_at + 4);
    target = (intptr_t)(g_base + (ENC_SEAL_RESUME - IMAGE_BASE));
    memcpy(p + jmp_at, &(int32_t){ (int32_t)(target - here) }, 4);
    tr("enc: thunk at %p, enc_log=%p, resume=%08lX", (void *)p, (void *)enc_log,
       (unsigned long)target);
    return p;
}

static void enc_log(const void *ret, const void *self)   /* cdecl, in the thunk's push order */
{
    unsigned *sp;
    char line[320];
    int n, i, got;
    if (!g_enc_trace) return;
    n = snprintf(line, sizeof line, "enc %ld this=%08X ret=%08X chain",
                 (long)InterlockedIncrement(&g_enc_n),
                 (unsigned)(uintptr_t)self, (unsigned)(uintptr_t)ret);
    __asm__ __volatile__("movl %%esp, %0" : "=r"(sp));
    got = 0;
    for (i = 0; i < 64 && n < (int)sizeof line - 12; i++) {
        unsigned v = sp[i];
        if (v >= 0x00401000u && v < 0x00500000u) {
            if (v == (unsigned)(uintptr_t)ret) continue;
            n += snprintf(line + n, sizeof line - n, " %08X", v);
            if (++got == 8) break;
        }
    }
    trace_line("%s", line);
}

/* push <addr> ; ret at a .text entry point.  The same shape as the IAT trampoline, but
 * the number of bytes displaced is a parameter, because the six-byte form only lands
 * on an instruction boundary when the caller picked the displacement length to match
 * (see enc_make_thunk: six would overwrite the `fild` the thunk jumps back to). */
static int patch_text(uintptr_t va, void *target, int len)
{
    unsigned char *p = (unsigned char *)va;
    DWORD old = 0;
    if (len < 6) return 0;
    if (!VirtualProtect(p, len + 8, PAGE_EXECUTE_READWRITE, &old)) {
        tr("enc: VirtualProtect(%08lX) failed, err=%lu", (unsigned long)va, GetLastError());
        return 0;
    }
    p[0] = 0x68;                                  /* push imm32 */
    memcpy(p + 1, &target, 4);
    p[5] = 0xC3;                                  /* ret */
    if (len > 6) memset(p + 6, 0x90, (size_t)(len - 6));   /* nop the rest */
    VirtualProtect(p, len + 8, old, &old);
    tr("enc: %08lX -> %p, %d bytes displaced", (unsigned long)va, target, len);
    return 1;
}

/* g_base is the reloaded image base; it is declared here because the module-base
 * machinery further down has not been reached at this point in the file. */
extern uintptr_t g_base;


static void install_enc_trace(void)
{
    uintptr_t va = (uintptr_t)(g_base + (VA_ENC_SEAL - IMAGE_BASE));
    if (patch_text(va, enc_make_thunk(), 7)) g_enc_trace = 1;
}

/* A bounded set of "already reported" return addresses, so the chain scan reports each
 * distinct caller once for the whole run instead of on all 1420 calls. */
static unsigned g_sites[512];
static int g_nsites;
static int site_seen(unsigned v)
{
    int i;
    for (i = 0; i < g_nsites; i++) if (g_sites[i] == v) return 1;
    if (g_nsites < 512) g_sites[g_nsites++] = v;
    return 0;
}

static int __cdecl hook_rand(void)
{
    g_holdrand = g_holdrand * 214013u + 2531011u;
    InterlockedIncrement(&g_rand_calls);
    if (g_rand_trace) {
        /* The site alone cannot separate a 4-call routine from a 352-iteration loop, and
         * it cannot say WHICH construction routine ran.  A stack scan for the first
         * unseen caller gives both, once per site, which is the whole boot-rand table. */
        unsigned *sp;
        void *ra = __builtin_return_address(0);
        char chain[80];
        int i, got = 0, cn = 0;
        __asm__ __volatile__("movl %%esp, %0" : "=r"(sp));
        chain[0] = 0;
        for (i = 0; i < 64 && got < 4; i++) {
            unsigned v = sp[i];
            if (v >= 0x00401000u && v < 0x00500000u && v != (unsigned)(uintptr_t)ra &&
                !site_seen(v)) {
                site_seen(v);
                cn += snprintf(chain + cn, sizeof chain - cn, " %08X", v);
                if (++got == 8) break;
            }
        }
        /* The virtual ms goes LAST so readers keyed on "<index> <site>" still parse. */
        trace_line("%d %08X%s t=%u", (int)g_rand_calls, (unsigned)(uintptr_t)ra, chain,
                   (unsigned)vnow());
    }
    return (int)((g_holdrand >> 16) & 0x7FFFu);
}
static void __cdecl hook_srand(unsigned seed)
{
    InterlockedIncrement(&g_srand_calls);
    g_holdrand = seed;
}

/* ------------------------------------------------------------- the timer table */

typedef struct {
    HWND      hwnd;
    UINT_PTR  id;
    uint32_t  interval;
    uint32_t  deadline;
    uint32_t  last_delivered;   /* virtual ms of the previous delivery, for the gap log */
    int       live;
    int       outstanding;
} Timer;

static Timer g_timers[MAX_TIMERS];

static Timer *timer_find(HWND hwnd, UINT_PTR id)
{
    int i;
    for (i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].live && g_timers[i].hwnd == hwnd && g_timers[i].id == id)
            return &g_timers[i];
    return NULL;
}
static Timer *timer_alloc(HWND hwnd, UINT_PTR id)
{
    int i;
    for (i = 0; i < MAX_TIMERS; i++)
        if (!g_timers[i].live) {
            memset(&g_timers[i], 0, sizeof g_timers[i]);
            g_timers[i].live = 1; g_timers[i].hwnd = hwnd; g_timers[i].id = id;
            return &g_timers[i];
        }
    return NULL;
}
static uint32_t timer_next_deadline(void)
{
    uint32_t best = 0xFFFFFFFFu; int i, any = 0;
    for (i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].live && !g_timers[i].outstanding) {
            any = 1;
            if (g_timers[i].deadline < best) best = g_timers[i].deadline;
        }
    return any ? best : 0xFFFFFFFFu;
}
static int timer_live_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_TIMERS; i++) if (g_timers[i].live) n++;
    return n;
}

static UINT_PTR __stdcall hook_SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC proc)
{
    Timer *t = timer_find(hwnd, id);
    (void)proc;
    if (!t) t = timer_alloc(hwnd, id);
    if (!t) return 0;
    t->interval  = elapse ? elapse : 1u;
    t->deadline  = vnow() + t->interval;
    t->outstanding = 0;
    return (UINT_PTR)t;                    /* SetTimer's return is not checked anywhere */
}

/* THE APP TOOK A WM_TIMER. This is the missing half of the timer model, and it was a
 * one-line omission with a large effect: `outstanding` was set when a WM_TIMER was posted
 * and cleared ONLY in hook_SetTimer, i.e. only when the timer was (re)armed. So the first
 * post of every timer latched it forever and it could never fire again.
 *
 * Measured consequence, and it is not a subtle one: the main frame's 100 ms timer
 * (id 0x16, FUN_00428360 at 0x428803) and the Book of Tactics dialog's id 2 were each
 * delivered EXACTLY ONCE in a whole run -- 1,416,245 PeekMessage calls and 472,165
 * GetMessage calls, and one delivery. Every timed transition in the game was starved,
 * which includes the front end's state 2 ("Where Do You Want To Play Today?"), which
 * advances through the FUN_004057D3 gate inside FUN_0041BDB4 rather than on a click.
 *
 * The re-arm rule is Win32's and is the one already agreed: `deadline += interval`, from
 * the DEADLINE and not from the dispatch instant, so the two readings differ by exactly
 * the coalesced backlog. The `outstanding` flag is what stops a backlog being replayed:
 * one message per period at most, and a stall collapses to a single delivery. */
static void timer_taken(HWND hwnd, WPARAM id)
{
    int i;
    for (i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timers[i];
        if (t->live && t->hwnd == hwnd && t->id == id) {
            t->outstanding = 0;
            t->deadline += t->interval;
            return;
        }
    }
}
static BOOL __stdcall hook_KillTimer(HWND hwnd, UINT_PTR id)
{
    Timer *t = timer_find(hwnd, id);
    if (t) t->live = 0;
    return 1;
}

/* --------------------------------------------------------------- time hooks */

static DWORD  __stdcall hook_GetTickCount(void) { return vnow(); }
static void   __stdcall hook_GetSystemTime(LPSYSTEMTIME st) { vfill_systemtime(st); }
static void   __stdcall hook_GetLocalTime(LPSYSTEMTIME st)  { vfill_systemtime(st); }
static time_t __cdecl hook_time(time_t *t) { time_t v = (time_t)vtime_s(); if (t) *t = v; return v; }
static __time64_t __cdecl hook__time64(__time64_t *t) { __time64_t v = (__time64_t)vtime_s(); if (t) *t = v; return v; }
static struct tm *__cdecl hook_localtime(const time_t *t)
{
    time_t v = t ? *t : (time_t)vtime_s();
    int64_t days = (int64_t)(v / 86400);
    int y, m, d;
    static struct tm buf;
    civil_from_days(days, &y, &m, &d);
    buf.tm_sec   = (int)(v % 60);
    buf.tm_min   = (int)((v / 60) % 60);
    buf.tm_hour  = (int)((v / 3600) % 24);
    buf.tm_mday  = d;
    buf.tm_mon   = m - 1;
    buf.tm_year  = y - 1900;
    buf.tm_wday  = (int)(((v / 86400) + 4) % 7);
    buf.tm_yday  = (int)(v / 86400) - (int)days_from_civil(y, m, d);
    buf.tm_isdst = 0;
    return &buf;
}
static char *__cdecl hook_ctime(const time_t *t)
{
    static char b[32];
    time_t v = t ? *t : (time_t)vtime_s();
    snprintf(b, sizeof b, "%s", "");
    {
        struct tm *tmv = hook_localtime(&v);
        static const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *mo[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
        snprintf(b, sizeof b, "%s %s %2d %02d:%02d:%02d %d\n",
                 wd[tmv->tm_wday % 7], mo[tmv->tm_mon % 12], tmv->tm_mday,
                 tmv->tm_hour, tmv->tm_min, tmv->tm_sec, tmv->tm_year + 1900);
    }
    return b;
}
static time_t __cdecl hook_mktime(struct tm *tmv)
{
    if (!tmv) return (time_t)-1;
    return (time_t)(days_from_civil(tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday) * 86400
                    + tmv->tm_hour * 3600 + tmv->tm_min * 60 + tmv->tm_sec);
}

/* ------------------------------------------------------- keyboard and exit path */

static SHORT __stdcall hook_GetKeyState(int vk)      { (void)vk; return 0; }
static SHORT __stdcall hook_GetAsyncKeyState(int vk) { (void)vk; return 0; }
static BOOL  __stdcall hook_WinHelpA(HWND h, LPCSTR f, UINT c, DWORD x)
{ (void)h; (void)f; (void)c; (void)x; return 1; }

static void tr(const char *fmt, ...);
uintptr_t g_base;

/* --- WOS_DETOURS group "probe": a one-shot introspection of the live process. ---
 * The options table DAT_004F2A58 lives in .bss and is filled by a start-up constructor,
 * so it is only reachable at run time; this is how it is read out of the original. */
static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                           HWND, HMENU, HINSTANCE, LPVOID);
static int g_probed;

static void probe_tables(void)
{
    uintptr_t b = g_base;
    int n = *(int *)(b + (0x004F2BD8u - IMAGE_BASE));
    int i;
    tr("probe: option count=%d (DAT_004F2BD8)", n);
    for (i = 0; i < n && i < 64; i++) {
        int *rec = (int *)(b + (0x004F2A58u - IMAGE_BASE) + i * 12);
        tr("probe: option %d id=%d default=%d label=\"%s\"", i, rec[0], rec[1],
           rec[2] ? (const char *)rec[2] : "(null)");
    }
    {
        char line[512]; int k, n2 = 0;
        line[0] = 0;
        for (k = 0; k < 33; k++)
            n2 += wsprintfA(line + n2, "%s%d", k ? "," : "",
                            *(int *)(b + (0x006840D0u - IMAGE_BASE) + k * 4));
        tr("probe: values=%s", line);
    }
    tr("probe: serial=%d network=%d state=%d map=%d world=\"%s\"",
       *(int *)(b + (0x004DD20Cu - IMAGE_BASE)), *(int *)(b + (0x004E6910u - IMAGE_BASE)),
       *(int *)(b + (0x004DF8A4u - IMAGE_BASE)), *(int *)(b + (0x004E0DDCu - IMAGE_BASE)),
       (char *)(b + (0x004E0BD0u - IMAGE_BASE)));
    tr("probe: frame=%p hwnd=%p", *(void **)(b + (0x004E4840u - IMAGE_BASE)),
       *(HWND *)(b + (0x004E4840u - IMAGE_BASE) + 0x20));
    g_probed = 1;
}

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR ttl, DWORD st,
                                        int x, int y, int w, int h, HWND parent,
                                        HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r = real_CreateWindowExA(ex, cls, ttl, st, x, y, w, h, parent, menu, inst, param);
    tr("probe: CreateWindowExA class=\"%s\" title=\"%s\" %dx%d -> %p", cls ? cls : "",
       ttl ? ttl : "", w, h, (void *)r);
    /* MFC passes CW_USEDEFAULT (-2147483648) for the frame, so trigger on the caption
     * instead: it is IDR_MAINFRAME's, and only the main frame carries it. */
    if (!g_probed && r && ttl && strstr(ttl, "Well of Souls")) probe_tables();
    return r;
}

/* --------------------------------------------------------------- the installer */

static int g_detoured;

static void tr(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * HOW THE HOOKS ARE INSTALLED: import-address-table patching, everywhere.
 *
 * The exe's IAT is not enough.  The message pump lives inside MFC42.DLL and in MFC's
 * modal loops, so Souls.exe's import table has no GetMessageA at all (nor
 * DispatchMessageA, TranslateMessage or timeGetTime -- checked against the PE import
 * directory, not the decomp).  Patching only the exe would miss the one function that
 * drives everything.
 *
 * An earlier version overwrote six bytes at each export with `push imm32; ret`.  That is
 * compact, needs no relocation fixups, and is what most hook DLLs do -- but a PE export
 * table records only a start RVA, and some Wine exports are two-byte stubs, so a
 * six-byte overwrite runs into the *next* export and any call to that one jumps into the
 * middle of the hook DLL.  It faulted at 0x7A2A1D27 with hook.dll based at 0x7A2A0000.
 *
 * So: walk every module already loaded in the process, walk its import descriptors,
 * and rewrite the IAT slots that resolve to the functions we hook.  IAT entries are
 * data, not code, so there is nothing to clobber and no length to guess.  A module
 * loaded later is not patched; the ones that matter (the exe, MFC42.DLL, msvcp60.dll)
 * are all mapped before the process is created suspended and injected into.
 * --------------------------------------------------------------------------- */

static int patch_iat(HMODULE mod, const char *wantdll, const char *wantfn, void *hook)
{
    unsigned char *base = (unsigned char *)mod;
    DWORD *pe, *opt, *dir;
    DWORD impRva, i;
    unsigned char *imp;
    if (!base) return 0;
    pe = (DWORD *)(base + *(DWORD *)(base + 0x3C));
    if (pe[0] != 0x00004550) return 0;
    opt = pe + 6;
    if ((WORD)opt[0] != 0x010B) return 0;
    /* IMAGE_DATA_DIRECTORY is { DWORD VirtualAddress; DWORD Size; } -- EIGHT bytes per
     * entry.  Treating it as an array of DWORDs reads every second field, which lands on
     * the export entry and makes the import table look empty. */
    dir = (DWORD *)((unsigned char *)opt + 96);
    impRva = dir[2];                                  /* data directory entry 1 */
    if (!impRva) return 0;
    imp = base + impRva;
    for (i = 0; i < 512; i++) {
        DWORD *desc = (DWORD *)(imp + i * 20);
        DWORD oft = desc[0], nameRva = desc[3], ft = desc[4];   /* OFT, Name, FirstThunk */
        DWORD *names, *thunks;
        unsigned k;
        if (!nameRva && !ft) break;
        /* every RVA in a descriptor must be a plausible offset into the image; a
         * descriptor past the end of the table can hold anything */
        if (nameRva >= 0x10000000u || ft >= 0x10000000u) break;
        if (_stricmp((const char *)(base + nameRva), wantdll) != 0) continue;
        names  = (DWORD *)(base + (oft ? oft : ft));
        thunks = (DWORD *)(base + ft);
        /* The loader leaves .idata read-only once relocations are done, so an IAT slot
         * is not writable until we say so.  The write below faults at the store
         * otherwise. */
        {
            DWORD old = 0, span = 4096u * 4u;
            VirtualProtect(thunks, span, PAGE_READWRITE, &old);
        }
        for (k = 0; k < 4096; k++) {
            DWORD v = names[k];
            if (!v) break;
            if (v & 0x80000000u) continue;            /* imported by ordinal */
            if (v >= 0x10000000u) continue;           /* not an RVA we can trust */
            if (strcmp((const char *)(base + v + 2), wantfn) == 0) {
                thunks[k] = (DWORD)(uintptr_t)hook;
                g_detoured++;
                tr("hook: %s!%s patched in module %p slot %u", wantdll, wantfn,
                   (void *)mod, (unsigned)k);
                return 1;
            }
        }
    }
    return 0;
}


/* detour(module, "user32.dll", "PeekMessageA", hook): patch that function in that
 * module's IAT.  The macro takes (dllname, fnname, hook) for readability at the call
 * sites; the module is found by walking the process's module list. */
static int hook_in_all_modules(const char *dll, const char *fn, void *h);

#define DETOUR(dll, fn, hook) hook_in_all_modules((dll), (fn), (void *)(hook))

/* Every module the game can call one of these functions from.  MFC42.DLL is the one
 * that matters and the one a naive "patch the exe's IAT" gets wrong.  A module loaded
 * after this point is not patched; nothing that matters is. */
static const char *g_modules[] = {
    NULL,                 /* the exe itself */
    "MFC42.DLL", "mfc42u.dll", "msvcp60.dll", "MSVCRT.dll",
    "USER32.dll", "GDI32.dll", "ADVAPI32.dll", "SHELL32.dll",
    "COMCTL32.dll", "OLEAUT32.dll", "WSOCK32.dll", "KERNEL32.dll", "SRNet.dll",
    NULL
};

static int hook_in_all_modules(const char *dll, const char *fn, void *h)
{
    int hits = 0, i;
    for (i = 0; g_modules[i] || i == 0; i++) {
        HMODULE m = g_modules[i] ? GetModuleHandleA(g_modules[i]) : GetModuleHandleA(NULL);
        if (m) hits += patch_iat(m, dll, fn, h);
    }
    if (!hits) tr("hook: %s!%s not found in any loaded module's IAT", dll, fn);
    return hits;
}

/* ------------------------------------------------------------- debug tracing */

/* WOS_LOG=<path>: append-only trace of what the harness is doing.  Off unless set.
 * This is the first thing to look at when a run produces no dumps. */
static char  g_logpath[512];
static int   g_logon;
static HANDLE g_logh = INVALID_HANDLE_VALUE;
static volatile LONG g_in_trace;
static volatile LONG g_steps;

/* One cached handle and a re-entrancy guard: the trace hooks themselves call tr(), and
 * a hook that reopens the log on every line would recurse the moment the log is what
 * failed to open. */
static void tr(const char *fmt, ...)
{
    /* The buffer is STATIC, not on the stack.  This function runs on the injected
     * thread, whose stack the loader has already eaten into, and vsnprintf on top of a
     * 1 KB frame was enough to run it off the guard page -- which killed the thread
     * while it held the process heap lock and deadlocked the game.  Zero stack here. */
    static char buf[2048];
    va_list ap;
    DWORD n;
    if (!g_logon || g_logh == INVALID_HANDLE_VALUE) return;
    if (InterlockedCompareExchange(&g_in_trace, 1, 0) != 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    n = (DWORD)strlen(buf);
    buf[n] = '\r'; buf[n + 1] = '\n';
    WriteFile(g_logh, buf, n + 2, &n, NULL);
    InterlockedExchange(&g_in_trace, 0);
}


/* --------------------------------------------------------------- the script */

typedef enum {
    EV_CLICK, EV_RCLICK, EV_DOWN, EV_UP, EV_MOVE,
    EV_KEY, EV_TEXT, EV_DIALOG, EV_DUMP
} EvKind;

/* dialog value kinds */
#define DT_TEXT   0
#define DT_SEL    1
#define DT_CHECK  2
#define DT_CLICK  3
#define DT_FOCUS  4
#define DT_DEFAULT 5      /* "ok" / "cancel": click the standard button */

typedef struct {
    EvKind    kind;
    int       done;
    uint32_t  t;
    int       x, y;
    int       vkey;
    char     *text;       /* EV_TEXT payload, EV_DUMP label, EV_DIALOG verb */
    int       ctl;        /* EV_DIALOG */
    int       val;        /* EV_DIALOG kind */
    char     *valstr;
} Event;

static Event  g_ev[MAX_EVENTS];
static int     g_nev;
static volatile LONG g_ready;
static uint32_t g_end_ms = 0xFFFFFFFFu;
static int     g_finished, g_quit_posted;
static HWND    g_main;
static int     g_resize_tries;
static char    g_outdir[512] = ".";
uintptr_t g_base = IMAGE_BASE;
static int     g_dump_seq;

static const struct { const char *name; int vk; } g_vk[] = {
    {"BACK",8},{"TAB",9},{"RETURN",13},{"ENTER",13},{"SHIFT",16},{"CTRL",17},{"CONTROL",17},
    {"ALT",18},{"ESC",27},{"ESCAPE",27},{"SPACE",32},{"PGUP",33},{"PGDN",34},{"END",35},
    {"HOME",36},{"LEFT",37},{"UP",38},{"RIGHT",39},{"DOWN",40},{"INS",45},{"DEL",46},
    {"NUMLOCK",144},{"SCROLLLOCK",145},{"BREAK",46},{"APPS",187},
    {"F1",112},{"F2",113},{"F3",114},{"F4",115},{"F5",116},{"F6",117},{"F7",118},
    {"F8",119},{"F9",120},{"F10",121},{"F11",122},{"F12",123},
    {NULL,0}
};

static int vk_lookup(const char *name)
{
    int i; char up[32]; size_t n = strlen(name), j;
    if (n >= 2 && name[0] == 'V' && name[1] == 'K') { name += 2; n -= 2; }
    if (n == 1 && name[0] >= 'A' && name[0] <= 'Z') return name[0];
    if (n == 1 && name[0] >= '0' && name[0] <= '9') return name[0];
    for (j = 0; j < n && j < 31; j++) {
        char c = name[j];
        up[j] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    up[j] = 0;
    for (i = 0; g_vk[i].name; i++)
        if (strcmp(g_vk[i].name, up) == 0) return g_vk[i].vk;
    return -1;
}

static char *xstrdup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

/* split a line into whitespace-separated tokens, in place; returns the count */
static int tokenize(char *s, char **tok, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s) break;
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static void parse_dialog_value(Event *e, const char *spec)
{
    const char *eq = strchr(spec, '=');
    char key[64];
    size_t klen;
    const char *v;
    if (!eq) return;
    klen = (size_t)(eq - spec);
    if (klen >= sizeof key) klen = sizeof key - 1;
    memcpy(key, spec, klen); key[klen] = 0;
    e->ctl = atoi(key);
    v = eq + 1;
    if (!strncmp(v, "sel:", 4))        { e->val = DT_SEL;   e->valstr = xstrdup(v + 4, strlen(v + 4)); }
    else if (!strncmp(v, "check:", 6)) { e->val = DT_CHECK; e->valstr = xstrdup(v + 6, strlen(v + 6)); }
    else if (!strncmp(v, "click:", 6)) { e->val = DT_CLICK; e->valstr = xstrdup(v + 6, strlen(v + 6)); }
    else if (!strncmp(v, "focus:", 6)) { e->val = DT_FOCUS; e->valstr = xstrdup(v + 6, strlen(v + 6)); }
    else if (!strcmp(v, "click"))      { e->val = DT_CLICK; e->valstr = xstrdup("", 0); }
    else if (!strcmp(v, "focus"))      { e->val = DT_FOCUS; e->valstr = xstrdup("", 0); }
    else                               { e->val = DT_TEXT;  e->valstr = xstrdup(v, strlen(v)); }
}

static int parse_script(const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[2048];
    if (!f) { tr("oracle: cannot open script \"%s\"", path); return 0; }
    while (fgets(line, sizeof line, f)) {
        char *tok[16];
        int n;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';') continue;
        n = tokenize(p, tok, 16);
        if (n == 0) continue;
        if (!strcmp(tok[0], "end")) { g_end_ms = n > 1 ? (uint32_t)strtoul(tok[1], NULL, 10) : 0;
                                      continue; }
        if (strcmp(tok[0], "at") || n < 2 || g_nev >= MAX_EVENTS) continue;
        {
            Event *e = &g_ev[g_nev];
            memset(e, 0, sizeof *e);
            e->t = (uint32_t)strtoul(tok[1], NULL, 10);
            if (n < 3) continue;
            if (!strcmp(tok[2], "click") || !strcmp(tok[2], "rclick") ||
                !strcmp(tok[2], "down")   || !strcmp(tok[2], "up") ||
                !strcmp(tok[2], "move")) {
                e->x = n > 3 ? atoi(tok[3]) : 0;
                e->y = n > 4 ? atoi(tok[4]) : 0;
                if      (!strcmp(tok[2], "rclick")) e->kind = EV_RCLICK;
                else if (!strcmp(tok[2], "down"))   e->kind = EV_DOWN;
                else if (!strcmp(tok[2], "up"))     e->kind = EV_UP;
                else if (!strcmp(tok[2], "move"))   e->kind = EV_MOVE;
                else                                e->kind = EV_CLICK;
            } else if (!strcmp(tok[2], "key")) {
                e->kind = EV_KEY;
                e->vkey = n > 3 ? vk_lookup(tok[3]) : -1;
            } else if (!strcmp(tok[2], "text")) {
                char buf[1024]; size_t used = 0;
                int i;
                e->kind = EV_TEXT;
                for (i = 3; i < n; i++) {
                    size_t l = strlen(tok[i]);
                    if (used + l + 2 >= sizeof buf) break;
                    if (used) buf[used++] = ' ';
                    memcpy(buf + used, tok[i], l); used += l;
                    buf[used] = 0;
                }
                e->text = xstrdup(buf, used);
            } else if (!strcmp(tok[2], "dialog")) {
                int i;
                e->kind = EV_DIALOG;
                e->ctl  = -1;
                for (i = 3; i < n; i++) {
                    if (strchr(tok[i], '=')) parse_dialog_value(e, tok[i]);
                    else {
                        e->val = DT_DEFAULT;
                        e->text = xstrdup(tok[i], strlen(tok[i]));
                    }
                }
                if (e->ctl < 0) continue;      /* nothing to key the dialog off */
            } else if (!strcmp(tok[2], "dump")) {
                e->kind = EV_DUMP;
                e->text = xstrdup(n > 3 ? tok[3] : "dump", n > 3 ? strlen(tok[3]) : 4);
            } else continue;
            g_nev++;
        }
    }
    fclose(f);
    return 1;
}

/* -------------------------------------------------------------- the main window */

static BOOL CALLBACK trace_toplevel(HWND hwnd, LPARAM p)
{
    DWORD pid = 0; RECT r; char cls[128], ttl[128]; int n = *(int *)p;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    GetClassNameA(hwnd, cls, sizeof cls);
    GetWindowTextA(hwnd, ttl, sizeof ttl);
    GetClientRect(hwnd, &r);
    tr("  win[%d] %p class=\"%s\" vis=%d style=%08lX client=%dx%d title=\"%s\"",
       n, (void *)hwnd, cls, (int)IsWindowVisible(hwnd),
       (unsigned long)GetWindowLong(hwnd, GWL_STYLE),
       r.right - r.left, r.bottom - r.top, ttl);
    (*(int *)p)++;
    if (IsWindowVisible(hwnd)) {           /* a visible top-level is usually modal: dump it */
        HWND ch = (HWND)GetWindow(hwnd, GW_CHILD);
        for (; ch; ch = (HWND)GetWindow(ch, GW_HWNDNEXT)) {
            char cc[64], ct[64]; RECT cr;
            GetClassNameA(ch, cc, sizeof cc);
            GetWindowTextA(ch, ct, sizeof ct);
            GetClientRect(ch, &cr);
            tr("      child %p id=%d(0x%X) class=\"%s\" %dx%d text=\"%s\"",
               (void *)ch, GetDlgCtrlID(ch), (unsigned)GetDlgCtrlID(ch), cc,
               cr.right - cr.left, cr.bottom - cr.top, ct);
        }
    }
    return TRUE;
}

static BOOL CALLBACK find_main_proc(HWND hwnd, LPARAM p)
{
    DWORD pid = 0; RECT r;
    (void)p;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    GetClientRect(hwnd, &r);
    if (r.right - r.left < 200 || r.bottom - r.top < 200) return TRUE;
    g_main = hwnd;
    return FALSE;
}
/* The exact main window, read out of the game rather than guessed.  The game creates 54
 * top-level windows at boot (every tool dialog in the editor/help set is a real
 * top-level window, several of them larger than the frame), so a "biggest visible
 * window" heuristic picks the wrong one.  `DAT_004e4840` is the CMainFrame pointer
 * (boot_flow.md section 3c: every `SendMessage(DAT_004e4840 + 0x20, ...)` in the decomp
 * is a message to the frame) and CWnd::m_hWnd sits at +0x20, so the handle is a
 * two-dword read.  The window enumeration stays as a fallback. */
#define VA_MAIN_FRAME   0x004E4840u

static HWND find_main(void)
{
    uintptr_t b = g_base;
    if (g_main && IsWindow(g_main)) return g_main;
    g_main = NULL;
    if (*(void **)(b + (VA_MAIN_FRAME - IMAGE_BASE))) {
        HWND h = *(HWND *)(b + (VA_MAIN_FRAME - IMAGE_BASE) + 0x20);
        if (h && IsWindow(h)) { g_main = h; return g_main; }
    }
    EnumWindows(find_main_proc, 0);
    return g_main;
}

/* Both sides must share coordinates, so the client area is forced to exactly 640x480.
 * The frame has a menu bar, so the window is grown until the CLIENT area matches. */
static void ensure_main(void)
{
    RECT c, wr;
    int guard = 0;
    if (!find_main()) return;
    if (!g_resize_tries) {
        SetWindowPos(g_main, HWND_TOP, 0, 0, CLIENT_W, CLIENT_H,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_resize_tries = 1;
    }
    while (guard++ < 8) {
        int dw, dh;
        GetClientRect(g_main, &c);
        dw = CLIENT_W - (c.right - c.left);
        dh = CLIENT_H - (c.bottom - c.top);
        if (!dw && !dh) return;
        if (dw < -4096 || dh < -4096 || dw > 4096 || dh > 4096) return;
        GetWindowRect(g_main, &wr);
        SetWindowPos(g_main, NULL, wr.left, wr.top,
                     (wr.right - wr.left) + dw, (wr.bottom - wr.top) + dh,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}
/* Log the whole window tree under the main window, with each child's client rect
 * expressed in the MAIN's client coordinates -- which are the coordinates a .dsc
 * `click <x> <y>` uses.  A click is delivered to the deepest visible window whose
 * client rect contains the point, so this is what says which window a coordinate
 * actually reaches.  `depth` is only a label for the log. */
static uint32_t wintree_at;
static void wintree_walk(HWND w, int depth)
{
    HWND ch = (HWND)GetWindow(w, GW_CHILD);
    while (ch) {
        char cls[80], txt[80];
        RECT rc, mc, pc;
        int k, vis;
        GetClassNameA(ch, cls, sizeof cls);
        GetWindowTextA(ch, txt, sizeof txt);
        GetClientRect(ch, &rc);
        GetClientRect(g_main, &mc);
        POINT o, s; o.x = 0; o.y = 0;
        ClientToScreen(ch, &o);
        s = o;                                   /* the SCREEN origin, kept before the
                                                  * conversion below mutates o into
                                                  * main-client coordinates */
        ScreenToClient(g_main, &o);
        pc.left = o.x; pc.top = o.y; pc.right = o.x + rc.right; pc.bottom = o.y + rc.bottom;
        vis = IsWindowVisible(ch);
        for (k = 0; k < (int)sizeof txt - 1 && txt[k]; k++) if (txt[k] < 32) txt[k] = '.';
        /* `id` and the SCREEN rect are both emitted, and both are needed.  The
         * main-client rect above is meaningless for a control belonging to a TOP-LEVEL
         * window: a modal is not positioned relative to the main window's client, so a
         * `.dsc click` -- which takes SCREEN coordinates, because that is what
         * WindowFromPoint hit-tests -- cannot be aimed from it.  And `id` is what the
         * `dialog <id> click` op keys on, so it is the only stable way to name a
         * control whose caption may repeat across dialogs. */
        tr("wintree d=%d hwnd=%p id=%d class=%s text=%s vis=%d mainclient=(%ld,%ld)-(%ld,%ld)"
           " screen=(%ld,%ld)-(%ld,%ld)",
           depth, (void *)ch, GetDlgCtrlID(ch), cls, txt, vis,
           (long)(pc.left - mc.left), (long)(pc.top - mc.top),
           (long)(pc.right - mc.left), (long)(pc.bottom - mc.top),
           (long)(s.x + rc.left), (long)(s.y + rc.top),
           (long)(s.x + rc.right), (long)(s.y + rc.bottom));
        wintree_walk(ch, depth + 1);
        ch = (HWND)GetWindow(ch, GW_HWNDNEXT);
    }
}
static void wintree_dump(int once)
{
    char cls[80], txt[80];
    RECT mc;
    static LONG done;
    if (once && InterlockedCompareExchange(&done, 1, 0) != 0) return;
    if (!find_main()) return;
    GetClassNameA(g_main, cls, sizeof cls);
    GetWindowTextA(g_main, txt, sizeof txt);
    GetClientRect(g_main, &mc);
    tr("wintree MAIN hwnd=%p class=%s text=%s client=%ldx%ld",
       (void *)g_main, cls, txt, (long)(mc.right - mc.left), (long)(mc.bottom - mc.top));
    wintree_walk(g_main, 0);
    /* Top-level windows of this process too, not just the main window's children. A
     * SEPARATE top-level window sitting over the client steals clicks -- WindowFromPoint
     * finds it and real hit-testing delivers to it -- and it does not appear in the main
     * window's child list at all. Measured: the main menu's "Play now" hotspot was
     * unreachable because a top-level RICHEDIT captioned "TERMS OF SERVICE", with a
     * Cancel button, was covering the point the button was drawn at; 0x046B was posted
     * zero times in the whole run. */
    {
        HWND top = NULL;
        while ((top = (HWND)FindWindowExA(NULL, top, NULL, NULL)) != NULL) {
            DWORD pid = 0;
            char cls[80], txt[160];
            RECT rc;
            GetWindowThreadProcessId(top, &pid);
            if (pid != GetCurrentProcessId() || top == g_main) continue;
            if (!IsWindowVisible(top)) continue;
            GetClassNameA(top, cls, sizeof cls);
            GetWindowTextA(top, txt, sizeof txt);
            GetWindowRect(top, &rc);
            tr("wintree TOP hwnd=%p class=%s text=%s win=(%ld,%ld)-(%ld,%ld)",
               (void *)top, cls, txt, (long)rc.left, (long)rc.top,
               (long)rc.right, (long)rc.bottom);
            wintree_walk(top, 1);
        }
    }
}

/* ------------------------------------------------------------------- the input */

/* The window a real click at a screen point reaches.
 *
 * `WindowFromPoint` ignores WS_VISIBLE and WS_CHILD: it returns the window whose
 * WINDOW rectangle contains the point, hidden or not.  Real input dispatch does
 * not -- an invisible window is skipped and the point falls through to whatever
 * is visible underneath.  That difference is not academic here: the MFC MDI tree
 * under the main frame is a stack of panes that the front end shows and hides
 * (FUN_0041B891 calls ShowWindow on five of them per state change), and a hidden
 * MDI pane still owns the whole client rect.  Measured with WOS_WINTREE=1 at the
 * title: `00010082 AfxWnd42` is INVISIBLE and covers mainclient (21,2)-(458,315),
 * and the first version of this function sent a click at (320,240) straight to
 * it -- so the front end never saw the click and DAT_004DF8A4 stayed 0.  The
 * fix is to make the traversal match input dispatch: only visible, enabled
 * windows are candidates, and the deepest visible one wins. */
static HWND deepest_at(POINT screen)
{
    HWND pick = NULL, h;
    POINT root = screen;
    RECT rc;
    h = (HWND)WindowFromPoint(screen);
    while (h) {
        GetWindowRect(h, &rc);
        if (PtInRect(&rc, screen)) { pick = h; break; }
        h = (HWND)GetParent(h);
    }
    if (!pick) return NULL;
    /* Descend to the deepest visible descendant that still contains the point, and ASK
     * EACH CANDIDATE whether it wants the click. This is the missing half of input
     * dispatch and it is not cosmetic.
     *
     * Measured with WOS_MSGLOG=1: the title click at (320,240) is delivered to the front
     * view 0001007A as it should be, but the "Play now" click at (215,141) -- inside the
     * settled hotspot rect (79,120,352,162) -- is delivered to hwnd 000203AC, a window
     * created late in the run whose client origin sits at main-client (126,69). So the
     * click lands on a different window than the one that draws the button, FUN_00405765
     * is never reached, and 0x046B is never posted: measured, zero occurrences of id=046B
     * in the whole run. That is why the main menu appears unclickable.
     *
     * The reason is that hit-testing is not "the deepest visible window". Win32 asks each
     * candidate with WM_NCHITTEST and a window that answers HTTRANSPARENT (-1) passes the
     * click to whatever is beneath it; WS_EX_TRANSPARENT windows are skipped for the same
     * reason. An overlay pane that draws nothing at that point must not swallow clicks,
     * and a static geometric descent cannot tell that from a button.
     *
     * So: walk the candidates as before, and accept the first one that both contains the
     * point and does NOT answer HTTRANSPARENT. Falling back to the geometric answer if
     * every candidate declines keeps a click working for a window that ignores the
     * message, which is better than dropping it. */
    {
        HWND ch = (HWND)GetWindow(pick, GW_CHILD);
        while (ch) {
            if (IsWindowVisible(ch) && IsWindowEnabled(ch)) {
                GetWindowRect(ch, &rc);
                if (PtInRect(&rc, screen)) {
                    LPARAM lp = (LPARAM)MAKELPARAM(screen.x, screen.y);
                    LRESULT ht = SendMessageA(ch, WM_NCHITTEST, 0, lp);
                    if ((int)ht != HTTRANSPARENT) { pick = ch; break; }
                }
            }
            ch = (HWND)GetWindow(ch, GW_HWNDNEXT);
        }
    }
    (void)root;
    return pick;
}

static void post_mouse(HWND w, UINT msg, int cx, int cy)
{
    PostMessageA(w, msg, 0, (LPARAM)(((cy & 0xFFFF) << 16) | (cx & 0xFFFF)));
}

static void do_mouse(int x, int y, int kind)
{
    POINT pt; HWND w; POINT c;
    ensure_main();
    if (!g_main) return;
    pt.x = x; pt.y = y;
    ClientToScreen(g_main, &pt);
    SetCursorPos(pt.x, pt.y);
    w = deepest_at(pt);
    if (!w) w = g_main;
    c = pt; ScreenToClient(w, &c);
    post_mouse(w, WM_MOUSEMOVE, c.x, c.y);
    if (kind == EV_MOVE) return;
    if (kind == EV_RCLICK) { post_mouse(w, WM_RBUTTONDOWN, c.x, c.y);
                             post_mouse(w, WM_RBUTTONUP, c.x, c.y); return; }
    if (kind == EV_DOWN)   { post_mouse(w, WM_LBUTTONDOWN, c.x, c.y); return; }
    if (kind == EV_UP)     { post_mouse(w, WM_LBUTTONUP, c.x, c.y); return; }
    post_mouse(w, WM_LBUTTONDOWN, c.x, c.y);
    post_mouse(w, WM_LBUTTONUP, c.x, c.y);
}

static void do_key(int vk)
{
    HWND w = GetFocus();
    if (!w) w = g_main;
    if (!w || vk <= 0) return;
    PostMessageA(w, WM_KEYDOWN, (WPARAM)vk, (LPARAM)1);
    PostMessageA(w, WM_KEYUP,   (WPARAM)vk, (LPARAM)0xC0000001u);
}

static void do_text(const char *s)
{
    HWND w = GetFocus();
    if (!w) w = g_main;
    if (!w) return;
    while (s && *s) {
        char c = *s++;
        if (c == '\n') { do_key(VK_RETURN); continue; }
        PostMessageA(w, WM_CHAR, (WPARAM)(unsigned char)c, 1);
    }
}

/* ----------------------------------------------------------------- the dialogs */

static HWND search_wnd(HWND h, int ctl, int depth)
{
    HWND ch = (HWND)GetWindow(h, GW_CHILD);
    for (; ch; ch = (HWND)GetWindow(ch, GW_HWNDNEXT)) {
        if (GetDlgCtrlID(ch) == ctl) return ch;
        if (depth < 24) {
            HWND r = search_wnd(ch, ctl, depth + 1);
            if (r) return r;
        }
    }
    return NULL;
}
static BOOL CALLBACK search_toplevel(HWND h, LPARAM p)
{
    DWORD pid = 0;
    int *found = (int *)p;
    HWND r;
    GetWindowThreadProcessId(h, &pid);
    r = search_wnd(h, found[1], 0);
    if (r) { found[0] = (int)(intptr_t)h; return FALSE; }
    return TRUE;
}
static HWND find_dialog_with(int ctl)
{
    int found[2];
    found[0] = 0; found[1] = ctl;
    if (ctl <= 0) return NULL;
    EnumWindows(search_toplevel, (LPARAM)found);
    return (HWND)(intptr_t)found[0];
}

static void do_dialog(Event *e)
{
    HWND dlg = find_dialog_with(e->ctl);
    HWND ctl;
    if (!dlg) return;
    if (e->val == DT_DEFAULT) {
        UINT id = (e->text && (e->text[0] == 'c' || e->text[0] == 'C')) ? IDCANCEL : IDOK;
        ctl = GetDlgItem(dlg, id);
        if (ctl) SendMessageA(ctl, BM_CLICK, 0, 0);
        return;
    }
    ctl = GetDlgItem(dlg, e->ctl);
    if (!ctl) { HWND t = search_wnd(dlg, e->ctl, 0); ctl = t; }
    if (!ctl) return;
    switch (e->val) {
    case DT_TEXT:
        SendMessageA(ctl, WM_SETTEXT, 0, (LPARAM)(e->valstr ? e->valstr : ""));
        break;
    case DT_SEL: {
        int idx = atoi(e->valstr ? e->valstr : "0");
        if (SendMessageA(ctl, CB_SETCURSEL, (WPARAM)idx, 0) == CB_ERR)
            SendMessageA(ctl, LB_SETCURSEL, (WPARAM)idx, 0);
        break;
    }
    case DT_CHECK:
        SendMessageA(ctl, BM_SETCHECK,
                     (WPARAM)(atoi(e->valstr ? e->valstr : "0") ? BST_CHECKED : BST_UNCHECKED), 0);
        break;
    case DT_CLICK: SendMessageA(ctl, BM_CLICK, 0, 0); break;
    case DT_FOCUS: SetFocus(ctl); break;
    }
}

/* ----------------------------------------------------------------- the dumps */

/* The key registry is docs/re/oracle.md.  Module dumps use the same key=value form. */
static void dump_bmp(const char *label)
{
    char path[700];
    HDC dc, mem;
    HBITMAP bmp;
    BITMAPINFO bi;
    const int bw = CLIENT_W, bh = CLIENT_H;
    unsigned char *bits;
    DWORD *row;
    FILE *f;
    int y;
    ensure_main();
    if (!g_main) return;
    /* Xvfb has no window manager, so nothing forces a repaint: ask for one.
     * RDW_UPDATENOW sends WM_PAINT synchronously; no message loop is re-entered. */
    RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    dc = GetDC(g_main);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, bw, bh);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, bw, bh, dc, 0, 0, SRCCOPY);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = bw;
    bi.bmiHeader.biHeight      = -bh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bits = (unsigned char *)malloc((size_t)bw * bh * 4);
    if (bits) GetDIBits(mem, bmp, 0, bh, bits, &bi, DIB_RGB_COLORS);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g_main, dc);
    if (!bits) return;
    wsprintfA(path, "%s\\%s.bmp", g_outdir, label);
    f = fopen(path, "wb");
    if (f) {
        DWORD image = (DWORD)((3 * bw * bh) / 4);
        BYTE h[54];
        DWORD v;
        memset(h, 0, sizeof h);
        h[0] = 'B'; h[1] = 'M';
        v = 54 + image;                     memcpy(h + 2, &v, 4);
        v = 54;                             memcpy(h + 10, &v, 4);
        v = 40;                             memcpy(h + 14, &v, 4);
        v = (DWORD)bw;                      memcpy(h + 18, &v, 4);
        v = (DWORD)bh;                      memcpy(h + 22, &v, 4);
        h[26] = 1; h[27] = 0;
        h[28] = 24; h[29] = 0;
        v = image;                          memcpy(h + 34, &v, 4);
        fwrite(h, 1, sizeof h, f);
        row = (DWORD *)bits;
        for (y = 0; y < bh; y++) {
            DWORD *src = row + (size_t)(bh - 1 - y) * bw;   /* bottom-up on disk */
            unsigned char *out = (unsigned char *)malloc((size_t)bw * 3);
            int x;
            if (!out) break;
            for (x = 0; x < bw; x++) {
                DWORD c = src[x];
                out[x * 3 + 0] = (unsigned char)(c & 0xFF);
                out[x * 3 + 1] = (unsigned char)((c >> 8) & 0xFF);
                out[x * 3 + 2] = (unsigned char)((c >> 16) & 0xFF);
            }
            fwrite(out, 1, (size_t)bw * 3, f);
            free(out);
        }
        fclose(f);
    }
    free(bits);
}

/* The dump is written in the PORT's key vocabulary, not a private one.
 * `src/game_main.c`'s dump table maps a `dump <label>` to exactly one module, and
 * each module emits its own namespace, so the oracle has to answer with the
 * same namespace for the same label or the diff is meaningless:
 *
 *   label clock|rng -> clock.ms clock.time_s rng.state rng.calls
 *   label hero      -> hero.record hero.valid hero.name hero.in_use hero.serial
 *                      hero.level hero.gender hero.map hero.link hero.hp hero.mp
 *                      hero.abil.{str,wis,sta,agi,dex} hero.checksum
 *   any other label -> the harness keys, plus oracle.* extras
 *
 * `hero.record` is the whole 0x16CC record as ONE lowercase hex key, which is
 * the form src/game/hero.c emits; the 64-byte `hero.hex.*` lines are kept as a
 * debug extra so a human can diff a single range.
 *
 * Everything the oracle knows that the port has no key for is prefixed
 * `oracle.` and is excluded from the diff by tools/oracle/cmp_dump.py.  A key
 * the PORT emits that the oracle has no source for is reported as
 * `no-source`, never silently dropped: that is the list of work still owed. */
/* `dump <module>@<tag>` -- the tagged form, agreed with Core.  A script may
 * checkpoint the SAME module several times in one run; without a tag the file
 * names collide and the diff silently compares the last occurrence.  The split
 * is at the LAST '@', the module is the prefix, and the emitted `label` key is
 * the whole token, so one string names the file and the event.  `dump <module>`
 * with no '@' still works and still emits label=<module>. */
static const char *label_module_of(const char *label, char *buf, size_t cap)
{
    const char *at = strrchr(label, '@');
    size_t n;
    if (!at) return label;
    n = (size_t)(at - label);
    if (n == 0 || n >= cap) return label;
    memcpy(buf, label, n);
    buf[n] = 0;
    return buf;
}
static int label_is(const char *l, const char *want) { return strcmp(l, want) == 0; }
static int label_module(const char *l)
{
    static const char *names[] = { "clock","rng","hero","map","scene","battle",
                                   "panels","items","minigame","options","world", NULL };
    int i;
    for (i = 0; names[i]; i++) if (label_is(l, names[i])) return 1;
    return 0;
}
/* ------------------------------------------------------- the module read-outs
 *
 * Everything below reads the ORIGINAL's own memory, at the address the module owner
 * cited, so the key has the same meaning on both sides. Where the original has NO
 * counterpart for a key the port emits, the key is NOT emitted here and the reason is
 * recorded in docs/re/oracle.md section 5.4 -- a port-only key must be named as one
 * rather than given a plausible-looking original value, because a value that is
 * "obviously the same thing" is how a modelling gap hides.
 *
 * Two indirections are load-bearing and easy to get wrong:
 *   DAT_0067FBF8 is a POINTER to the hero table, assigned once at 0x426B9A as
 *     `DAT_004e4870 + 0x1560a5c`. Every record-relative address is
 *     `*(u32*)0x0067FBF8 + offset` -- deref once, then add.
 *   DAT_004E4874 is a POINTER to the scene block, re-pointed to a fresh random
 *     16-aligned offset inside the 0x7C140 block on every scene load (0x426BE0).
 *     Every scene-block address is `*(u32*)0x004E4874 + offset`.
 */
static unsigned char *hero_rec(void)
{
    unsigned char *p = *(unsigned char **)(g_base + (0x0067FBF8u - IMAGE_BASE));
    return p ? p : (unsigned char *)(g_base + (0x0067FBF8u - IMAGE_BASE));
}
static unsigned char *scene_blk(void)
{
    unsigned char *p = *(unsigned char **)(g_base + (0x004E4874u - IMAGE_BASE));
    return p ? p : (unsigned char *)(g_base + (0x004E4874u - IMAGE_BASE));
}
#define RDI(p, off)   (*(int *)((p) + (off)))
#define RDX(p, off)   (*(int *)((p) + (off)))

/* map.* -- the walk state lives in hero record 0. Offsets from the disassembly of
 * FUN_004620F3 (0x4620F3), FUN_0046230E (0x46230E) and FUN_00461948 (0x461948), and
 * the path/link globals from FUN_00461DCC's callers. Positions are 24.8 fixed: every
 * consumer shifts by 8 (0x462958, 0x4620F3), so the port's map-unit keys are the
 * stored word shifted, and map.fx/map.fy are the stored word itself. */
/* `map.*` describes the hero's position, and the hero lives in record 0. When no soul
 * is loaded that record is the post-memset zero block, so every field reads 0 -- which
 * is not "the hero is at map 0", it is "there is no hero". The port says map.id=-1 in
 * that state, and it is right to. So the whole namespace is gated on the same liveness
 * test hero.valid uses (FUN_0041F832/FUN_0042095E leave 1 alive, 2 ghost, 4 loaded but
 * unincarnated), and a dump with no hero emits map.id=-1 and nothing else, rather than
 * a table of zeros that reads like agreement. */
static void dump_map(FILE *f)
{
    unsigned char *h = hero_rec();
    uintptr_t b = g_base;
    int inuse = RDI(h, 0x00);
    int x, y;
    if (inuse < 1 || inuse > 4) { fprintf(f, "map.id=-1\n"); return; }
    x = RDI(h, 0x94); y = RDI(h, 0x98);
    fprintf(f, "map.fx=%d\n", x);
    fprintf(f, "map.fy=%d\n", y);
    fprintf(f, "map.x=%d\n", x >> 8);
    fprintf(f, "map.y=%d\n", y >> 8);
    fprintf(f, "map.tx=%d\n", RDI(h, 0x9C));
    fprintf(f, "map.ty=%d\n", RDI(h, 0xA0));
    fprintf(f, "map.speed=%d\n", RDI(h, 0xAC));
    fprintf(f, "map.duration=%d\n", RDI(h, 0xB4));
    /* +0x90 is the MAP (START_LOCATION arg 1, written at all.c:24389 from the class
     * table's +0x1ABCC) and +0x67C is the LINK (arg 2, +0x1ABD0). The two are told
     * apart by the use site: all.c:24099 sends 0x46A(link, map) with 0x67C first. */
    fprintf(f, "map.id=%d\n", RDI(h, 0x90));
    /* the facing is the original's own encoding: fy*4+fx with a 5->9 remap (0x46230E),
     * NOT the port's fy*3+fx, so this is compared as the raw stored word. */
    fprintf(f, "map.facing_raw=%d\n", RDI(h, 0x88));
    fprintf(f, "map.path_count=%d\n", RDI(b + (0x004F2168u - IMAGE_BASE), 0));
    fprintf(f, "map.path_cursor=%d\n", RDI(b + (0x004F216Cu - IMAGE_BASE), 0));
    fprintf(f, "map.nearest=%d\n", RDI(b + (0x004F2240u - IMAGE_BASE), 0));
    fprintf(f, "map.hit=%d\n", RDI(b + (0x004F2244u - IMAGE_BASE), 0));
    fprintf(f, "map.latched=%d\n", RDI(b + (0x004F222Cu - IMAGE_BASE), 0));
    /* ELAPSED, not the raw stamp. The original's own test is
     * `5000 < GetTickCount() - _DAT_004f2220` (FUN_0046260E's encounter roll), i.e. the
     * quantity the rule consumes is the DIFFERENCE. A raw GetTickCount word only means
     * something relative to another GetTickCount reading, so emitting it raw made the
     * key a wall-clock stamp that is not comparable between two processes even when both
     * are correct -- and it compared the original's 0 against the port's 1200, which are
     * not two values of one thing but two different quantities. With a fresh stamp of 0
     * this correctly reads as "time since boot", which is the honest initial state. */
    fprintf(f, "map.enc_a=%ld\n", (long)(vnow() - (unsigned)*(unsigned *)(b + (0x004F2220u - IMAGE_BASE))));
    fprintf(f, "map.enc_b=%ld\n", (long)(vnow() - (unsigned)*(unsigned *)(b + (0x004F2224u - IMAGE_BASE))));
    /* 0, not -1: the key is a time and -1 is not one. The original's word is 0 until a
     * fight ends, and MapView-2's -1 sentinel meant the same thing through a different
     * convention, which is exactly the kind of aliasing this registry exists to remove. */
    fprintf(f, "map.enc_grace=%ld\n", (long)(vnow() - (unsigned)*(unsigned *)(b + (0x004E70A8u - IMAGE_BASE))));
    fprintf(f, "map.no_monsters=%d\n", RDI(b + (0x004F2228u - IMAGE_BASE), 0));
    fprintf(f, "map.wander=%d\n", RDI(b + (0x004F2190u - IMAGE_BASE), 0) != 0);
    fprintf(f, "map.wander_legs=%d\n", (int)(*(unsigned *)(b + (0x004F219Cu - IMAGE_BASE))));
    fprintf(f, "map.music=%d\n", RDI(b + (0x004E70B8u - IMAGE_BASE), 0));
    /* option 7, "Enable automatic Way Point calculations": the port's map.waypoints */
    fprintf(f, "map.waypoints=%d\n", RDI(b + (0x006840D0u - IMAGE_BASE), 7 * 4));
}

/* battle.* -- the fight's `this` is the CWnd sub-object at pane+0xBC, where the pane
 * is CSplitterWnd::GetPane(*(void**)0x004E483C + 0x13FC, 0, 0). 0x004E483C is the
 * CSoulsView pointer, live for the whole game, so it is NOT "a fight is open" and is
 * not a minigame flag either. The chain is read by walking the MFC thunk
 * FUN_004C52EE (`jmp [0xD8B1EC]`, rizin names it GetPane), which lands in MFC42 --
 * so the oracle cannot follow it, and battle.state is emitted as the scene block's
 * own fight word instead, which is the same value the port calls state. */
static void dump_battle(FILE *f)
{
    unsigned char *sc = scene_blk();
    uintptr_t b = g_base;
    int i, n = RDI(sc, 0x0D0);
    if (n < 0 || n > 8) n = 0;
    fprintf(f, "battle.state=%d\n", RDI(sc, 0x0C0));
    fprintf(f, "battle.result=%d\n", RDI(sc, 0x0C4));
    fprintf(f, "battle.count=%d\n", n);
    fprintf(f, "battle.target=%d\n", RDI(sc, 0x0C8));
    fprintf(f, "battle.attacker=%d\n", RDI(sc, 0x0CC));
    fprintf(f, "battle.victim=%d\n", RDI(sc, 0x0B4));
    fprintf(f, "battle.spell=%d\n", RDI(sc, 0x0B8));
    fprintf(f, "battle.participation_total=%d\n", RDI(b + (0x00502830u - IMAGE_BASE), 0));
    fprintf(f, "battle.scene_kills=%d\n", RDI(sc, 0x3E034));
    fprintf(f, "battle.auto_resurrect=%d\n", RDI(b + (0x00502A48u - IMAGE_BASE), 0));
    fprintf(f, "battle.sticky=%d\n", RDI(b + (0x00502B14u - IMAGE_BASE), 0));
    fprintf(f, "battle.hp_gauge=%d\n", (int)(*(unsigned *)(b + (0x004F91CCu - IMAGE_BASE))));
    fprintf(f, "battle.mp_gauge=%d\n", (int)(*(unsigned *)(b + (0x004F91D0u - IMAGE_BASE))));
    /* the combatant records: rec(i) = scene + 0x128 + i*0x6E0, 144 slots. The sealed
     * fields are NOT encrypted at rest -- FUN_0049B71B stores the plain dword and
     * FUN_0049B70F returns it -- so they are directly readable ints. The x/y are
     * PLAIN logical units in 0..360 x 0..256 (FUN_0048B13C returns 360, and 0x4806CE
     * adds it straight to a rand()%32), so there is no >>8 here. */
    for (i = 0; i < n; i++) {
        unsigned char *r = sc + 0x128 + (size_t)i * 0x6E0;
        if (RDI(r, 0x000) == 0 && RDI(r, 0x004) == 0) continue;
        fprintf(f, "battle.slot%d.id=%d\n", i, RDI(r, 0x004));
        fprintf(f, "battle.slot%d.hp=%d\n", i, RDI(r, 0x5B8));
        fprintf(f, "battle.slot%d.maxhp=%d\n", i, RDI(r, 0x2A8));
        fprintf(f, "battle.slot%d.mp=%d\n", i, RDI(r, 0x5BC));
        fprintf(f, "battle.slot%d.x=%d\n", i, RDI(r, 0x51C));
        fprintf(f, "battle.slot%d.y=%d\n", i, RDI(r, 0x520));
        fprintf(f, "battle.slot%d.state=%d\n", i, RDI(r, 0x524));
        fprintf(f, "battle.slot%d.participation=%d\n", i, RDI(r, 0x11A));
    }
}

/* items.* -- the trophy bag is 128 words at hero+0x0CE0 (FUN_0046F726 reads,
 * FUN_0046F779 writes `(count<<8 | id<<16) ^ 0x1D43E217`) and the geometry word at
 * hero+0x0EE0 (`w<<16 | h&0xFFFF`, split by FUN_0046F60F). The pet pen is its own
 * block: base DAT_004E4878, stride 0x608, 32 slots (FUN_0040FBFD's fixup loop). */
static void dump_items(FILE *f)
{
    unsigned char *h = hero_rec();
    uintptr_t b = g_base;
    unsigned char *pen = (unsigned char *)(b + (0x004E4878u - IMAGE_BASE));
    int i, used = 0, geo = RDI(h, 0xEE0);
    int w = (geo >> 16) & 0xFFFF, hh = geo & 0xFFFF;
    char text[1024];
    int len = 0;
    text[0] = 0;
    for (i = 0; i < 128 && len < (int)sizeof text - 40; i++) {
        int word = RDI(h, 0x0CE0 + i * 4);
        int id, count;
        if (!word) continue;
        /* the stored word is the obfuscated (count, id) pair */
        id = ((word ^ 0x1D43E217) >> 16) & 0xFF;
        count = ((word ^ 0x1D43E217) >> 8) & 0xFF;
        if (!id || !count) continue;
        len += snprintf(text + len, sizeof text - (size_t)len, "%s%d:%d", len ? "," : "", id, count);
        ++used;
    }
    if (!used) snprintf(text, sizeof text, "0");
    fprintf(f, "items.trophy_bag=%s\n", text);
    snprintf(text, sizeof text, "%dx%d", w, hh);
    fprintf(f, "items.trophy_bag_size=%s\n", text);
    fprintf(f, "items.trophy_bag_used=%d\n", used);
    fprintf(f, "items.trophy_bag_free=%d\n", (w * hh) - used);
    /* +0x0EE8 is the pet's live id (read only as "!= 0"); +0x0EEC is a GetTickCount
     * stamp written by FUN_0043B8E7, NOT a second pet id. */
    fprintf(f, "items.pet_ids0=%d\n", RDI(h, 0x0EE8));
    fprintf(f, "items.pet_ids1=%d\n", (int)(*(unsigned *)(h + 0x0EEC)));
    {
        int pets = 0;
        for (i = 0; i < 32; i++) if (RDI(pen + (size_t)i * 0x608, 0)) ++pets;
        fprintf(f, "items.pet_count=%d\n", pets);
    }
}

/* minigame.* -- DAT_004E18A4 is the armed game number. There is NO original global for
 * "a minigame window is open": the original keys off the dialog's own window, and
 * DAT_004E483C (which the port cited) is the CSoulsView pointer, live all game. */
static void dump_minigame(FILE *f)
{
    uintptr_t b = g_base;
    fprintf(f, "minigame.armed=%d\n", RDI(b + (0x004E18A4u - IMAGE_BASE), 0));
}

/* options.* -- the 33 numbered options are REAL and live at DAT_006840D0.
 *
 * The table itself (DAT_004F2A58, 32 records of {id, default, label}, count 32 at
 * DAT_004F2BD8) is initialised .data, not built by a constructor -- the byte-pattern
 * search for a store to it in .text correctly finds nothing, because it is never
 * supposed to be stored by code. FUN_00466EF3 loads each record with
 * `GetProfileInt("Preferences", "option <id>", <default>)`, so the value a run sees is
 * the registry's if the key exists and the .data default otherwise. The dump reads the
 * LIVE array, which is what the game is actually using, not the defaults.
 *
 * Nine NAMED scalars (worldLocation, seanceInProgress, enableMusic,
 * enableEnvironmentalSounds, enableSFX, enableSoundCard, askForSkins, enableHowDoYou,
 * eavesdropEnabled) are read the same way but have no array slot, so they are PORT-ONLY
 * here: their value lives in the registry, not in the image. */
static void dump_options(FILE *f)
{
    unsigned char *a = (unsigned char *)(g_base + (0x006840D0u - IMAGE_BASE));
    int i;
    for (i = 0; i < 33; i++) fprintf(f, "options.%d=%d\n", i, RDI(a, i * 4));
}

/* The front end's clickable regions, read straight out of the original.
 *
 * FUN_004056E7 (0x4056E7) is `AddHotspot`: it finds a free slot in the table at
 * DAT_005339F8 and calls FUN_00405000 (0x405000), which fills one 0x2F-dword record.
 * FUN_00405765 (0x405765) is the hit test the front end runs on every mouse-up: it
 * walks the same table, skips records whose state word is not 1..2 or whose flag byte
 * has bit 2 clear, and `PtInRect`s the RECT at record+0x1D dwords; on a hit it posts
 * `(&DAT_00533a60)[i*0x2F]` the message `(&DAT_00533a64)[i*0x2F]` with wParam = i.
 *
 * This is the ONLY reliable way to get a click coordinate for the front end, and it is
 * worth reading rather than inferring from the art for three reasons. The layout is
 * computed from `GetClientRect` at the moment each state is entered, so the same
 * button is at a different pixel after a resize; the label text is a heap string the
 * hook can print, which names the button; and a `dump` of the table is a `front.*` key
 * that the port can answer from its own hotspot list, which is the parity question that
 * actually matters (same buttons, same rects, same order) rather than "a click
 * somewhere in the middle looked right".
 *
 * Stride 0xBC bytes, 100 slots, bounds (&DAT_005339F8, &DAT_00538368). */
#define VA_HOTSPOT      0x005339F8u
#define HOTSPOT_STRIDE  0xBCu
#define HOTSPOT_SLOTS   100
#define HOTSPOT_RECT    0x74u   /* record + 0x1D dwords, per FUN_00405765 */
/* Emit every live hotspot. `pfx` is the key prefix, so the same reader serves the
 * `front` label and a diagnostic label. */
static void dump_hotspots(FILE *f, const char *pfx)
{
    uintptr_t b = g_base;
    unsigned char *t = (unsigned char *)(b + (VA_HOTSPOT - IMAGE_BASE));
    int i, live = 0;
    for (i = 0; i < HOTSPOT_SLOTS; i++) {
        unsigned char *r = t + (size_t)i * HOTSPOT_STRIDE;
        int state = *(int *)r;
        int rect[4], flags, msg, target;
        HWND hwnd;
        const char *label;
        int k;
        if (state < 1 || state > 2) continue;      /* FUN_00405765's own liveness test */
        for (k = 0; k < 4; k++) rect[k] = *(int *)(r + HOTSPOT_RECT + 4 * k);
        /* FUN_00405765 tests the byte at record+9 against 4, which is bit 10 of the
         * dword at +0x08 -- the port's 0x400 "the entry is clickable".  Emitting the
         * dword's whole low byte would report 4 for an entry that is not clickable. */
        flags = *(int *)(r + 0x08) & 0x400;
        label = *(const char **)(r + 0x14);
        /* Record layout, from FUN_00405000's stores and FUN_00405765's PostMessage:
         *   +0x68 the HWND the entry posts to, +0x6C the message, +0x70 the lParam.
         * The earlier version of this dump read +0x70 and called it "target", which is
         * the lParam -- 0 for every entry -- and never emitted the window at all, so the
         * one field that says WHERE a click's effect goes was the one field missing. */
        msg   = *(int *)(r + 0x6C);
        target= *(int *)(r + 0x70);
        hwnd  = *(HWND *)(r + 0x68);
        if (rect[2] <= rect[0] || rect[3] <= rect[1]) continue;   /* an empty rect never hits */
        fprintf(f, "%s.hotspot.%d.state=%d\n", pfx, i, state);
        fprintf(f, "%s.hotspot.%d.rect=%d,%d,%d,%d\n", pfx, i,
                rect[0], rect[1], rect[2], rect[3]);
        /* HotspotAnimTick's lerped font size, x and y (+0xAC/+0xB0/+0xB4, per-mille):
         * the font-independent part of the rect, so the port can be held to it. */
        fprintf(f, "%s.hotspot.%d.anchor=%d,%d,%d\n", pfx, i,
                *(int *)(r + 0xAC), *(int *)(r + 0xB0), *(int *)(r + 0xB4));
        fprintf(f, "oracle.hotspot.%d.t_start=%u\n", i, *(unsigned *)(r + 0x0C));
        fprintf(f, "%s.hotspot.%d.clickable=%d\n", pfx, i, flags ? 1 : 0);
        fprintf(f, "%s.hotspot.%d.msg=%04X\n", pfx, i, (unsigned)msg);
        fprintf(f, "%s.hotspot.%d.target=%d\n", pfx, i, target);
        /* the window the entry's message is posted to, as the hook sees it */
        fprintf(f, "%s.hotspot.%d.hwnd=%lu\n", pfx, i,
                (unsigned long)(hwnd ? (unsigned long)(uintptr_t)hwnd : 0ul));
        if (hwnd) {
            char cls[64];
            GetClassNameA(hwnd, cls, sizeof cls);
            fprintf(f, "%s.hotspot.%d.class=%s\n", pfx, i, cls);
        }
        if (label) {
            char buf[96];
            int n = 0;
            for (k = 0; k < 95 && label[k]; k++)
                buf[n++] = (label[k] >= 32 && label[k] < 127) ? label[k] : '.';
            buf[n] = 0;
            fprintf(f, "%s.hotspot.%d.label=%s\n", pfx, i, buf);
        }
        if (++live >= 24) break;      /* bounded: a pathological table must not flood the file */
    }
    fprintf(f, "%s.hotspot_count=%d\n", pfx, live);
    /* The client size the hotspot rects were laid out against.  Without it a rect
     * that runs off the client cannot be told apart from a wrong one, and the front
     * end recomputes every rect on entry, so a resize between entry and dump makes
     * the stored rects stale -- which is exactly the case that has to be visible. */
    if (g_main) {
        RECT c;
        GetClientRect(g_main, &c);
        /* TWO keys, not one "WxH" pair: a single pair key aliases a width change into a
         * height change -- the value differs either way, but the key does not say which,
         * so a bisect has to go back to the live process to find out. FrontHero-2's
         * front_dump emits client_w/client_h and the oracle matches it. */
        fprintf(f, "%s.client_w=%ld\n", pfx, (long)(c.right - c.left));
        fprintf(f, "%s.client_h=%ld\n", pfx, (long)(c.bottom - c.top));
    }
}
/* The hero record's offset inside the 0x15C0170 block FUN_00426149 malloc'd:
 * 0x1560A5C + (rand() % 0x3FFF & ~0xF), the draw taken at 0x426B94 and added into the
 * table base at 0x426B9A. It is the one part of the record's address both sides can
 * agree on, and because it is that rand() it pins WHERE in the boot LCG stream the
 * record landed -- a mismatch here means the boot rand order diverged even when
 * rng.calls happens to match. */
static unsigned hero_base_offset(void)
{
    uintptr_t b = g_base;
    unsigned char *blk = *(unsigned char **)(b + (0x004E486Cu - IMAGE_BASE));
    unsigned char *rec = *(unsigned char **)(b + (0x0067FBF8u - IMAGE_BASE));
    if (!blk || !rec) return 0;
    /* The KEY is the rand() draw, not the offset from the malloc block. The block
     * offset is that draw plus the constant 0x1560A5C, and the constant is fixed, so
     * the two differ by 22417116 on every run -- which reads as a mismatch on a key
     * that is in fact identical. The draw is also the only part the port can compute
     * from its own RNG stream, which is the whole point: a difference here means the
     * boot LCG diverged at draw 1409. Measured on a boot run: the block offset is
     * 22417132 and the draw is 16, i.e. 22417132 - 0x1560A5C, and `rand() % 0x3FFF`
     * returned 16 with the low 4 bits already clear. */
    return (unsigned)(intptr_t)(rec - blk) - 0x1560A5Cu;
}

static void do_dump(const char *label)
{
    char path[700];
    char modbuf[64];
    const char *mod = label_module_of(label, modbuf, sizeof modbuf);
    FILE *f;
    unsigned char *hero;
    uintptr_t b = g_base;
    int i, live, is_hero = label_is(mod, "hero");
    int is_clock = label_is(mod, "clock") || label_is(mod, "rng");
    wsprintfA(path, "%s\\%s.txt", g_outdir, label);
    f = fopen(path, "wb");
    if (!f) { tr("oracle: cannot write %s", path); return; }
    /* DAT_0067FBF8 is a POINTER to the hero record, not the record: FUN_004269AF
     * at 0x426B8F does `DAT_0067fbf8 = DAT_004e4870 + 0x1560a5c`, where
     * DAT_004e4870 is the 0x15C0170-byte block FUN_00426149 malloc'd plus
     * `(rand() & 0x3fff & ~0xf)`.  So the record base is the VALUE read through
     * the pointer, and hero.base_offset is that per-run offset, which is the
     * only part of the address the two sides can share. */
    hero = *(unsigned char **)(b + (0x0067FBF8u - IMAGE_BASE));
    if (!hero) hero = (unsigned char *)(b + (0x0067FBF8u - IMAGE_BASE));
    fprintf(f, "label=%s\n", label);
    if (mod != label) fprintf(f, "oracle.tag=%s\n", label + strlen(mod) + 1);

    /* --- the harness keys, in the port's spelling -------------------------
     * Only for the labels the port answers with them: `dump <label>` maps to
     * exactly one module on both sides, and putting clock.* in a `dump hero`
     * would make every hero label a mismatch over a key that is not the
     * hero's. */
    if (is_clock) {
        fprintf(f, "clock.ms=%u\n", vnow());
        fprintf(f, "clock.time_s=%u\n", vtime_s());
        fprintf(f, "rng.state=%u\n", g_holdrand);
        fprintf(f, "rng.calls=%d\n", (int)g_rand_calls);
        /* UNPREFIXED, because the port emits it: src/game_main.c's dump_builtin writes
         * rng.srand_calls on the clock and rng labels. Emitting it as oracle.srand_calls
         * instead made every one of those labels report "only in port" for a key the
         * oracle has and simply misfiled. */
        fprintf(f, "rng.srand_calls=%d\n", (int)g_srand_calls);
        /* hero.base_offset is a property of the boot, not of the hero, and the port
         * emits it on every label. Emitting it only under `dump hero` made it read as
         * missing from the clock and rng labels. */
        fprintf(f, "hero.base_offset=%u\n", hero_base_offset());
    }

    /* --- the front end's clickable regions ---------------------------------
     * Only on a `front` label. The hotspot table IS front-end state, but there is no
     * `front` module in src/game_main.c's dump table, so emitting `front.*` on every
     * label would add ~100 un-comparable keys to every other module's dump and bury the
     * real differences. `front.*` is PORT-ONLY until the port grows a front_dump; the
     * keys are what such a dump would answer, and cmp_dump.py lists the prefix as
     * no-source rather than as a mismatch (docs/re/oracle.md 5.4.4). */
    if (label_is(mod, "front")) {
        /* DAT_004DF8A4 under the port's own key, which the port emits on a front label. */
        fprintf(f, "front_state=%d\n", *(int *)(b + (0x004DF8A4u - IMAGE_BASE)));
        dump_hotspots(f, "front");
    }

    /* --- the module read-outs, in the port's key vocabulary ---------------- */
    if (label_is(mod, "map"))       dump_map(f);
    if (label_is(mod, "battle"))    dump_battle(f);
    if (label_is(mod, "items"))     dump_items(f);
    if (label_is(mod, "panels"))    dump_items(f);   /* panels_dump calls items_dump */
    if (label_is(mod, "minigame"))  dump_minigame(f);
    if (label_is(mod, "options"))   dump_options(f);

    /* --- the hero record, in the port's spelling -------------------------- */
    if (is_hero) {
        char nm[33];
        int k;
        fprintf(f, "hero.record=");
        for (i = 0; i < (int)0x16CC; i++) fprintf(f, "%02x", hero[i]);
        fputc('\n', f);
        /* hero.valid mirrors src/game/hero.c, and so does the early return: the
         * port emits hero.record + hero.valid and nothing else when no hero is
         * loaded, so the oracle must not invent the rest.  The record counts as
         * loaded only for the three in-use values FUN_0041F832/FUN_0042095E can
         * leave (1 alive, 2 ghost, 4 loaded-but-unincarnated); anything else in
         * that slot is uninitialised memory, not a hero. */
        if (*(int *)(hero + 0x000) < 1 || *(int *)(hero + 0x000) > 4) {
            fprintf(f, "hero.valid=0\n");
            goto hero_done;
        }
        fprintf(f, "hero.valid=1\n");
        for (k = 0; k < 32 && ((const char *)hero)[0x14 + k]; k++)
            nm[k] = ((const char *)hero)[0x14 + k];
        nm[k] = 0;
        fprintf(f, "hero.name=%s\n", nm);
        fprintf(f, "hero.in_use=%d\n",  *(int *)(hero + 0x000));
        fprintf(f, "hero.serial=%d\n",  *(int *)(hero + 0x004));
        fprintf(f, "hero.level=%d\n",   *(int *)(hero + 0x064));
        fprintf(f, "hero.map=%d\n",     *(int *)(hero + 0x090));
        fprintf(f, "hero.gender=%d\n",  *(int *)(hero + 0x1A8));
        fprintf(f, "hero.link=%d\n",    *(int *)(hero + 0x67C));
        fprintf(f, "hero.hp=%d\n",      *(int *)(hero + 0x6BC));
        fprintf(f, "hero.mp=%d\n",      *(int *)(hero + 0x6C0));
        fprintf(f, "hero.abil.str=%d\n", *(int *)(hero + 0x1A0));
        fprintf(f, "hero.abil.wis=%d\n", *(int *)(hero + 0x1A4));
        fprintf(f, "hero.abil.sta=%d\n", *(int *)(hero + 0x1A8 - 8));
        fprintf(f, "hero.abil.agi=%d\n", *(int *)(hero + 0x1A0 + 8));
        fprintf(f, "hero.abil.dex=%d\n", *(int *)(hero + 0x1A0 + 12));
        fprintf(f, "hero.checksum=%08x\n", (unsigned)*(unsigned *)(hero + 0x16C8));
        /* hero.base_offset is the hero record's offset inside the 0x15C0170 block
         * FUN_00426149 malloc'd: 0x1560A5C + (rand() & 0x3FFF & ~0xF).  It is the one
         * part of the record's address both sides can agree on, and because it is
         * that rand() draw it pins WHERE in the boot LCG stream the record landed --
         * a mismatch here means the boot rand order diverged even when rng.calls
         * happens to match.  Core is asked to emit it from the same expression. */
        fprintf(f, "hero.base_offset=%u\n", hero_base_offset());
hero_done: ;
    }

    /* --- oracle-only: everything the port has no key for ------------------ */
    fprintf(f, "oracle.seq=%d\n", g_dump_seq++);
    fprintf(f, "oracle.gate_stamp=%lu\n",
            (unsigned long)*(volatile DWORD *)(b + (VA_GATE_STAMP - IMAGE_BASE)));
    fprintf(f, "oracle.world_stamp=%lu\n",       /* NetGraphTick's 25 ms world-step stamp */
            (unsigned long)*(volatile DWORD *)(b + (0x004E48CCu - IMAGE_BASE)));
    fprintf(f, "oracle.steps=%ld\n", (long)g_steps);
    fprintf(f, "oracle.timers_live=%d\n", (live = timer_live_count(), live));
    fprintf(f, "oracle.next_deadline=%u\n", timer_next_deadline());
    fprintf(f, "oracle.srand_calls=%d\n", (int)g_srand_calls);
    fprintf(f, "oracle.front_state=%d\n", *(int *)(b + (0x004DF8A4u - IMAGE_BASE)));
    fprintf(f, "oracle.front_serial=%d\n", *(int *)(b + (0x004DD20Cu - IMAGE_BASE)));
    fprintf(f, "oracle.front_network=%d\n", *(int *)(b + (0x004E6910u - IMAGE_BASE)));
    fprintf(f, "oracle.front_map=%d\n", *(int *)(b + (0x004E0DDCu - IMAGE_BASE)));
    /* `battle.spell_success_percent` is emitted UNPREFIXED, because it is a key the
     * port's battle_dump() has to emit too (Battle3) or there is nothing to compare.
     * The source is _DAT_004E0FF8 in the running original, read at 0x4A7507 by
     * FUN_004A7456 and compared against 100 at 0x4A750C -- 100 means "no scaling".
     * It is on the battle label rather than an oracle.* key on purpose: the value does
     * not move during a fight in the original, so a divergence here is the port's
     * cast_success() applying a global where the original applies per-spell flags at
     * spell[0x124]/0x128, and it has to be visible in the diff at the moment it
     * happens rather than showing up later as an unexplained roll difference. */
    fprintf(f, "battle.spell_success_percent=%d\n",
            *(int *)(b + (0x004E0FF8u - IMAGE_BASE)));
    {
        const char *w = (const char *)(b + (0x004E0BD0u - IMAGE_BASE));
        char wbuf[64]; int k;
        for (k = 0; k < 63 && w[k]; k++) wbuf[k] = (w[k] >= 32 && w[k] < 127) ? w[k] : '?';
        wbuf[k] = 0;
        fprintf(f, "oracle.front_world=%s\n", wbuf);
    }
    fprintf(f, "oracle.hero_inuse=%d\n", *(int *)(hero + 0x000));
    fprintf(f, "oracle.hero_seconds=%d\n", *(int *)(hero + 0x01CD));
    fprintf(f, "oracle.hero_invocations=%d\n", *(int *)(hero + 0x01CC));
    if (!label_module(mod)) fprintf(f, "oracle.unknown_label=1\n");
    for (i = 0; i < (int)0x16CC; i += 64) {            /* debug form, 64 bytes a line */
        int j, n = (int)0x16CC - i; if (n > 64) n = 64;
        fprintf(f, "oracle.hero_hex.%04X=", i);
        for (j = 0; j < n; j++) fprintf(f, "%02X", hero[i + j]);
        fputc('\n', f);
    }
    fclose(f);
    dump_bmp(label);
}

/* ------------------------------------------------------------- the pump hooks */

static void script_init(void);
static int pump_step_inner(void);
static volatile LONG g_pump_depth;
static uint32_t g_last_gate = 0xFFFFFFFFu;
/* the thread pump_step runs on -- the app's own -- recorded in script_init and used by
 * the stall reporter, which must not guess: a Wine process has RPC and console helper
 * threads whose EIP is in ntdll and says nothing about the app. */
static volatile DWORD g_app_tid;
/* set from WOS_STALL in script_init; declared here because the watchdog is defined after
 * script_init and the flag belongs to the stall reporter, not to the script parser. */
static volatile LONG stall_dump;
/* forward: the stall reporter is defined after script_init, which is where it is set */

/* Re-entrancy guard.  Everything pump_step does runs on the app's own thread inside a
 * message-pump call, and several of those calls (RedrawWindow with RDW_UPDATENOW,
 * SetWindowPos with SWP_FRAMECHANGED, EnumWindows) can re-enter user32, and MFC's
 * handlers can pump too.  Without this the harness can drive itself off the stack. */
static int pump_step(void)
{
    LONG d = InterlockedIncrement(&g_pump_depth);
    int r;
    if (d > 8) {
        tr("pump_step: re-entered %ld deep -- refusing (a pump call is re-entering the harness)", d);
        InterlockedDecrement(&g_pump_depth);
        return 0;
    }
    r = pump_step_inner();
    InterlockedDecrement(&g_pump_depth);
    return r;
}

static BOOL (WINAPI *real_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *real_GetMessageA)(LPMSG, HWND, UINT, UINT);

/* Returns 1 if something was posted.  Called from inside the message-pump hooks while
 * the queue is empty: this is the only place the virtual clock is allowed to move. */
static int pump_step_inner(void)
{
    uint32_t now, target, t_in, t_tim, t_idle, gate;
    int idx, i;

    if (!g_ready) { script_init(); return 0; }
    if (g_finished) return 0;
    /* The window is deliberately NOT required here.  Between CreateProcess and the
     * first window there are waits of the form `while (GetTickCount() - t0 < N);` and
     * those must complete, so the clock has to be moving before the window exists.
     * Because every idle pass advances it by exactly one step, the number of passes is
     * fixed and the run stays deterministic. */
    if (InterlockedIncrement(&g_steps) == 1)
        tr("pump_step: entering the loop, nev=%d end=%u", g_nev, g_end_ms);
    if ((g_steps % 2048) == 0) {          /* main-thread window probe; EnumWindows is
                                             * safe here but not from another thread */
        int n = 0;
        HWND h = find_main();
        tr("pump_step: step=%ld tick=%u main=%p tops=%d", (long)g_steps, vnow(), (void *)h, 0);
        EnumWindows(trace_toplevel, (LPARAM)&n);
        tr("pump_step: step=%ld top-level windows=%d", (long)g_steps, n);
    }

    now = vnow();

    /* the 20 Hz boundary, read from the original's own stamp so it cannot drift */
    gate = *(volatile DWORD *)(g_base + (VA_GATE_STAMP - IMAGE_BASE));
    if (gate > now) gate = now;                  /* stale, uninitialised, or moved */
    t_idle = gate + GATE_PERIOD;
    if (t_idle <= now) {
        /* The 20 Hz work is DUE at now. Give the app one pass at this clock value so its
         * idle path (AppRun 0x40A8D9 -> FUN_0040A7C7) runs on the boundary, as it does on
         * Windows where draining a timer's messages takes no time. Only one: if the gate
         * did not stamp (a modal loop, a closed gate), move on so the clock cannot livelock. */
        static uint32_t held_at = 0xFFFFFFFFu;
        if (held_at != now) { held_at = now; t_idle = now; }
        else t_idle = now + IDLE_QUANTUM;
    }

    for (idx = 0; idx < g_nev; idx++) if (!g_ev[idx].done) break;
    t_in = (idx < g_nev) ? g_ev[idx].t : 0xFFFFFFFFu;
    t_tim = timer_next_deadline();

    /* The 20 Hz stamp is a parity key in its own right, so log it whenever it MOVES as
     * well as for the first minute of steps: a stamp that never moves is the whole
     * answer to "why does the tick lag". */
    if (InterlockedCompareExchange(&g_steps, 0, 0) < 60 || gate != g_last_gate) {
        g_last_gate = gate;
        tr("pump_step: now=%u t_in=%u t_tim=%u t_idle=%u gate=%08lX",
           now, t_in, t_tim, t_idle, (unsigned long)gate);
    }

    /* `end` IS THE TERMINAL WHEN NOTHING IS PENDING -- and a live timer does not count as
     * pending, because a timer that is always due would otherwise keep the pump alive for
     * ever. Before the timer re-arm fix this branch was only reachable when no timer was
     * live at all; with the re-arm correct, id 0x16 is ALWAYS due, so the branch became
     * unreachable and the run raced past `end` indefinitely: measured, virtual
     * 5,666,940 ms against an `end 6400`, with 30,406 rand() draws. A script has to
     * terminate on its own schedule or the suite cannot run at all. So the test is on the
     * SCRIPT's pending events, and the clock is run out to `end` first, exactly as the
     * rule in docs/re/oracle.md 3.1 already says. */
    if (t_in == 0xFFFFFFFFu) {
        if (now < g_end_ms) { vadvance(g_end_ms); return 0; }   /* run the clock out */
        g_finished = 1;
        tr("pump_step: script done, posting WM_QUIT");
        if (!g_quit_posted) { g_quit_posted = 1; PostQuitMessage(0); }
        return 1;
    }

    /* `end` is the terminal when nothing is pending, never a clamp: clamping would
     * deadlock on an event scheduled past it. */
    target = now;
    if (t_idle > target) target = t_idle;
    if (t_in   < target) target = t_in;
    if (t_tim  < target) target = t_tim;
    if (target > now) vadvance(target);

    /* input wins over a timer that lands on the same millisecond */
    if (t_in <= target) {
        Event *e = &g_ev[idx];
        /* An event that needs a window waits for one, exactly as a dialog op does, but
         * not forever: a window that never appears must not hang the run. */
        if (e->kind == EV_DUMP || e->kind == EV_CLICK || e->kind == EV_RCLICK ||
            e->kind == EV_DOWN || e->kind == EV_UP || e->kind == EV_MOVE ||
            e->kind == EV_DIALOG) {
            if (!find_main() && (uint32_t)(target - e->t) < 60000u) return 0;
        }
        switch (e->kind) {
        case EV_CLICK: case EV_RCLICK: case EV_DOWN: case EV_UP: case EV_MOVE:
            do_mouse(e->x, e->y, e->kind); break;
        case EV_KEY:  do_key(e->vkey); break;
        case EV_TEXT: do_text(e->text); break;
        case EV_DUMP:
            /* WOS_WINTREE_MS=<ms> logs the tree at every dump at or past that clock
             * value, so a click coordinate can be traced to the window that owns the
             * message map instead of guessed from the art. */
            if (wintree_at && vnow() >= wintree_at) wintree_dump(0);
            do_dump(e->text ? e->text : "dump");
            break;
        case EV_DIALOG:
            /* A dialog op waits for its window: if it is not up yet the event stays
             * pending and the clock only moves to the next 20 Hz boundary. */
            if (!find_dialog_with(e->ctl)) return 0;
            do_dialog(e);
            break;
        }
        e->done = 1;
        tr("pump_step: fired event kind=%d t=%u at now=%u", (int)e->kind, e->t, vnow());
        return 1;
    }

    for (i = 0; i < MAX_TIMERS; i++) {          /* one WM_TIMER, re-armed from the deadline */
        Timer *t = &g_timers[i];
        if (t->live && !t->outstanding && t->deadline <= target) {
            t->outstanding = 1;
            /* Log every WM_TIMER the harness actually delivers, with the id and the gap
             * since the previous delivery of the SAME id. The question this answers is
             * whether the original's timers run on their nominal period or at a rate set
             * by how busy the app is: Oracle3's trace saw `WM_TIMER id 0x16` (the main
             * frame's 100 ms timer) fire ONCE, at 1500 ms, in a whole run, which is not
             * a 100 ms cadence. A timer that delivers on a period shows a constant gap
             * here; a timer whose delivery is load-dependent does not, and no fixed
             * period in the port can reproduce the second case. Bounded output: one line
             * per delivery, and a run posts tens of thousands of messages but only
             * hundreds of timers. */
            tr("timer: delivered id=%u at=%u gap=%ld", (unsigned)t->id, vnow(),
               (long)(vnow() - t->last_delivered));
            t->last_delivered = vnow();
            PostMessageA(t->hwnd, WM_TIMER, (WPARAM)t->id, 0);
            return 1;
        }
    }
    return 0;
}

/* WOS_MSGLOG=1 logs EVERY message the app's own pump takes, with the virtual clock, the
 * message id, the target window and the remove flag.  It is the measurement behind the
 * idle-rate model in docs/re/oracle.md section 3.4: the question is not "does the queue
 * ever have something in it" but "what is always in it".  Off unless asked for, because
 * it is one trace_line per PeekMessage and a run is ~2e5 of them. */
static volatile LONG g_msglog;
static volatile LONG g_msglog_n;
static volatile LONG g_idle_pass;
static uint32_t       g_msglog_until;   /* virtual ms; 0 = no bound */
static void msglog(LPMSG m, const char *fn, UINT rm, BOOL got)
{
    if (!g_msglog) return;
    if (g_msglog_until && vnow() > g_msglog_until) return;
    tr("msglog now=%u %s %s rm=%u id=%04X hwnd=%p w=%08lX l=%08lX idle=%ld n=%ld",
       vnow(), fn, got ? "GOT" : "EMPTY", rm, got ? m->message : 0u, got ? (void *)m->hwnd : 0,
       got ? (unsigned long)m->wParam : 0ul, got ? (unsigned long)m->lParam : 0ul,
       (long)g_idle_pass, (long)InterlockedIncrement(&g_msglog_n));
}
/* A message id alone does not say which window class produced it, and under Wine the
 * private ids (0x0360..0x03FF) are posted by MFC42 and by user32 itself, neither of
 * which is in the game's import table.  Naming the window once per hwnd is the cheapest
 * way to tell "the game posted this" from "the toolkit did". */
static void msglog_win(HWND h)
{
    char cls[128], txt[128];
    static HWND seen[64];
    static int nseen;
    int i;
    if (!h || !g_msglog) return;
    for (i = 0; i < nseen; i++) if (seen[i] == h) return;
    if (nseen < 64) seen[nseen++] = h;
    cls[0] = txt[0] = 0;
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, txt, sizeof txt);
    tr("msglog window hwnd=%p class=%s text=%s", (void *)h, cls, txt);
}

/* ...and this one answers "who put it there".  A module-level PostMessage/SendMessage
 * detour with the caller's return address is the only place a message's origin is
 * visible: the queue itself keeps no sender, and under Wine no IAT slot of the game
 * sits on the path of a message Wine posts itself. */
static BOOL (WINAPI *real_PostMessageA)(HWND, UINT, WPARAM, LPARAM);
static BOOL (WINAPI *real_SendMessageA)(HWND, UINT, WPARAM, LPARAM);
static BOOL WINAPI hook_PostMessageA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_msglog)
        tr("post now=%u ra=%08lX Post id=%04X hwnd=%p w=%08lX l=%08lX",
           vnow(), (unsigned long)(uintptr_t)__builtin_return_address(0), msg, (void *)h,
           (unsigned long)w, (unsigned long)l);
    return real_PostMessageA(h, msg, w, l);
}
/* `0x46F` IS LOGGED WITH ITS RETURN VALUE, ALWAYS, because it is the one call whose outcome
 * decides the front end's state 2 and nobody has measured it.
 *
 * `FUN_0041F699` (the `0x46B` "Play now" handler) does
 * `SendMessageA(*(HWND*)(DAT_004E4840 + 0x20), 0x46F, 0, 0)` at 0x41F6DC, and `0x46F`'s handler is
 * the SRNet open. The `if` after that call selects between staying in state 2 and going back to
 * state 1, and TWO runs of the same binary on the same script have been reported as behaving
 * differently -- one parked in the call for the rest of the run, one apparently not -- with
 * nobody having observed what it RETURNED. Guessing at that from the surrounding code is what
 * produced three wrong readings in a row, so the return value is written down.
 *
 * Both the call and the return are logged, because the two are different facts: the call being
 * made says the branch was reached, and the return says which way it went. A trace that recorded
 * only the call would be satisfied by a call that never came back. */
static uint32_t g_q46f_at;
static BOOL WINAPI hook_SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_msglog)
        tr("post now=%u ra=%08lX Send id=%04X hwnd=%p w=%08lX l=%08lX",
           vnow(), (unsigned long)(uintptr_t)__builtin_return_address(0), msg, (void *)h,
           (unsigned long)w, (unsigned long)l);
    if (msg == 0x46F) {
        uintptr_t ra = (uintptr_t)__builtin_return_address(0);
        tr("query 0x46F: calling at now=%u ra=%08lX hwnd=%p", vnow(),
           (unsigned long)ra, (void *)h);
        g_q46f_at = vnow();
        {
            BOOL r = real_SendMessageA(h, msg, w, l);
            tr("query 0x46F: RETURNED %d at now=%u (elapsed %lu ms)", (int)r, vnow(),
               (unsigned long)(vnow() - g_q46f_at));
            return r;
        }
    }
    return real_SendMessageA(h, msg, w, l);
}


static volatile LONG g_peeks;
static BOOL WINAPI hook_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT rm)
{
    BOOL r;
    if (InterlockedIncrement(&g_peeks) < 8)
        tr("hook_PeekMessageA: call %ld rm=%08X", (long)g_peeks, rm);
    /* Step only when the caller's queue is empty. AppRun's message loop peeks after every
     * message it dispatches; stepping there advanced the clock past 20 ms boundaries while
     * the app was still draining a timer handler's paints, so its idle path never saw them. */
    {
        MSG probe;
        if (!real_PeekMessageA(&probe, h, a, b, PM_NOREMOVE)) pump_step();
    }
    r = real_PeekMessageA(m, h, a, b, rm);
    /* Only a REMOVING peek takes the message. MFC peeks with PM_NOREMOVE before every
     * GetMessage, and counting that peek too re-armed every timer twice per delivery:
     * the 100 ms timers were delivered every 200 ms. */
    if (r && (rm & PM_REMOVE) && m->message == WM_TIMER) timer_taken(m->hwnd, m->wParam);
    if (!r) InterlockedIncrement(&g_idle_pass);
    if (r) msglog_win(m->hwnd);
    msglog(m, "Peek", rm, r);
    return r;
}

static volatile LONG g_gets;
/* WHY THE NEVER-BLOCK GUARD EXISTS, AND WHY IT HAS TO MATCH THE CALLER'S FILTER.
 *
 * The harness owns the virtual clock, and it only advances inside a hooked pump call.
 * So if the app ever reaches a REAL blocking `GetMessage`, the clock freezes and the
 * run hangs -- there is no thread left to move it.  The guard makes that impossible by
 * ensuring a message is always available before the blocking call.
 *
 * The first version of the guard was wrong in a way that cost a whole debugging pass:
 * it probed with `PeekMessage(&probe, NULL, 0, 0, PM_NOREMOVE)` -- every message to
 * every window -- and, finding none, posted `WM_NULL` to the THREAD queue.  Both halves
 * are wrong when the caller filters.  Measured with WOS_STALL=1: the app parked at
 * `win32u+0x11248` (`NtUserGetMessage`) with the return address inside hook.dll, i.e.
 * inside this very function, and the run stopped dead at 800 ms.  MFC's modal and
 * menu-tracking loops call `GetMessage(&msg, hwnd, 0, 0)` for ONE window, so a thread
 * message the caller's filter excludes is invisible to it, and the probe's "there is
 * nothing" was reading a queue the caller does not look at.
 *
 * The fix is to make the guard ask the CALLER'S question.  Two things have to match:
 * the window and the message range.  Probing with the caller's own hwnd/min/max is
 * half of it; the other half is that the synthesised wake-up has to be a message the
 * caller's filter actually accepts.  MFC's keyboard loops call
 * `GetMessage(&msg, hwnd, WM_KEYFIRST, WM_KEYLAST)`, and WM_NULL (0) is BELOW
 * WM_KEYFIRST (0x0100) -- so a guard that posts WM_NULL to a range-filtered caller is
 * invisible to it, which is a second way to hang, and the first fix did not find it
 * because the callers that hung had hwnd == NULL and no range.
 *
 * So the wake-up message is chosen from inside the caller's own [min,max] window: a
 * message the filter accepts, and one no window procedure acts on by default.  For an
 * unfiltered call (0,0) that is WM_NULL.  For a range that excludes it, WM_NULL is
 * moved up to the range's low bound, which is a synthesised no-op the same way: the
 * game never reads a message it did not post, and the alternative -- letting it block
 * -- freezes the virtual clock and loses the whole run. */
static UINT getmessage_wakeup_id(HWND h, UINT a, UINT b)
{
    UINT id = WM_NULL;
    if (a > id) id = a;
    if (b && id > b) id = b;          /* min>max is legal and matches nothing */
    (void)h;
    return id;
}
/* THE WAKE-UP IS A DUE TIMER IF THERE IS ONE, NOT ALWAYS A BARE WM_NULL.
 *
 * This is Win32's priority rule and it is not cosmetic. `GetMessage`/`PeekMessage`
 * synthesise a `WM_TIMER` for any armed timer whose deadline has passed whenever the
 * thread is about to wait, and the queue is drained in the order posted/input first,
 * then `WM_TIMER`, then `WM_PAINT`. Two consequences this harness had wrong:
 *
 *   * A continuously repainting window does NOT starve timers. Paint is the LOWEST
 *     priority, so a window that invalidates itself every frame still gets its timers.
 *   * A timer that is due and untaken is not "waiting for the app to come back" in any
 *     useful sense -- it becomes a message as soon as the app would block.
 *
 * Before this, the only thing that ever woke a blocked `GetMessage` was a bare WM_NULL,
 * so a due timer stayed unposted until the pump happened to run, and the pump only runs
 * from inside a hooked call. The measured consequence was that the original's main-frame
 * 100 ms timer (id 0x16, armed by FUN_00428360 at 0x428803) was delivered ONCE in
 * 3200 ms of idle -- an artefact of the harness, not a fact about the original. It
 * matters because the front end's state 2 ("Where Do You Want To Play Today?") advances
 * on a TIMED transition through the FUN_004057D3 gate inside FUN_0041BDB4, which is
 * reached from the 20 ms gate FUN_0040A7C7 -- i.e. from that timer. With the timer
 * starved, the state could never advance however correctly the script clicked.
 *
 * So: if the caller's queue is empty, look for the earliest due timer and post THAT;
 * only when no timer is due does the harness fall back to the inert wake-up message. */
static BOOL getmessage_wakeup(HWND h, UINT a, UINT b)
{
    UINT id = getmessage_wakeup_id(h, a, b);
    int i;
    uint32_t now = vnow();
    for (i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timers[i];
        if (t->live && !t->outstanding && t->deadline <= now) {
            t->outstanding = 1;
            tr("timer: delivered by synthesis id=%u at=%u", (unsigned)t->id, now);
            t->last_delivered = now;
            return PostMessageA(t->hwnd, WM_TIMER, (WPARAM)t->id, 0);
        }
    }
    return h ? PostMessageA(h, id, 0, 0)
             : PostThreadMessage(GetCurrentThreadId(), id, 0, 0);
}
static BOOL WINAPI hook_GetMessageA(LPMSG m, HWND h, UINT a, UINT b)
{
    BOOL r;
    InterlockedIncrement(&g_gets);
    if (InterlockedCompareExchange(&g_gets, 0, 0) < 8)
        tr("hook_GetMessageA: call %ld hwnd=%p min=%04X max=%04X",
           (long)g_gets, (void *)h, a, b);
    /* THE PROBE IS THE AUTHORITY ON "IS THE QUEUE EMPTY", NOT pump_step's RETURN VALUE.
     *
     * The obvious guard -- "only probe if pump_step said it posted nothing" -- is
     * wrong, and it is what hung this harness.  pump_step returns 1 for two different
     * situations: it genuinely posted a message, and it ran an event that posts
     * nothing.  `dump` is the second kind: it writes a file and calls RedrawWindow,
     * which is a synchronous SendMessage and leaves the queue untouched.  So after a
     * dump at t=800 the guard was skipped, the queue was empty, and
     * real_GetMessageA slept forever -- the virtual clock stopped with it, because
     * the clock only moves inside a pump call.  Measured with WOS_STALL=1: the app
     * parked at win32u+0x11248 with the return address at hook.dll+0x248D, which is
     * the instruction after this function's own real_GetMessageA call.
     *
     * So: pump, then ALWAYS probe with the caller's own filter, and wake only what the
     * probe says is missing.  One extra PeekMessage per GetMessage is a rounding error
     * against a run that posts hundreds of thousands of messages, and it removes the
     * whole class of bug where the harness's idea of "did I post something" and
     * user32's disagree. */
    {
        MSG probe;
        /* Step only when the queue is empty, as hook_PeekMessageA does: a GetMessage
         * that takes a message already queued costs no time on Windows. Stepping here
         * moved the clock 10 ms between the WM_MOUSEMOVE and the WM_LBUTTONDOWN of one
         * scripted click (both posted at 200, the button taken at 210). */
        memset(&probe, 0, sizeof probe);
        if (!real_PeekMessageA(&probe, h, a, b, PM_NOREMOVE)) {
            pump_step();
            memset(&probe, 0, sizeof probe);
            if (!real_PeekMessageA(&probe, h, a, b, PM_NOREMOVE))
                getmessage_wakeup(h, a, b);
        }
    }
    r = real_GetMessageA(m, h, a, b);
    if (r && m->message == WM_TIMER) timer_taken(m->hwnd, m->wParam);
    if (r) msglog(m, "Get", PM_REMOVE, r);
    return r;
}

/* ------------------------------------------------------------- the script thread */
static char g_scratch[64];


/* Deliberately NOT a separate thread.  A thread created from DllMain under the loader
 * lock has a stack the loader has already eaten into; running a file parse plus
 * vsnprintf on it walks off the guard page.  That killed the thread while it still held
 * the process heap lock, which deadlocked the game and produced a bogus
 * "<path> was not found." box -- an artefact of the harness, not a fact about the game.
 * This runs once, on the game's own main thread, from inside the pump. */
static void script_init(void)
{
    static volatile LONG done;
    char v[1024];
    DWORD n;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    n = GetEnvironmentVariableA("WOS_DSC", v, sizeof v);
    tr("script_init: WOS_DSC=%s (n=%lu)", n ? v : "<unset>", (unsigned long)n);
    if (!n || n >= sizeof v) {
        tr("script_init: FATAL WOS_DSC not set");
        return;
    }
    n = GetEnvironmentVariableA("WOS_OUT", g_outdir, sizeof g_outdir);
    if (!n || n >= sizeof g_outdir) strcpy(g_outdir, ".");
    tr("script_init: WOS_OUT=%s randtrace=%d parsing %s", g_outdir, (int)g_rand_trace, v);
    n = GetEnvironmentVariableA("WOS_MSGLOG", g_scratch, sizeof g_scratch);
    if (n && n < sizeof g_scratch) {
        g_msglog = atol(g_scratch);
        n = GetEnvironmentVariableA("WOS_MSGLOG_MS", g_scratch, sizeof g_scratch);
        g_msglog_until = (n && n < sizeof g_scratch) ? (uint32_t)atol(g_scratch) : 0u;
        tr("script_init: msglog=%d until=%u", (int)g_msglog, g_msglog_until);
    }
    /* WOS_WINTREE=1 logs every descendant of the main window with its class, caption,
     * client rect in client-of-main coordinates and visibility.  A .dsc click has to
     * land on the window that owns the message map, and the only way to know which
     * window that is at a given point is to read the tree rather than guess from the
     * art -- the title screen alone has a frame, a view, a splitter pane and a
     * Book of Tactics dialog stacked over each other. */
    n = GetEnvironmentVariableA("WOS_WINTREE", g_scratch, sizeof g_scratch);
    if (n && n < sizeof g_scratch) {
        wintree_dump(1);
        n = GetEnvironmentVariableA("WOS_WINTREE_MS", g_scratch, sizeof g_scratch);
        wintree_at = (n && n < sizeof g_scratch) ? (uint32_t)atol(g_scratch) : 0u;
    }
    n = GetEnvironmentVariableA("WOS_STALL", g_scratch, sizeof g_scratch);
    if (n && n < sizeof g_scratch) stall_dump = atol(g_scratch);
    /* Force the client to 640x480 BEFORE the front end lays itself out, not lazily on
     * the first click.  The front end computes every hotspot rect from `GetClientRect`
     * when a state is entered (FUN_00405153: pixel = per_mille * client / 1000), so a
     * resize that lands after that leaves every stored rect describing a client area
     * that no longer exists -- and a click coordinate derived from such a rect is off
     * screen.  Measured: with the resize deferred to the first input, "Play now" was
     * registered at rect (-269,397)-(282,606), i.e. 126 px below a 480-tall client,
     * and was unreachable.  Both sides must be at 640x480 before the first state. */
    ensure_main();
    if (!g_app_tid) g_app_tid = GetCurrentThreadId();
    if (!parse_script(v)) { tr("script_init: FATAL parse failed"); return; }
    tr("script_init: parsed %d events, end=%u", g_nev, g_end_ms);
    InterlockedExchange(&g_ready, 1);
}

/* --------------------------------------------------------- the modal dialog loop */

/* WHY THIS EXISTS.  Under Wine the modal loop of a dialog is NOT a call the game
 * makes: `DialogBoxIndirectParam` is implemented inside user32, and it enters its
 * own loop on win32u's NtUserGetMessage/NtUserPeekMessage.  No import slot of
 * Souls.exe, MFC42.DLL or any other module is on that path, so the pump hooks --
 * which are IAT patches -- are never reached, and the game sits in the box
 * forever with a frozen virtual clock.  (Measured: the game's main thread parks
 * in `anon_pipe_read` on the console pipe inside `NtUserGetMessage`, one thread,
 * 0% CPU, no `hook_PeekMessageA` line ever logged.)
 *
 * The fix is to own the loop.  MFC reaches the dialog through
 * `CDialog::DoModal` -> `::DialogBoxParam`/`::DialogBoxIndirectParam`, which ARE
 * IAT slots, so the hook replaces them with the same call Win32 makes: disable
 * the owner, create the dialog, then run GetMessage/IsDialogMessage/
 * TranslateMessage/DispatchMessage until `EndDialog` or WM_QUIT.  The only
 * difference from Wine's loop is that `pump_step()` runs first on every
 * iteration, so the virtual clock advances and scripted input is delivered
 * while a dialog is up.  That is the same rule as the main loop
 * (docs/re/oracle.md section 3.1): the clock only moves while the queue is
 * empty.
 *
 * `EndDialog` is hooked for the same reason -- it is what tells the loop to
 * stop, and the real one has no way to reach a loop we own. */

/* This toolchain's winuser.h predates DLGTPROCW (the ANSI/Unicode pair was still
 * behind UNICODE macros), and the W entry points are exactly the ones the W
 * dialog path needs.  Same prototype as DLGPROC: the W-ness is in the strings,
 * not in the signature. */
#ifndef DLGTPROCW
typedef INT_PTR (CALLBACK *DLGTPROCW)(HWND, UINT, WPARAM, LPARAM);
#endif

#define MAX_MODAL 8
typedef struct {
    HWND hwnd;              /* the dialog, NULL once EndDialog has fired */
    int  done;
    int  result;
} ModalFrame;
static ModalFrame g_modal[MAX_MODAL];
static int        g_modal_depth;

static int  (WINAPI *real_DialogBoxParamA)(HINSTANCE, UINT_PTR, HWND, DLGPROC, LPARAM);
static int  (WINAPI *real_DialogBoxParamW)(HINSTANCE, UINT_PTR, HWND, DLGTPROCW, LPARAM);
static int  (WINAPI *real_DialogBoxIndirectParamA)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM);
static int  (WINAPI *real_DialogBoxIndirectParamW)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGTPROCW, LPARAM);
static HWND (WINAPI *real_CreateDialogIndirectParamA)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM);
static HWND (WINAPI *real_CreateDialogIndirectParamW)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGTPROCW, LPARAM);
static BOOL (WINAPI *real_EndDialog)(HWND, INT_PTR);
static BOOL (WINAPI *real_IsDialogMessageA)(HWND, LPMSG);
static BOOL (WINAPI *real_IsDialogMessageW)(HWND, LPMSG);
static BOOL (WINAPI *real_TranslateMessage)(const MSG *);
static BOOL (WINAPI *real_DispatchMessageA)(const MSG *);
static BOOL (WINAPI *real_DispatchMessageW)(const MSG *);
static int  (WINAPI *real_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static int  (WINAPI *real_MessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT);

/* --- the dialog template, read out of the resource ------------------------
 * The resource id and the control ids are what a .dsc `dialog` op needs, and
 * reading them off the live template is the only place they are exact: the
 * caption in the resource is a caption, and the ids are the ids. */
static const WORD *dlg_str(const WORD *p, char *out, int outsz)
{
    int i = 0;
    out[0] = 0;
    if (*p == 0x0000) return p + 1;                 /* absent */
    if (*p == 0xFFFF) {                              /* ordinal */
        if (outsz > 12) wsprintfA(out, "#%u", (unsigned)p[1]);
        return p + 2;
    }
    while (p[i] && i < outsz - 1) { out[i] = (char)p[i]; i++; }
    out[i] = 0;
    return p + i + 1;
}
static void log_template(const char *what, HINSTANCE inst, const void *tmpl, int wide)
{
    const WORD *p = (const WORD *)tmpl;
    char title[128], cls[64];
    int ex = 0, cdit = 0, i;
    if (!p) { tr("modal: %s id=? template=NULL", what); return; }
    if (p[0] == 1 && p[1] == 0xFFFF) {              /* DLGTEMPLATEEX */
        ex = 1;
        cdit = p[8];
        p += 13;                                    /* ver sig helpID ex style cdit x y cx cy */
    } else {
        cdit = p[4];
        p += 9;                                     /* style exStyle cdit x y cx cy */
    }
    p = dlg_str(p, cls, sizeof cls);                 /* menu */
    p = dlg_str(p, cls, sizeof cls);                 /* class */
    if (wide) {
        char tmp[128];
        int k = 0;
        while (p[k] && k < 126) { tmp[k] = (char)p[k]; k++; }
        tmp[k] = 0;
        p += k + 1;
        title[0] = 0;
        for (i = 0; tmp[i] && i < 126; i++)          /* the caption is ASCII in this image */
            title[i] = (tmp[i] >= 32 && tmp[i] < 127) ? tmp[i] : '?';
        title[i] = 0;
    } else {
        p = dlg_str(p, title, sizeof title);
    }
    tr("modal: %s %s cdit=%d ex=%d class=\"%s\" title=\"%s\"",
       what, inst ? "hinst" : "NULL", cdit, ex, cls, title);
    for (i = 0; i < cdit; i++) {
        int id;
        if (ex) { id = (int)((unsigned)p[10] | ((unsigned)p[11] << 16)); p += 12; }
        else    { id = (int)(int16_t)p[9]; p += 10; }
        tr("modal:   ctl %d", id);
        p = dlg_str(p, cls, sizeof cls);             /* class */
        p = dlg_str(p, cls, sizeof cls);             /* title */
        p += 1;                                      /* creation data: WORD count */
    }
}

/* The loop.  `tmpl` is the template in memory; `owner` is disabled exactly as
 * Win32's DialogBox does, and re-enabled on the way out. */
static int modal_run(const char *what, HINSTANCE inst, const void *tmpl, int wide,
                     HWND owner, DLGPROC procA, DLGTPROCW procW, LPARAM lp)
{
    ModalFrame *f;
    MSG msg;
    HWND hwnd;
    int result = 0;
    if (g_modal_depth >= MAX_MODAL) {               /* deeper than any real nest */
        tr("modal: %s: nesting deeper than %d, calling through", what, MAX_MODAL);
        return wide ? real_DialogBoxIndirectParamW(inst, (LPCDLGTEMPLATE)tmpl, owner, procW, lp)
                    : real_DialogBoxIndirectParamA(inst, (LPCDLGTEMPLATE)tmpl, owner, procA, lp);
    }
    hwnd = wide ? real_CreateDialogIndirectParamW(inst, (LPCDLGTEMPLATE)tmpl, owner, procW, lp)
                : real_CreateDialogIndirectParamA(inst, (LPCDLGTEMPLATE)tmpl, owner, procA, lp);
    if (!hwnd) { tr("modal: %s: CreateDialogIndirectParam failed (%lu)", what,
                     (unsigned long)GetLastError()); return -1; }
    f = &g_modal[g_modal_depth++];
    f->hwnd = hwnd; f->done = 0; f->result = 0;
    tr("modal: %s up as %p, entering the loop", what, (void *)hwnd);
    if (owner) EnableWindow(owner, FALSE);
    for (;;) {
        pump_step();                       /* the virtual clock, and scripted input */
        if (f->done) break;
        if (!real_PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) continue;
        if (msg.message == WM_QUIT) {
            /* Win32 leaves WM_QUIT in the queue for the loop that is above us. */
            tr("modal: %s: WM_QUIT out of the dialog loop", what);
            PostQuitMessage(0);
            break;
        }
        if (f->hwnd && (wide ? real_IsDialogMessageW(f->hwnd, &msg)
                             : real_IsDialogMessageA(f->hwnd, &msg)))
            continue;                                /* tab, arrows, default button */
        real_TranslateMessage(&msg);
        if (wide) real_DispatchMessageW(&msg); else real_DispatchMessageA(&msg);
    }
    result = f->result;
    if (f->hwnd && IsWindow(f->hwnd)) DestroyWindow(f->hwnd);
    f->hwnd = NULL;
    if (owner) EnableWindow(owner, TRUE);
    --g_modal_depth;
    tr("modal: %s down, result=%d", what, result);
    return result;
}

static BOOL WINAPI hook_EndDialog(HWND h, INT_PTR r)
{
    int i;
    for (i = g_modal_depth - 1; i >= 0; i--)
        if (g_modal[i].hwnd == h) {
            g_modal[i].result = (int)r;
            g_modal[i].done = 1;
            g_modal[i].hwnd = NULL;
            tr("modal: EndDialog(%p,%d)", (void *)h, (int)r);
            DestroyWindow(h);
            return 1;
        }
    return real_EndDialog(h, r);
}

static int modal_template(HINSTANCE inst, UINT_PTR name, const void **out)
{
    HANDLE hr = (HANDLE)FindResourceA(inst, (LPCSTR)name, (LPCSTR)RT_DIALOG);
    HGLOBAL hg;
    if (!hr) return 0;
    hg = (HGLOBAL)LoadResource(inst, hr);
    if (!hg) return 0;
    *out = LockResource(hg);
    return 1;
}

static int WINAPI hook_DialogBoxParamA(HINSTANCE inst, UINT_PTR name, HWND owner,
                                       DLGPROC proc, LPARAM lp)
{
    const void *tmpl = NULL;
    char what[64];
    if (!real_DialogBoxParamA || !modal_template(inst, name, &tmpl)) {
        tr("modal: DialogBoxParamA: template %u not found, calling through", (unsigned)name);
        return real_DialogBoxParamA(inst, name, owner, proc, lp);
    }
    log_template("DialogBoxParamA", inst, tmpl, 0);
    wsprintfA(what, "DialogBoxParamA(%u)", (unsigned)name);
    return modal_run(what, inst, tmpl, 0, owner, proc, NULL, lp);
}

/* The call chain above a hook point, filtered to things that really are return
 * addresses: a stack full of .data pointers looks exactly like a stack full of
 * return addresses otherwise.  A value `v` counts only if the instruction that
 * ends at `v` is a call -- E8 rel32, FF /2, FF /3, or FF /4 through a register
 * or memory operand.  MFC sits at 0x5F4xxxxx and the game at 0x40xxxx-0x50xxxx,
 * and the game's frame is the one that turns a mystery box into a VA. */
static int looks_like_ret(DWORD v)
{
    const unsigned char *p = (const unsigned char *)v;
    if ((p[-5] & 0xFF) == 0xE8) return 1;                     /* call rel32 */
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;     /* call [r/m] */
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return 1;     /* call r32 */
    return 0;
}
static void log_callers(const char *tag)
{
    DWORD pad[8];
    DWORD *sp = pad;                 /* not __builtin_frame_address: -O2 drops the
                                     * frame pointer, and the scan has to start
                                     * inside THIS frame and walk up */
    int i, n = 0;
    for (i = 0; i < 1024 && n < 24; i++) {
        DWORD v = sp[i];
        if (v >= 0x00300000u && v < 0x7E000000u && looks_like_ret(v)) {
            const char *who = v < 0x00500000u ? "  game"
                         : v < 0x00600000u ? "  mfc"
                         : v < 0x11000000u ? "  srmisc" : "  sys";
            tr("callers[%s]: %08lX%s", tag, (unsigned long)v, who);
            ++n;
        }
    }
}
static int WINAPI hook_DialogBoxIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATE tmpl,
                                               HWND owner, DLGPROC proc, LPARAM lp)
{
    log_template("DialogBoxIndirectParamA", inst, tmpl, 0);
    return modal_run("DialogBoxIndirectParamA", inst, tmpl, 0, owner, proc, NULL, lp);
}
static int WINAPI hook_DialogBoxParamW(HINSTANCE inst, UINT_PTR name, HWND owner,
                                       DLGTPROCW proc, LPARAM lp)
{
    const void *tmpl = NULL;
    char what[64];
    if (!real_DialogBoxParamW || !modal_template(inst, name, &tmpl)) {
        tr("modal: DialogBoxParamW: template %u not found, calling through", (unsigned)name);
        return real_DialogBoxParamW(inst, name, owner, proc, lp);
    }
    log_template("DialogBoxParamW", inst, tmpl, 1);
    wsprintfA(what, "DialogBoxParamW(%u)", (unsigned)name);
    return modal_run(what, inst, tmpl, 1, owner, NULL, proc, lp);
}
static int WINAPI hook_DialogBoxIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATE tmpl,
                                               HWND owner, DLGTPROCW proc, LPARAM lp)
{
    log_template("DialogBoxIndirectParamW", inst, tmpl, 1);
    return modal_run("DialogBoxIndirectParamW", inst, tmpl, 1, owner, NULL, proc, lp);
}

/* A message box is the one modal thing that is NOT a dialog resource, and under a
 * headless X server it is a deadlock with no upside.  The harness answers IDOK and
 * writes down what it said: the text is the evidence, the answer is the only
 * thing that keeps the run alive.  A run that hits one says so in the log, and
 * the dump after it is the one to read. */
static int WINAPI hook_MessageBoxA(HWND owner, LPCSTR text, LPCSTR cap, UINT type)
{
    tr("modal: MessageBoxA(owner=%p, type=0x%X) caption=\"%s\" text=\"%s\" -> IDOK",
       (void *)owner, (unsigned)type, cap ? cap : "", text ? text : "");
    log_callers("MessageBoxA");
    return IDOK;
}
static int WINAPI hook_MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR cap, UINT type)
{
    char t[256], c[128];
    int i = 0, j = 0;
    while (text && text[i] && i < 250) { t[i] = (char)text[i]; i++; }
    t[i] = 0;
    while (cap && cap[j] && j < 120) { c[j] = (char)cap[j]; j++; }
    c[j] = 0;
    tr("modal: MessageBoxW(owner=%p, type=0x%X) caption=\"%s\" text=\"%s\" -> IDOK",
       (void *)owner, (unsigned)type, c, t);
    log_callers("MessageBoxW");
    return IDOK;
}



/* --------------------------------------------------------------------- install */

/* --- file-access tracing (WOS_DETOURS group "trace") ------------------------
 * The exact path that fails, and WHERE it failed from: the return address is
 * the single most useful thing in this whole harness when the game asks for a
 * file that is not there, because it maps straight onto a VA in the decomp. */
static int g_trace_on;
static int (__cdecl *real__access)(const char *, int);
static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                         DWORD, DWORD, HANDLE);
static FILE *(__cdecl *real_fopen)(const char *, const char *);

static int __cdecl hook__access(const char *path, int mode)
{
    int r = real__access(path, mode);
    if (r == -1 && g_trace_on)
        tr("TRACE _access(\"%s\",%d) -> -1 at %08lX", path ? path : "(null)", mode,
           (unsigned long)(uintptr_t)__builtin_return_address(0));
    return r;
}
static HANDLE WINAPI hook_CreateFileA(LPCSTR path, DWORD acc, DWORD sh,
                                     LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    HANDLE h = real_CreateFileA(path, acc, sh, sa, cd, fl, t);
    if (h == INVALID_HANDLE_VALUE && g_trace_on)
        tr("TRACE CreateFileA(\"%s\",0x%lX) -> FAIL at %08lX", path ? path : "(null)",
           (unsigned long)acc, (unsigned long)(uintptr_t)__builtin_return_address(0));
    return h;
}
static FILE *__cdecl hook_fopen(const char *path, const char *mode)
{
    FILE *f = real_fopen(path, mode);
    if (!f && g_trace_on)
        tr("TRACE fopen(\"%s\",\"%s\") -> NULL at %08lX", path ? path : "(null)",
           mode ? mode : "", (unsigned long)(uintptr_t)__builtin_return_address(0));
    return f;
}

/* --- the module list, read out of the PEB ---------------------------------
 * Which module a patch landed in only means something if the module has a
 * name, and half the job of this harness is telling "MFC42 did not import
 * DialogBoxIndirectParamW" from "the module that did is not the one you
 * think".  Wine maps the 32-bit view in the low 2 GB, so the bases alone are
 * ambiguous. */
static void log_modules(void)
{
    HANDLE snap;
    MODULEENTRY32 me;
    /* ToolHelp, not the PEB: a PEB walk is one struct offset away from wrong on
     * every Wine build, and this runs once. */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { tr("modules: snapshot failed"); return; }
    me.dwSize = sizeof me;
    if (Module32First(snap, &me))
        do { tr("modules: %08lX %s", (unsigned long)(uintptr_t)me.modBaseAddr, me.szModule); }
        while (Module32Next(snap, &me));
    CloseHandle(snap);
}

/* --- the watchdog -----------------------------------------------------------
 * One question this harness has to be able to answer at any moment: is the game
 * still moving, and if not, what is it looking at?  When a run produces no
 * dumps the difference between "the pump is running and the script is done" and
 * "the app is parked inside a loop that is not ours" is invisible from the
 * outside, and that is the whole diagnosis.
 *
 * The thread is created from DllMain, which is a place threads should not be
 * created from, so it does exactly one thing before it touches anything: it
 * sleeps long enough for DllMain to have returned and the loader lock to be
 * gone.  It never calls LoadLibrary or GetProcAddress, and it is given an
 * explicit stack because the default one is a liability here. */
static volatile LONG g_watchdog_started;
/* The index of the next un-fired event, for the watchdog's one line. */
static int pump_next_event(void)
{
    int i;
    for (i = 0; i < g_nev; i++) if (!g_ev[i].done) return i;
    return -1;
}
/* `WOS_STALL=1`: when the pump counters stop moving, say where the app is parked.
 *
 * The counters alone cannot distinguish "the app is blocked in a real GetMessage with
 * an empty queue" from "the app is inside a long operation" from "the app crashed into
 * a modal box the hook does not own", and those need three different fixes.  So the
 * watchdog suspends the main thread, reads its context, and logs the EIP plus a bounded
 * EBP-chain walk of return addresses.  `DbgBreakPoint` is not used and no debugger is
 * attached, so the suspend is brief and the process resumes exactly where it was. */
static void stall_report(void)
{
    HANDLE t;
    CONTEXT ctx;
    DWORD want = g_app_tid;
    int k;
    /* g_app_tid, not "the first thread that is not me": a Wine process has RPC and
     * console helper threads whose EIP sits in ntdll, and reporting one of those says
     * nothing about the app.  The app's own thread is recorded the first time the
     * script parser runs, which is by definition the thread that drives the pump. */
    if (!want) { tr("stall: g_app_tid not recorded yet"); return; }
    {
        DWORD_PTR fs;
        t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                       THREAD_QUERY_INFORMATION, FALSE, want);
        if (!t) { tr("stall: OpenThread(%lu) failed %lu", want, (DWORD)GetLastError()); return; }
        SuspendThread(t);
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!GetThreadContext(t, &ctx)) {
            tr("stall: GetThreadContext failed %lu", (DWORD)GetLastError());
            ResumeThread(t); CloseHandle(t); return;
        }
        tr("stall: tid=%lu eip=%08lX ebp=%08lX esp=%08lX",
           want, (unsigned long)ctx.Eip, (unsigned long)ctx.Ebp, (unsigned long)ctx.Esp);
        /* the return-address chain, bounded: 6 frames is enough to name the call path
         * and cannot walk off a stack that is nearly exhausted */
        fs = (DWORD_PTR)ctx.Ebp;
        for (k = 0; k < 6; k++) {
            DWORD prev, ret;
            /* No SEH: this is built with -fno-exceptions and no __try support, so the
             * frame pointer is validated by hand instead.  A plausible EBP is one that
             * is above the stack pointer, below the top of the thread's committed stack,
             * and monotonically increasing as the chain is walked -- which is exactly
             * the condition that a corrupted chain would violate. */
            if (fs < (DWORD_PTR)ctx.Esp || fs > (DWORD_PTR)ctx.Esp + 0x200000u) {
                tr("stall:   frame %d: ebp %08lX out of range (esp %08lX)",
                   k, (unsigned long)fs, (unsigned long)ctx.Esp);
                break;
            }
            prev = *(DWORD *)fs;
            ret  = *(DWORD *)(fs + 4);
            tr("stall:   frame %d ret=%08lX prev=%08lX", k, (unsigned long)ret,
               (unsigned long)prev);
            if (ret < 0x00400000u || ret > 0x00500000u) {
                tr("stall:   (left Souls.exe: .text)");
                break;
            }
            if (prev <= fs) break;
            fs = prev;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
}
static DWORD WINAPI watchdog_thread(LPVOID unused)
{
    (void)unused;
    /* Counters only.  EnumWindows from this thread is what the first version
     * did, and it never came back: the main thread is parked inside a user32
     * wait while the watchdog is asking user32 for the window list.  A
     * diagnostic that can deadlock the thing it measures is not a diagnostic. */
    Sleep(4000);                       /* let DllMain return and the game start */
    for (;;) {
        LONG last;
        Sleep(3000);
        last = InterlockedCompareExchange(&g_steps, 0, 0);
        tr("watch: steps=%ld peeks=%ld gets=%ld modal=%d ready=%ld finished=%d "
           "rng=%ld ticks=%u next_in=%d",
           (long)g_steps, (long)g_peeks, (long)g_gets, g_modal_depth,
           (long)g_ready, g_finished, (long)g_rand_calls, vnow(), pump_next_event());
        /* A stall is the one failure the counters cannot explain.  `steps` not moving
         * means the app is no longer calling any hooked pump entry point, and the only
         * way to say WHERE it is parked is to read its context.  The main thread's EIP
         * is mapped back through the image base to a VA in Souls.exe, and a bounded
         * EBP-chain walk gives the callers above it, which is enough to name the
         * function without a symbol server.  Off unless WOS_STALL=1: suspending the
         * app's own thread changes the run's timing, so it must be a deliberate act. */
        if (stall_dump && last == InterlockedCompareExchange(&g_steps, 0, 0))
            stall_report();
    }
    return 0;
}
static void start_watchdog(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_watchdog_started, 1, 0) != 0) return;
    t = CreateThread(NULL, 128 * 1024, watchdog_thread, NULL, 0, NULL);
    tr("watch: watchdog thread %p", (void *)t);
}


/* WOS_DETOURS selects which groups are installed, for bisecting a behaviour difference
 * against the unhooked game.
 *
 * The default is the CORE set -- the hooks a differential run needs and nothing
 * else.  The rest are diagnostics that cost a great deal of wall time (a traced
 * run does 500k relay-worthy calls in the first two seconds) and must be asked
 * for by name:
 *
 *   core  (default) pump, timer, key, tick, wall, clock, rng, help, modal, focus
 *   probe           one-shot dump of the live options table (the "values=" line)
 *   trace           every failing _access/fopen/CreateFileA, with its path
 *   watch           log every modal dialog, with its resource id, caption and
 *                   control ids, and auto-answer every MessageBox
 *
 * An unset variable means the core set; a set variable means exactly the names
 * it lists (plus "all" for everything). */
static int in_list(char *list, const char *name)
{
    char *p = list, *q;
    for (;;) {
        q = strchr(p, ',');
        if (q) *q = 0;
        if (!strcmp(p, name)) return 1;
        if (!q) return 0;
        p = q + 1;
    }
}
static int grp(const char *name)
{
    char core[] = "pump,timer,key,tick,wall,clock,rng,help,modal,watch,focus";
    char buf[256];
    DWORD n = GetEnvironmentVariableA("WOS_DETOURS", buf, sizeof buf);
    if (!n) return in_list(core, name);
    if (!buf[0]) return 0;
    return in_list(buf, name) || in_list(buf, "all");
}

/* GetForegroundWindow: report the game's main frame. The Run loop (0x0040A8D9) spins its
 * idle path, and with it FUN_0040A7C7's 20 ms gate, only while the foreground window's
 * top-level parent is the main frame (*0x004E4844); otherwise it blocks in GetMessage
 * and only the 100 ms WM_TIMER (FUN_00428C8F) enters the gate. Under Xvfb with no window
 * manager the foreground window drifts with whatever the title's timers show, so the idle
 * cadence depended on the Wine build. A player has the game focused, and so does the
 * oracle. m_hWnd is at +0x20 (SetTimer(*(this+0x20), ...) throughout); else fall through. */
static HWND (WINAPI *real_GetForegroundWindow)(void);
static HWND WINAPI hook_GetForegroundWindow(void)
{
    static int logged;
    uintptr_t frame = *(uintptr_t *)(g_base + (0x004E4844u - IMAGE_BASE));
    HWND w = frame ? *(HWND *)(frame + 0x20) : NULL;
    if (!w || !IsWindow(w)) w = real_GetForegroundWindow();
    if (!logged && frame) { logged = 1; tr("focus: main frame %p hwnd %p", (void *)frame, (void *)w); }
    return w;
}

static BOOL (WINAPI *real_SetCursorPos)(int, int);
static BOOL WINAPI hook_SetCursorPos(int x, int y)
{
    if (g_msglog)
        tr("post now=%u ra=%08lX SetCursorPos %d,%d", vnow(),
           (unsigned long)(uintptr_t)__builtin_return_address(0), x, y);
    return real_SetCursorPos(x, y);
}

/* ------------------------------------------------- front-end state machine trace
 *
 * WOS_DETOURS=front traces the front end's own functions, which no IAT can reach:
 * they are in Souls.exe and every one of them is called by address from a vtable or
 * a message map, never through an import.  patch_text() is the same six-byte
 * `push imm32; ret` the module detours use, and the thunks below rebuild the
 * displaced prologue in fresh memory so the original still runs.
 *
 * What it answers, in one line each:
 *   0x0041B891  FUN_0041b891(state)  -- every state transition, with the caller VA,
 *                so "state 2 is never left" becomes "nobody CALLS the state that
 *                would leave it" rather than an inference from a dump.
 *   0x0041D31F  the state-2 title label
 *   0x0041D374  the "*** Scanning ***" label
 *   0x0041D3CC  the world-list rows (0x472/0x473/0x474 hotspots)
 *   0x0041D717  the world-list init (enumerates worlds\*)
 *   0x0043E8E8  the solo stepper FUN_00438e8e
 */
static const char *g_front_name[16];
static volatile LONG g_front_n;
static int g_front_hot[16];
static DWORD g_front_hot_at[16];

/* NOT static: the hand-written assembly below calls it by its C name, and a static
 * function has internal linkage, so the assembler reference is an undefined symbol at
 * link time. */
void front_log_c(int idx, void *self, void *ret, int arg)
{
    /* Two of the traced functions run at 20 Hz and would write a line every 50 ms for
     * the whole run, which buries the handful of lines that matter.  They are logged on
     * their first call and then once a second, which is enough to show that they are
     * running at all and how their argument changes; the state transitions and the
     * label registrars are all low-frequency and are logged in full. */
    if (g_front_hot[idx]) {
        DWORD t = (DWORD)vnow();
        if (t - g_front_hot_at[idx] < 1000u) return;
        g_front_hot_at[idx] = t;
    }
    tr("front %ld %s this=%08X ret=%08X arg=%d now=%u",
       (long)InterlockedIncrement(&g_front_n),
       g_front_name[idx] ? g_front_name[idx] : "?",
       (unsigned)(uintptr_t)self, (unsigned)(uintptr_t)ret,
       arg, (unsigned)vnow());
}

/* The stack offsets are read HERE, in hand-written assembly, because a C function
 * cannot be relied on to be entered at its first instruction: the compiler may push
 * registers and open a frame before the first statement, and every offset below would
 * then be off by that frame.  The layout on entry, with E = the thunk's ESP:
 *   [E+0x00] return address back into the thunk          (call rel32)
 *   [E+0x04] the slot index                              (push imm32)
 *   [E+0x08..0x28] pushad's eight dwords, ascending: EDI, ESI, EBP, the pushed ESP,
 *                  EBX, EDX, ECX, EAX -- so ECX, which is the `this` of a
 *                  __thiscall callee, is at E+0x20
 *   [E+0x28] the ORIGINAL return address
 *   [E+0x2C] the original first stack argument
 * After `push ebp; mov esp,ebp; sub esp,8` those become ebp+0x28, +0x2c and +0x30.
 * This is cdecl, so the thunk's `popad` puts every register back and the ESP is
 * exactly what the relocated prologue expects. */
__asm__(
    ".text\n"
    ".globl _front_log_asm\n"
    "_front_log_asm:\n"
    "  pushl %ebp\n"
    "  movl  %esp, %ebp\n"
    "  subl  $8, %esp\n"
    "  pushl 0x30(%ebp)\n"        /* original first stack argument */
    "  pushl 0x2c(%ebp)\n"        /* original return address        */
    "  pushl 0x24(%ebp)\n"        /* ECX, i.e. `this`               */
    "  pushl 0x08(%ebp)\n"        /* slot index                     */
    "  call _front_log_c\n"
    "  addl  $16, %esp\n"
    "  leave\n"
    "  ret\n");
void front_log_asm(void);

/* The trampoline.  `call front_log_asm` is cdecl and must not clobber the callee's
 * registers, so every register the displaced prologue needs is pushed first and the
 * ESP is realigned back to exactly what the original prologue expects.  The displaced
 * bytes are COPIED into the same block, not re-executed in place, because the entry
 * is overwritten. */
static void *front_make_thunk(int idx, uintptr_t resume, unsigned char *prologue, int plen)
{
    unsigned char *p;
    intptr_t here, target;
    int call_at, jmp_at, out_at, base;
    /* pushad(1) push imm32(5) call rel32(5) add esp,4(3) popad(1) jmp rel32(5) = 20 */
    unsigned char head[] = { 0x60, 0x68, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x61, 0xE9 };
    p = (unsigned char *)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!p) return NULL;
    memcpy(p, head, sizeof head);
    /* `push imm32` is a 0x68 opcode plus FOUR immediate bytes, so the index goes at
     * p+2.  Writing it at p+1 overwrites the opcode with a displacement, the thunk
     * then executes whatever instruction that happens to decode to, and the game dies
     * on the first traced call with nothing logged -- which is exactly what a patch
     * that logs its own installation and then nothing else looks like. */
    memcpy(p + 2, &idx, 4);
    call_at = 7; out_at = 15; base = 24;
    here   = (intptr_t)(p + call_at + 4);
    target = (intptr_t)front_log_asm;
    memcpy(p + call_at, &(int32_t){ (int32_t)(target - here) }, 4);
    /* The displaced prologue is COPIED here and replayed by the thunk, then a jump back
     * into the rest of the function.  It has to be copied: patch_text() overwrites the
     * bytes it is copied from, and the thunk is built before patch_text() runs, so the
     * copy is the original code.
     *
     * patch_text() ALWAYS writes six bytes (`push imm32; ret`), so a prologue shorter
     * than six cannot be replayed from a copy of `plen` bytes and resumed at p+plen: the
     * sixth byte of the function has been overwritten with the trampoline's own `ret`,
     * and resuming there re-executes a `ret` in the middle of a prologue -- which is
     * silent until the function is actually called, and then the run simply stops with
     * no further trace lines.  The copy is therefore `plen` rounded UP to the patch
     * width, and the resume is p+that width, so the extra bytes are replayed rather
     * than skipped.  (An earlier version of this also had the rel32 fields one byte
     * low, which is a second, independent way to build a thunk that jumps nowhere.)
     *
     * A rel32 field starts ONE PAST its opcode and `here` is the END of the whole
     * instruction, i.e. field+4. */
    {
        int wide = (plen < 6) ? 6 : plen;
        memcpy(p + base, prologue, (size_t)wide);
        memcpy(p + base + wide, "\xE9\0\0\0\0", 5);
        jmp_at = base + wide + 1;
        here   = (intptr_t)(p + jmp_at + 4);
        target = (intptr_t)(resume + (uintptr_t)(wide - plen));
        memcpy(p + jmp_at, &(int32_t){ (int32_t)(target - here) }, 4);
        /* the jmp at offset 15 is the LAST thing head[] does: it goes to base, which is
         * 24 so it clears head[]'s own rel32 field at 16..19. */
        here   = (intptr_t)(p + out_at + 1 + 4);
        target = (intptr_t)(p + base);
        memcpy(p + out_at + 1, &(int32_t){ (int32_t)(target - here) }, 4);
    }
    return p;
}

static void install_front_trace(void)
{
    /* (VA, name, prologue length).  THE LENGTH MUST BE A WHOLE NUMBER OF INSTRUCTIONS
     * AND AT LEAST SIX, because patch_text() always writes six bytes and the thunk
     * replays the copy and resumes at p+length.  Two ways to get that wrong, both of
     * which look like a game that blocks: a length of 5 leaves the sixth byte
     * overwritten by the trampoline's own `ret`, so the replayed prologue executes a
     * `ret` mid-function; and a length that stops mid-instruction (the first 5 bytes of
     * a 10-byte `mov eax,imm32 / call chkstk`) replays half an opcode.  Either produces
     * a run that installs its patches, logs a few lines, and then stops -- which is
     * indistinguishable from a hang unless you know to check this table first. */
    static const struct { unsigned va; const char *name; int plen; } T[] = {
        { 0x0041B891u, "front_state_set", 12 },  /* sub esp,104h / push ebx,esi,edi / mov ecx,ebx / push ebp */
        { 0x0041D31Fu, "state2_label",     7 },  /* sub esp,10h / lea eax,[esp] */
        { 0x0041D374u, "scanning_label",   7 },
        { 0x0041D3CCu, "worldlist_rows",   7 },  /* push ebp / mov eax,[esp+0Ch] / mov ebp,esp */
        { 0x0041D717u, "worldlist_init",   6 },  /* push ebp / mov eax,11C0h */
        { 0x00438E8Eu, "solo_stepper",     6 },  /* push ebp / mov eax,1FBCh */
        /* 0x0042AA10 is deliberately NOT traced here.  Its first ten bytes are
         * `mov eax,1F50h / call chkstk`, and `call rel32` is POSITION DEPENDENT: the
         * trampoline replays those bytes at a VirtualAlloc address, so the copy's
         * displacement lands somewhere else and the replayed prologue calls garbage.
         * The run then stops dead right after this entry, which reads exactly like
         * "0x46F blocks" and is how that conclusion was reached twice.  The same
         * applies to 0x0042B06A (`call FUN_00427d89` at +6).  0x46F's entry and RETURN
         * are traced instead in hook_SendMessageA, an IAT detour, which has no
         * relocation problem at all. */
        { 0x0046CE24u, "network_info",     7 },  /* push esi / mov esi,[esp+8] / test esi,esi */
        { 0x0042AF38u, "channel_close",   6 },  /* sub esp,FA0h */
    };
    unsigned i;
    for (i = 0; i < sizeof T / sizeof T[0] && i < 16; i++) {
        unsigned char *p = (unsigned char *)(g_base + (T[i].va - IMAGE_BASE));
        /* the thunk is built BEFORE patch_text, so the copy of the prologue it takes
         * is the original code.  patch_text then overwrites p[0..5] with
         * `push thunk; ret`, and the thunk replays p[0..plen-1] out of its own memory
         * and jumps to p+plen -- p+plen, not p+6+plen: the six overwritten bytes are
         * part of the copy, so resuming after them would skip six bytes of the
         * function and the run would diverge from the unpatched one. */
        void *th = front_make_thunk((int)i, (uintptr_t)(p + T[i].plen), p, T[i].plen);
        g_front_name[i] = T[i].name;
        if (!th || !patch_text((uintptr_t)p, th, 6)) tr("front: %s NOT traced", T[i].name);
    }
    /* the two 20 Hz entries are rate-limited to one line a second inside the logger;
     * every other traced function is low-frequency and is logged in full. */
    g_front_hot[8] = 1;   /* frame_20hz */
    g_front_hot[9] = 1;   /* per_frame  */
    tr("front: %u entry traces installed", (unsigned)(sizeof T / sizeof T[0]));
}

static void install_detours(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE u32 = GetModuleHandleA("user32.dll");
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    HMODULE self = GetModuleHandleA(NULL);

    if (self && (uintptr_t)self != IMAGE_BASE)
        g_base = (uintptr_t)self;

    real_PeekMessageA = (BOOL (WINAPI *)(LPMSG, HWND, UINT, UINT, UINT))GetProcAddress(u32, "PeekMessageA");
    real_GetMessageA  = (BOOL (WINAPI *)(LPMSG, HWND, UINT, UINT))GetProcAddress(u32, "GetMessageA");
    real_TranslateMessage  = (BOOL (WINAPI *)(const MSG *))GetProcAddress(u32, "TranslateMessage");
    real_DispatchMessageA  = (BOOL (WINAPI *)(const MSG *))GetProcAddress(u32, "DispatchMessageA");
    real_DispatchMessageW  = (BOOL (WINAPI *)(const MSG *))GetProcAddress(u32, "DispatchMessageW");
    real_IsDialogMessageA  = (BOOL (WINAPI *)(HWND, LPMSG))GetProcAddress(u32, "IsDialogMessageA");
    real_IsDialogMessageW  = (BOOL (WINAPI *)(HWND, LPMSG))GetProcAddress(u32, "IsDialogMessageW");
    real_EndDialog         = (BOOL (WINAPI *)(HWND, INT_PTR))GetProcAddress(u32, "EndDialog");
    real_DialogBoxParamA   = (int (WINAPI *)(HINSTANCE, UINT_PTR, HWND, DLGPROC, LPARAM))
                             GetProcAddress(u32, "DialogBoxParamA");
    real_DialogBoxParamW   = (int (WINAPI *)(HINSTANCE, UINT_PTR, HWND, DLGTPROCW, LPARAM))
                             GetProcAddress(u32, "DialogBoxParamW");
    real_DialogBoxIndirectParamA = (int (WINAPI *)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM))
                             GetProcAddress(u32, "DialogBoxIndirectParamA");
    real_DialogBoxIndirectParamW = (int (WINAPI *)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGTPROCW, LPARAM))
                             GetProcAddress(u32, "DialogBoxIndirectParamW");
    real_CreateDialogIndirectParamA = (HWND (WINAPI *)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM))
                             GetProcAddress(u32, "CreateDialogIndirectParamA");
    real_CreateDialogIndirectParamW = (HWND (WINAPI *)(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGTPROCW, LPARAM))
                             GetProcAddress(u32, "CreateDialogIndirectParamW");
    real_MessageBoxA = (int (WINAPI *)(HWND, LPCSTR, LPCSTR, UINT))GetProcAddress(u32, "MessageBoxA");
    real_MessageBoxW = (int (WINAPI *)(HWND, LPCWSTR, LPCWSTR, UINT))GetProcAddress(u32, "MessageBoxW");
    real_PostMessageA = (BOOL (WINAPI *)(HWND, UINT, WPARAM, LPARAM))GetProcAddress(u32, "PostMessageA");
    real_SendMessageA = (BOOL (WINAPI *)(HWND, UINT, WPARAM, LPARAM))GetProcAddress(u32, "SendMessageA");

    if (grp("pump")) {
        DETOUR("user32.dll", "PeekMessageA",         hook_PeekMessageA);
        DETOUR("user32.dll", "PeekMessageW",         hook_PeekMessageA);
        DETOUR("user32.dll", "GetMessageA",          hook_GetMessageA);
        DETOUR("user32.dll", "GetMessageW",          hook_GetMessageA);
        DETOUR("user32.dll", "PostMessageA",         hook_PostMessageA);
        DETOUR("user32.dll", "SendMessageA",         hook_SendMessageA);
    }
    real_SetCursorPos = GetProcAddress(u32, "SetCursorPos");
    if (real_SetCursorPos) DETOUR("user32.dll", "SetCursorPos", hook_SetCursorPos);

    /* The modal loop Wine keeps on the far side of its own imports. */
    if (grp("modal")) {
        DETOUR("user32.dll", "EndDialog",             hook_EndDialog);
        if (real_DialogBoxParamA)          DETOUR("user32.dll", "DialogBoxParamA", hook_DialogBoxParamA);
        if (real_DialogBoxIndirectParamA)  DETOUR("user32.dll", "DialogBoxIndirectParamA",
                                                  hook_DialogBoxIndirectParamA);
        if (real_DialogBoxParamW)          DETOUR("user32.dll", "DialogBoxParamW", hook_DialogBoxParamW);
        if (real_DialogBoxIndirectParamW)  DETOUR("user32.dll", "DialogBoxIndirectParamW",
                                                  hook_DialogBoxIndirectParamW);
        DETOUR("user32.dll", "MessageBoxA",           hook_MessageBoxA);
        DETOUR("user32.dll", "MessageBoxW",           hook_MessageBoxW);
    }

    if (grp("timer")) {
    DETOUR("user32.dll", "SetTimer",         hook_SetTimer);
        DETOUR("user32.dll", "KillTimer",        hook_KillTimer);
    }

    if (grp("focus")) {
        real_GetForegroundWindow = (HWND (WINAPI *)(void))(void *)GetProcAddress(
            GetModuleHandleA("user32.dll"), "GetForegroundWindow");
        if (real_GetForegroundWindow)
            DETOUR("user32.dll", "GetForegroundWindow", hook_GetForegroundWindow);
    }

    if (grp("key")) {
    DETOUR("user32.dll", "GetKeyState",      hook_GetKeyState);
        DETOUR("user32.dll", "GetAsyncKeyState", hook_GetAsyncKeyState);
    }

    if (grp("help")) {
    DETOUR("user32.dll", "WinHelpA",         hook_WinHelpA);
        DETOUR("user32.dll", "WinHelpW",         hook_WinHelpA);
    }


    if (grp("tick")) {
    DETOUR("kernel32.dll", "GetTickCount",   hook_GetTickCount);
        DETOUR("kernel32.dll", "GetTickCount64", hook_GetTickCount);
    }

    if (grp("wall")) {
    /* Sleep is VIRTUAL. The harness owns the clock, so a `Sleep(n)` that really slept
     * for n milliseconds of wall time would do two things at once: make the run's
     * duration depend on the host, and let the app sit in a wait the harness cannot
     * observe or advance. The original's worst offender is FUN_0046831C (0x46831C),
     * the MCI audio shutdown, which after `mciSendStringA("close song")` waits
     *
     *     while (DAT_004f416c != 0 && GetTickCount() - t0 < 3000) Sleep(100);
     *
     * Under Wine the MCI close FAILS (oracle.md 1.1), so DAT_004f416c is set and
     * nothing ever clears it, and the loop runs the full three seconds of REAL time.
     * Measured: a 3400 ms script took 84 s of wall clock and stopped at virtual 1740 ms,
     * parked in ntdll with the return address at Souls.exe 0x4683CF.
     *
     * Advancing the virtual clock by n and returning immediately is the same answer the
     * original gets on a machine where the MCI device closes properly (the loop exits
     * after one or two 100 ms sleeps), and it keeps the run deterministic: the number of
     * Sleep calls is fixed by the code path, so the clock advance is too. Sleep(0) is
     * left alone -- it is a yield, not a wait, and yielding is how the app lets the
     * message queue drain. */
    DETOUR("kernel32.dll", "Sleep",           hook_Sleep);

    DETOUR("kernel32.dll", "GetSystemTime",  hook_GetSystemTime);
        DETOUR("kernel32.dll", "GetLocalTime",   hook_GetLocalTime);
    }


    tr("install: modules k32=%p u32=%p crt=%p", (void *)k32, (void *)u32, (void *)crt);
    if (grp("probe")) {
        real_CreateWindowExA = (HWND (WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                                HWND, HMENU, HINSTANCE, LPVOID))
                               GetProcAddress(u32, "CreateWindowExA");
        if (real_CreateWindowExA) DETOUR("user32.dll", "CreateWindowExA", hook_CreateWindowExA);
    }
    if (grp("trace")) {
        real__access = (__cdecl int (*)(const char *, int))GetProcAddress(crt, "_access");
        real_fopen    = (FILE *(__cdecl *)(const char *, const char *))GetProcAddress(crt, "fopen");
        real_CreateFileA = (HANDLE (WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                              DWORD, DWORD, HANDLE))GetProcAddress(k32, "CreateFileA");
        tr("trace group: access=%p fopen=%p createfile=%p", (void *)real__access,
           (void *)real_fopen, (void *)real_CreateFileA);
        g_trace_on = 1;
        if (real__access)    DETOUR("msvcrt.dll", "_access",     hook__access);
        if (real_fopen)       DETOUR("msvcrt.dll", "fopen",       hook_fopen);
        if (real_CreateFileA) DETOUR("kernel32.dll", "CreateFileA", hook_CreateFileA);
    }
    if (grp("rng")) {
    DETOUR("msvcrt.dll", "rand",          hook_rand);
        DETOUR("msvcrt.dll", "_rand",         hook_rand);
        DETOUR("msvcrt.dll", "srand",         hook_srand);
        DETOUR("msvcrt.dll", "_srand",        hook_srand);
    }

    if (grp("clock")) {
    DETOUR("msvcrt.dll", "time",          hook_time);
        DETOUR("msvcrt.dll", "_time64",       hook__time64);
        DETOUR("msvcrt.dll", "localtime",     hook_localtime);
        DETOUR("msvcrt.dll", "_localtime64",  hook_localtime);
        DETOUR("msvcrt.dll", "ctime",         hook_ctime);
        DETOUR("msvcrt.dll", "_ctime64",      hook_ctime);
        DETOUR("msvcrt.dll", "mktime",        hook_mktime);
        DETOUR("msvcrt.dll", "_mktime64",     hook_mktime);
    }

    if (grp("front")) install_front_trace();

    tr("install: %d detours installed; base=%08lX realPeek=%p realGet=%p",
       g_detoured, (unsigned long)g_base, (void *)real_PeekMessageA, (void *)real_GetMessageA);
    log_modules();
    if (grp("watch")) start_watchdog();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (GetEnvironmentVariableA("WOS_LOG", g_logpath, sizeof g_logpath)) {
            g_logh = CreateFileA(g_logpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_logh != INVALID_HANDLE_VALUE) {
                SetFilePointer(g_logh, 0, NULL, FILE_END);
                g_logon = 1;
            }
        }
        tr("DllMain: attaching, log=%s, self=%p exe=%p, stack=%p..%p", g_logon ? g_logpath : "<off>",
           (void *)h, (void *)GetModuleHandleA(NULL),
           (void *)((uintptr_t)&h), (void *)((uintptr_t)&h + 0x10000));
        /* WOS_RANDTRACE is read HERE, not in script_init: the game burns ~1400
         * rands during boot, long before the first pump step, and a trace armed
         * at script_init misses every one of them.  Tracing costs an fopen per
         * call, so it stays off unless the variable is set. */
        g_rand_trace = GetEnvironmentVariableA("WOS_RANDTRACE", g_scratch, sizeof g_scratch) != 0;
        if (g_rand_trace) trace_open();
        if (GetEnvironmentVariableA("WOS_ENC_TRACE", g_scratch, sizeof g_scratch)) {
            g_enc_trace = 1;              /* install_enc_trace also sets it; the write here
                                         * covers the rands that happen before DllMain
                                         * returns, which is all 1408 of the boot ones */
            install_enc_trace();
        }
        install_detours();
    }
    return 1;
}
