/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_split_proc.c - the second game process of a split-screen game: start it,
 * watch it, stop it.
 *
 * This file includes <windows.h>, whose RECT16 / `byte` / EnterCriticalSection
 * cannot coexist with the PSX headers, so it includes nothing from the game and
 * speaks only plain C types (the same wall as sh_net_platform.c).
 */
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>

/* Starting Steam puts its app id into this process's environment (SteamAppId,
 * SteamGameId), and a copy that inherits it is taken by Steam for the same
 * Steam Input game: Steam hides the controllers from it, Player 2's pad
 * included, and that copy cannot read them through Steam either. So Player 2
 * starts without Steam's variables, and without the controller lists Steam
 * gives the games it launches. */
static int Proc_IsSteamVar(const wchar_t* e)
{
    static const wchar_t* const prefix[] = {
        L"Steam", L"STEAM_", L"SDL_GAMECONTROLLER", L"EnableConfiguratorSupport", L"ENABLE_VK_LAYER_VALVE_steam"
    };
    size_t i;

    for (i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
        if (_wcsnicmp(e, prefix[i], wcslen(prefix[i])) == 0)
            return 1;
    return 0;
}

/* This process's environment block without those; free() it. NULL = could
 * not be made, inherit it as it is. */
static wchar_t* Proc_EnvWithoutSteam(void)
{
    wchar_t*       src = GetEnvironmentStringsW();
    const wchar_t* p;
    size_t         total = 0;
    wchar_t*       out;
    wchar_t*       w;

    if (!src)
        return NULL;
    for (p = src; *p; p += wcslen(p) + 1)
        total += wcslen(p) + 1;
    out = (wchar_t*)malloc((total + 2) * sizeof(wchar_t));
    if (!out)
    {
        FreeEnvironmentStringsW(src);
        return NULL;
    }
    w = out;
    for (p = src; *p; p += wcslen(p) + 1)
    {
        const size_t len = wcslen(p) + 1;
        if (Proc_IsSteamVar(p))
            continue;
        memcpy(w, p, len * sizeof(wchar_t));
        w += len;
    }
    w[0] = L'\0';
    w[1] = L'\0';
    FreeEnvironmentStringsW(src);
    return out;
}

void* PcProc_Spawn(const char* cmdline)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char*               buf;
    size_t              n;
    wchar_t*            env;
    BOOL                ok;

    if (!cmdline || !cmdline[0])
        return NULL;
    /* CreateProcess may write into the command line. */
    n   = strlen(cmdline) + 1;
    buf = (char*)malloc(n);
    if (!buf)
        return NULL;
    memcpy(buf, cmdline, n);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* CREATE_NO_WINDOW: the game is a console program, and the second copy
     * would otherwise flash a console over the first one's window. */
    env = Proc_EnvWithoutSteam();
    ok  = CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                         env, NULL, &si, &pi);
    free(env);
    free(buf);
    if (!ok)
        return NULL;
    CloseHandle(pi.hThread);
    return (void*)pi.hProcess;
}

int PcProc_Alive(void* h)
{
    return h != NULL && WaitForSingleObject((HANDLE)h, 0) == WAIT_TIMEOUT;
}

void PcProc_Kill(void* h)
{
    if (!h)
        return;
    if (WaitForSingleObject((HANDLE)h, 0) == WAIT_TIMEOUT)
        TerminateProcess((HANDLE)h, 0);
    CloseHandle((HANDLE)h);
}

unsigned long PcProc_SelfPid(void)
{
    return (unsigned long)GetCurrentProcessId();
}

int PcProc_PidAlive(unsigned long pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    int    alive;

    if (!h)
        return 0;
    alive = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
    CloseHandle(h);
    return alive;
}

int PcProc_ExePath(char* out, int cap)
{
    DWORD n;

    if (!out || cap <= 0)
        return 0;
    n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    return n > 0 && n < (DWORD)cap;
}

#else /* not Windows: split screen needs the launcher of a second window */

void*         PcProc_Spawn(const char* cmdline)   { (void)cmdline; return NULL; }
int           PcProc_Alive(void* h)               { (void)h; return 0; }
void          PcProc_Kill(void* h)                { (void)h; }
unsigned long PcProc_SelfPid(void)                { return 0; }
int           PcProc_PidAlive(unsigned long pid)  { (void)pid; return 0; }
int           PcProc_ExePath(char* out, int cap)  { (void)out; (void)cap; return 0; }

#endif
