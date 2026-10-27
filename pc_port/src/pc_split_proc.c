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

void* PcProc_Spawn(const char* cmdline)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char*               buf;
    size_t              n;

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
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        free(buf);
        return NULL;
    }
    free(buf);
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
