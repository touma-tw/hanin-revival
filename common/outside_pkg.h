/*
 * outside_pkg.h - keep Hanin processes out of MSIX "desktop bridge" containers.
 *
 * A process started by a packaged desktop app (Claude desktop, some Store apps,
 * ...) inherits that package's identity, and Windows silently redirects its
 * writes under %LOCALAPPDATA% to %LOCALAPPDATA%\Packages\<pkg>\LocalCache\Local.
 * The engine host and the toolbox must always see the real user profile, so
 * when they find themselves packaged they relaunch with Explorer as parent.
 *
 * Also provides a one-time rescue of data that was already written into such
 * a virtualised copy.
 */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

static BOOL hanin_has_package_identity(void)
{
    typedef LONG(WINAPI * PFN)(UINT32 *, PWSTR);
    PFN f = (PFN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetCurrentPackageFullName");
    UINT32 len = 0;
    if (!f) return FALSE;
    return f(&len, NULL) != APPMODEL_ERROR_NO_PACKAGE;
}

/* Behavioural test: are our writes to %LOCALAPPDATA% being redirected into some
 * app's private Packages\<pkg>\LocalCache\Local folder? This catches package
 * identity as well as containers that virtualise the file system without one
 * (observed with processes started from the Claude desktop app). */
static BOOL hanin_in_package(void)
{
    char la[MAX_PATH], probe[MAX_PATH], name[64], pat[MAX_PATH], cand[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f, h;
    BOOL redirected = FALSE;
    if (hanin_has_package_identity()) return TRUE;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) return FALSE;
    _snprintf(name, sizeof(name), "hanin-vprobe-%lu-%lu.tmp", GetCurrentProcessId(), GetTickCount());
    _snprintf(probe, MAX_PATH, "%s\\%s", la, name);
    f = CreateFileA(probe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    _snprintf(pat, MAX_PATH, "%s\\Packages\\*", la);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            _snprintf(cand, MAX_PATH, "%s\\Packages\\%s\\LocalCache\\Local\\%s", la, fd.cFileName, name);
            if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) { redirected = TRUE; break; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CloseHandle(f);
    return redirected;
}

/* Re-run this exe with the same command line, parented to Explorer (which is
 * never packaged). Returns TRUE if the new process was created. */
static BOOL hanin_relaunch_outside_package(const char *workDir)
{
    HWND shell = GetShellWindow();
    DWORD pid = 0;
    HANDLE parent;
    SIZE_T sz = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs;
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi;
    char exe[MAX_PATH], *cmd;
    BOOL ok;

    if (!shell) return FALSE;
    GetWindowThreadProcessId(shell, &pid);
    parent = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, pid);
    if (!parent) return FALSE;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, sz);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &sz)) { CloseHandle(parent); return FALSE; }
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), NULL, NULL);

    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    cmd = _strdup(GetCommandLineA());
    ok = CreateProcessA(exe, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, workDir, &si.StartupInfo, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    free(cmd);
    DeleteProcThreadAttributeList(attrs);
    HeapFree(GetProcessHeap(), 0, attrs);
    CloseHandle(parent);
    return ok;
}

/* Find the most recently written virtualised copy of HaninRevival\data under
 * %LOCALAPPDATA%\Packages\*\LocalCache\Local. Returns TRUE and fills `out`. */
static BOOL hanin_find_virtualized_data(char *out, size_t cch)
{
    char la[MAX_PATH], pat[MAX_PATH], probe[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    FILETIME best = { 0, 0 };
    BOOL found = FALSE;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) return FALSE;
    _snprintf(pat, MAX_PATH, "%s\\Packages\\*", la);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        _snprintf(probe, MAX_PATH, "%s\\Packages\\%s\\LocalCache\\Local\\HaninRevival\\data\\whanin.ini", la, fd.cFileName);
        if (GetFileAttributesExA(probe, GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &best) > 0) {
            best = a.ftLastWriteTime;
            _snprintf(out, cch, "%s\\Packages\\%s\\LocalCache\\Local\\HaninRevival\\data", la, fd.cFileName);
            found = TRUE;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}
