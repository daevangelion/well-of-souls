/* tools/oracle/hook/launcher.c -- start Souls.exe suspended, inject the hook, resume.
 *
 *   launcher.exe <target.exe> <hook.dll> [-- <args to target>...]
 *
 * Runs inside the same Wine prefix as the game, so it is a 32-bit PE and can use the
 * ordinary Win32 injection API.  No file on disk is touched: only the new process.
 * The target is created CREATE_SUSPENDED, which means the loader has already mapped
 * every module and resolved every import by the time we get control, so the hook's
 * DllMain can detour kernel32/user32/msvcrt safely and is fully armed before the
 * target's entry point runs a single instruction.
 *
 * After the target is resumed we wait up to --timeout seconds (default 120).  If the
 * script ends the harness posts WM_QUIT, and the game's ExitInstance would otherwise
 * open the help file, so a hard kill is the normal end of a run; the exit code we
 * report is 0 when the process finished inside the grace period and 42 when we had
 * to terminate it.  run.sh treats both as success.
 */

#include <windows.h>
#include <stdio.h>

#define GRACE_MS 4000

static int fail(const char *what)
{
    fprintf(stderr, "launcher: %s (win32 error %lu)\n", what, (unsigned long)GetLastError());
    return 2;
}

static BOOL inject(HANDLE proc, const char *dllpath)
{
    size_t n = strlen(dllpath) + 1;
    void *remote;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadlib;
    HANDLE th;
    DWORD id = 0;
    if (!k32) return FALSE;
    loadlib = GetProcAddress(k32, "LoadLibraryA");
    if (!loadlib) return FALSE;
    remote = (void *)VirtualAllocEx(proc, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return FALSE;
    if (!WriteProcessMemory(proc, remote, dllpath, n, NULL)) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return FALSE; }
    th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)loadlib, remote, 0, &id);
    if (!th) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return FALSE; }
    WaitForSingleObject(th, 15000);
    GetExitCodeThread(th, &id);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return id != 0;                        /* the LoadLibraryA return value */
}

int main(int argc, char **argv)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[2048];
    int i, targ = 0, dll = 0, tmo = 120, waited = 0;
    DWORD rc = 42;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) tmo = atoi(argv[++i]);
        else if (!targ) targ = i;
        else if (!dll) dll = i;
    }
    if (!targ || !dll) {
        fprintf(stderr, "usage: launcher.exe <target.exe> <hook.dll> [--timeout S] [target args...]\n");
        return 2;
    }

    /* target command line: argv[targ] plus everything after the hook dll, minus the launcher's
     * own --timeout pair. Passing it through made MFC's ParseCommandLine treat the number as a
     * document to open ("<N> was not found") and left the app off its message loop. */
    cmd[0] = 0;
    strncat(cmd, argv[targ] + (strrchr(argv[targ], '\\') ? (size_t)(strrchr(argv[targ], '\\') - argv[targ] + 1) : 0), sizeof cmd - strlen(cmd) - 1);
    for (i = dll + 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) continue;
        if (!strcmp(argv[i], "--timeout")) { ++i; continue; }
        strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(argv[targ], cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        return fail("CreateProcess");
    if (!inject(pi.hProcess, argv[dll])) {
        TerminateProcess(pi.hProcess, 3);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return fail("inject");
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    /* wait, polling so a hung process can be killed at the timeout */
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &rc);
            break;
        }
        if (waited >= tmo * 1000) { TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, GRACE_MS);
                                    GetExitCodeProcess(pi.hProcess, &rc); break; }
        waited += 200;
    }
    CloseHandle(pi.hProcess);
    printf("launcher: target finished, exit=%lu\n", (unsigned long)rc);
    return 0;
}
