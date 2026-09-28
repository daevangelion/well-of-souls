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
    char line[192];
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
            if (++got == 4) break;
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
                if (++got == 4) break;
            }
        }
        trace_line("%d %08X%s", (int)g_rand_calls, (unsigned)(uintptr_t)ra, chain);
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

/* ------------------------------------------------------------------- the input */

static HWND deepest_at(POINT screen)
{
    HWND h = (HWND)WindowFromPoint(screen);
    while (h) {
        POINT p = screen;
        RECT c;
        ScreenToClient(h, &p);
        GetClientRect(h, &c);
        if (PtInRect(&c, p)) {
            HWND pick = h, ch = (HWND)GetWindow(h, GW_CHILD);
            while (ch) {
                POINT q = screen;
                RECT rc;
                ScreenToClient(ch, &q);
                GetClientRect(ch, &rc);
                if (PtInRect(&rc, q)) { pick = ch; ch = (HWND)GetWindow(ch, GW_CHILD); }
                else                  ch = (HWND)GetWindow(ch, GW_HWNDNEXT);
            }
            return pick;
        }
        h = (HWND)GetParent(h);
    }
    return NULL;
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
    if (pid != GetCurrentProcessId()) return TRUE;
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
static int label_is(const char *l, const char *want) { return strcmp(l, want) == 0; }
static int label_module(const char *l)
{
    static const char *names[] = { "clock","rng","hero","map","scene","battle",
                                   "panels","items","minigame","options", NULL };
    int i;
    for (i = 0; names[i]; i++) if (label_is(l, names[i])) return 1;
    return 0;
}
static void do_dump(const char *label)
{
    char path[700];
    FILE *f;
    unsigned char *hero;
    uintptr_t b = g_base;
    int i, live, is_hero = label_is(label, "hero");
    wsprintfA(path, "%s\\%s.txt", g_outdir, label);
    f = fopen(path, "wb");
    if (!f) { tr("oracle: cannot write %s", path); return; }
    hero = (unsigned char *)(b + (0x0067FBF8u - IMAGE_BASE));
    fprintf(f, "label=%s\n", label);

    /* --- the harness keys, in the port's spelling -------------------------
     * Only for the labels the port answers with them: `dump <label>` maps to
     * exactly one module on both sides, and putting clock.* in a `dump hero`
     * would make every hero label a mismatch over a key that is not the
     * hero's. */
    if (label_is(label, "clock") || label_is(label, "rng")) {
        fprintf(f, "clock.ms=%u\n", vnow());
        fprintf(f, "clock.time_s=%u\n", vtime_s());
        fprintf(f, "rng.state=%u\n", g_holdrand);
        fprintf(f, "rng.calls=%d\n", (int)g_rand_calls);
    }

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
hero_done: ;
    }

    /* --- oracle-only: everything the port has no key for ------------------ */
    fprintf(f, "oracle.seq=%d\n", g_dump_seq++);
    fprintf(f, "oracle.gate_stamp=%u\n",
            *(volatile DWORD *)(b + (VA_GATE_STAMP - IMAGE_BASE)));
    fprintf(f, "oracle.steps=%ld\n", (long)g_steps);
    fprintf(f, "oracle.timers_live=%d\n", (live = timer_live_count(), live));
    fprintf(f, "oracle.next_deadline=%u\n", timer_next_deadline());
    fprintf(f, "oracle.srand_calls=%d\n", (int)g_srand_calls);
    fprintf(f, "oracle.front_state=%d\n", *(int *)(b + (0x004DF8A4u - IMAGE_BASE)));
    fprintf(f, "oracle.front_serial=%d\n", *(int *)(b + (0x004DD20Cu - IMAGE_BASE)));
    fprintf(f, "oracle.front_network=%d\n", *(int *)(b + (0x004E6910u - IMAGE_BASE)));
    fprintf(f, "oracle.front_map=%d\n", *(int *)(b + (0x004E0DDCu - IMAGE_BASE)));
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
    if (!label_module(label)) fprintf(f, "oracle.unknown_label=1\n");
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
    if (t_idle <= now) t_idle = now + IDLE_QUANTUM;   /* must be strictly forward,
                                                           or the clock livelocks */

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

    if (t_in == 0xFFFFFFFFu && t_tim == 0xFFFFFFFFu) {
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
        case EV_DUMP: do_dump(e->text ? e->text : "dump"); break;
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
static BOOL WINAPI hook_SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_msglog)
        tr("post now=%u ra=%08lX Send id=%04X hwnd=%p w=%08lX l=%08lX",
           vnow(), (unsigned long)(uintptr_t)__builtin_return_address(0), msg, (void *)h,
           (unsigned long)w, (unsigned long)l);
    return real_SendMessageA(h, msg, w, l);
}


static volatile LONG g_peeks;
static BOOL WINAPI hook_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT rm)
{
    BOOL r;
    if (InterlockedIncrement(&g_peeks) < 8)
        tr("hook_PeekMessageA: call %ld rm=%08X", (long)g_peeks, rm);
    pump_step();
    r = real_PeekMessageA(m, h, a, b, rm);
    if (!r) InterlockedIncrement(&g_idle_pass);
    if (r) msglog_win(m->hwnd);
    msglog(m, "Peek", rm, r);
    return r;
}

static volatile LONG g_gets;
static BOOL WINAPI hook_GetMessageA(LPMSG m, HWND h, UINT a, UINT b)
{
    BOOL r;
    InterlockedIncrement(&g_gets);
    tr("hook_GetMessageA: call %ld", (long)g_gets);
    if (!pump_step()) {
        MSG probe;
        memset(&probe, 0, sizeof probe);
        if (!real_PeekMessageA(&probe, NULL, 0, 0, PM_NOREMOVE))
            PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0);  /* never block */
    }
    r = real_GetMessageA(m, h, a, b);
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
static DWORD WINAPI watchdog_thread(LPVOID unused)
{
    (void)unused;
    /* Counters only.  EnumWindows from this thread is what the first version
     * did, and it never came back: the main thread is parked inside a user32
     * wait while the watchdog is asking user32 for the window list.  A
     * diagnostic that can deadlock the thing it measures is not a diagnostic. */
    Sleep(4000);                       /* let DllMain return and the game start */
    for (;;) {
        Sleep(3000);
        tr("watch: steps=%ld peeks=%ld gets=%ld modal=%d ready=%ld finished=%d "
           "rng=%ld ticks=%u next_in=%d",
           (long)g_steps, (long)g_peeks, (long)g_gets, g_modal_depth,
           (long)g_ready, g_finished, (long)g_rand_calls, vnow(), pump_next_event());
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
 *   core  (default) pump, timer, key, tick, wall, clock, rng, help, modal
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
    char core[] = "pump,timer,key,tick,wall,clock,rng,help,modal,watch";
    char buf[256];
    DWORD n = GetEnvironmentVariableA("WOS_DETOURS", buf, sizeof buf);
    if (!n) return in_list(core, name);
    if (!buf[0]) return 0;
    return in_list(buf, name) || in_list(buf, "all");
}

static BOOL (WINAPI *real_SetCursorPos)(int, int);
static BOOL WINAPI hook_SetCursorPos(int x, int y)
{
    if (g_msglog)
        tr("post now=%u ra=%08lX SetCursorPos %d,%d", vnow(),
           (unsigned long)(uintptr_t)__builtin_return_address(0), x, y);
    return real_SetCursorPos(x, y);
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
