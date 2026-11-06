/*
 * hanin_tools.c - launcher that runs the original 漢音工具箱 (Tools95.exe, 2000)
 * on modern Windows.
 *
 * Tools95.exe has its relocations stripped and must live at 0x400000, so this
 * launcher is linked elsewhere (/BASE:0x10000000), maps Tools95 into its own
 * process at its preferred base, fills the import table itself (redirecting the
 * hard-coded C:\HANIN paths and the Windows-directory whanin.ini to the user's
 * data directory) and jumps to its entry point.
 *
 * Live setting changes reach the engine the original way: Tools95 finds the
 * IME UI window (class "Hanin95", owned by hanin_host.exe) and SendMessage()s
 * it. When the toolbox exits after writing anything, the engine host is
 * restarted so dictionaries and the ini are reloaded from scratch.
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <share.h>
#include <psapi.h>
#include <stdlib.h>
#include "../common/outside_pkg.h"

static FILE   *g_log;
static char    g_dataDir[MAX_PATH], g_exeDir[MAX_PATH], g_toolsPath[MAX_PATH];
static HMODULE g_tools;
static int     g_isChild;
static char    g_app[32];            /* "" = Tools95, "query" = QUERY.exe        */
static char    g_userArgs[512];      /* arguments meant for the original program */
static char    g_targetCmd[MAX_PATH + 520];

/* split our command line into launcher flags (--child, --app X) and the rest */
static void parse_cmdline(void)
{
    const char *p = GetCommandLineA();
    char tok[256];
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ') p++;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        if (!*p) break;
        while (*p && *p != ' ' && n < 255) tok[n++] = *p++;
        tok[n] = 0;
        if (!strcmp(tok, "--child")) g_isChild = 1;
        else if (!strcmp(tok, "--app")) {
            int m = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && m < 31) g_app[m++] = *p++;
            g_app[m] = 0;
        } else {
            if (g_userArgs[0]) strcat(g_userArgs, " ");
            if (strlen(g_userArgs) + n < sizeof(g_userArgs) - 1) strcat(g_userArgs, tok);
        }
    }
}
static int     g_wrote;

static void logf(const char *fmt, ...)
{
    char buf[1024];
    SYSTEMTIME st;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (!g_log) return;
    GetLocalTime(&st);
    fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_log);
}

static const char *b5(const char *s)
{
    static char out[4][1024];
    static int slot;
    WCHAR w[512];
    char *o = out[slot++ & 3];
    int n;
    if (!s) return "(null)";
    n = MultiByteToWideChar(950, 0, s, -1, w, 511);
    n = WideCharToMultiByte(CP_UTF8, 0, w, n, o, 1023, NULL, NULL);
    o[n > 0 ? n - 1 : 0] = 0;
    return o;
}

/* ---- path redirection: C:\HANIN\... and bare names -> data dir ---- */
static const char *redirect(const char *path, char *buf)
{
    static const char pre[] = "C:\\HANIN";
    if (!path || !HIWORD(path)) return path;
    if (_strnicmp(path, pre, sizeof(pre) - 1) == 0 && (path[8] == 0 || path[8] == '\\')) {
        _snprintf(buf, MAX_PATH, "%s%s", g_dataDir, path + 8);
        return buf;
    }
    if (!strchr(path, '\\') && !strchr(path, '/') && !strchr(path, ':')) {
        _snprintf(buf, MAX_PATH, "%s\\%s", g_dataDir, path);
        return buf;
    }
    return path;
}

UINT WINAPI H_GetWindowsDirectoryA(LPSTR buf, UINT size)
{
    UINT n = (UINT)strlen(g_dataDir);
    if (size > n) strcpy(buf, g_dataDir);
    return n;
}
HFILE WINAPI H_OpenFile(LPCSTR name, LPOFSTRUCT of, UINT style)
{
    char b[MAX_PATH]; const char *p = redirect(name, b);
    HFILE r = OpenFile(p, of, style);
    if (style & (OF_WRITE | OF_READWRITE | OF_CREATE | OF_DELETE)) g_wrote = 1;
    logf("OpenFile(%s -> %s, 0x%x) = %d", b5(name), b5(p), style, r);
    return r;
}
HFILE WINAPI H__lopen(LPCSTR name, int mode)
{
    char b[MAX_PATH]; const char *p = redirect(name, b);
    HFILE r = _lopen(p, mode);
    if (mode & (OF_WRITE | OF_READWRITE)) g_wrote = 1;
    logf("_lopen(%s -> %s, %d) = %d", b5(name), b5(p), mode, r);
    return r;
}
HFILE WINAPI H__lcreat(LPCSTR name, int attr)
{
    char b[MAX_PATH]; const char *p = redirect(name, b);
    HFILE r = _lcreat(p, attr);
    g_wrote = 1;
    logf("_lcreat(%s -> %s) = %d", b5(name), b5(p), r);
    return r;
}
BOOL WINAPI H_DeleteFileA(LPCSTR name)
{
    char b[MAX_PATH]; const char *p = redirect(name, b);
    g_wrote = 1;
    logf("DeleteFileA(%s -> %s)", b5(name), b5(p));
    return DeleteFileA(p);
}
HANDLE WINAPI H_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd)
{
    char b[MAX_PATH]; const char *p = redirect(name, b);
    HANDLE r = FindFirstFileA(p, fd);
    logf("FindFirstFileA(%s -> %s) = %p", b5(name), b5(p), r);
    return r;
}
BOOL WINAPI H_GetDiskFreeSpaceA(LPCSTR root, LPDWORD a, LPDWORD b, LPDWORD c, LPDWORD d)
{
    char buf[MAX_PATH]; const char *p = redirect(root, buf);
    char drive[4] = "C:\\";
    if (p && p[1] == ':') { drive[0] = p[0]; p = drive; }
    return GetDiskFreeSpaceA(p, a, b, c, d);
}
DWORD WINAPI H_GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file)
{
    char b[MAX_PATH]; const char *p = redirect(file, b);
    return GetPrivateProfileStringA(app, key, def, out, n, p);
}
UINT WINAPI H_GetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file)
{
    char b[MAX_PATH]; const char *p = redirect(file, b);
    return GetPrivateProfileIntA(app, key, def, p);
}
BOOL WINAPI H_WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file)
{
    char b[MAX_PATH]; const char *p = redirect(file, b);
    g_wrote = 1;
    logf("WritePrivateProfileString [%s] %s = \"%s\" (%s)", app ? app : "", key ? key : "", val ? b5(val) : "(null)", b5(p));
    return WritePrivateProfileStringA(app, key, val, p);
}
HMODULE WINAPI H_GetModuleHandleA(LPCSTR name)
{
    if (!name) return g_tools;
    return GetModuleHandleA(name);
}
DWORD WINAPI H_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n)
{
    if (!m || m == g_tools) {
        DWORD len = (DWORD)strlen(g_toolsPath);
        if (n) { strncpy(buf, g_toolsPath, n); buf[n - 1] = 0; }
        return len < n ? len : n;
    }
    return GetModuleFileNameA(m, buf, n);
}
LPSTR WINAPI H_GetCommandLineA(void) { return g_targetCmd; }

HWND WINAPI H_FindWindowA(LPCSTR cls, LPCSTR title)
{
    HWND r = FindWindowA(cls, title);
    logf("FindWindowA(%s, %s) = %p", cls && HIWORD(cls) ? cls : "(atom)", title ? b5(title) : "(null)", r);
    return r;
}
LRESULT WINAPI H_SendMessageA(HWND h, UINT m, WPARAM w, LPARAM l)
{
    char c[64] = "";
    if (m >= WM_USER) {
        GetClassNameA(h, c, 64);
        if (!strcmp(c, "Hanin95")) logf("-> engine SendMessage(0x%04x, 0x%x, 0x%x)", m, (unsigned)w, (unsigned)l);
    }
    return SendMessageA(h, m, w, l);
}
BOOL WINAPI H_PostMessageA(HWND h, UINT m, WPARAM w, LPARAM l)
{
    char c[64] = "";
    if (m >= WM_USER) {
        GetClassNameA(h, c, 64);
        if (!strcmp(c, "Hanin95")) logf("-> engine PostMessage(0x%04x, 0x%x, 0x%x)", m, (unsigned)w, (unsigned)l);
    }
    return PostMessageA(h, m, w, l);
}

/* restart the engine host so it reloads ini + dictionaries */
static void restart_engine(void)
{
    DWORD mySess = 0, sess;
    HANDLE snap;
    PROCESSENTRY32 pe;
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySess);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    pe.dwSize = sizeof(pe);
    if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &pe)) {
        do {
            if (!_stricmp(pe.szExeFile, "hanin_host.exe") && ProcessIdToSessionId(pe.th32ProcessID, &sess) && sess == mySess) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); WaitForSingleObject(h, 2000); CloseHandle(h); logf("engine host %lu stopped", pe.th32ProcessID); }
            }
        } while (Process32Next(snap, &pe));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    _snprintf(exe, MAX_PATH, "%s\\hanin_host.exe", g_exeDir);
    _snprintf(cmd, sizeof(cmd), "\"%s\" --server", exe);
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_exeDir, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        logf("engine host restarted (pid %lu)", pi.dwProcessId);
    } else logf("engine host restart failed (%lu); the IME will start it on the next key", GetLastError());
}

VOID WINAPI H_ExitProcess(UINT code)
{
    logf("Tools95 exit (code %u, wrote=%d)", code, g_wrote);
    if (g_wrote) restart_engine();
    if (g_log) fclose(g_log);
    ExitProcess(code);
}

typedef struct { const char *dll, *name; void *fn; } HOOK;
static const HOOK g_hooks[] = {
    { "KERNEL32.dll", "GetWindowsDirectoryA", H_GetWindowsDirectoryA },
    { "KERNEL32.dll", "OpenFile", H_OpenFile },
    { "KERNEL32.dll", "_lopen", H__lopen },
    { "KERNEL32.dll", "_lcreat", H__lcreat },
    { "KERNEL32.dll", "DeleteFileA", H_DeleteFileA },
    { "KERNEL32.dll", "FindFirstFileA", H_FindFirstFileA },
    { "KERNEL32.dll", "GetDiskFreeSpaceA", H_GetDiskFreeSpaceA },
    { "KERNEL32.dll", "GetPrivateProfileStringA", H_GetPrivateProfileStringA },
    { "KERNEL32.dll", "GetPrivateProfileIntA", H_GetPrivateProfileIntA },
    { "KERNEL32.dll", "WritePrivateProfileStringA", H_WritePrivateProfileStringA },
    { "KERNEL32.dll", "GetModuleHandleA", H_GetModuleHandleA },
    { "KERNEL32.dll", "GetModuleFileNameA", H_GetModuleFileNameA },
    { "KERNEL32.dll", "ExitProcess", H_ExitProcess },
    { "KERNEL32.dll", "GetCommandLineA", H_GetCommandLineA },
    { "USER32.dll", "FindWindowA", H_FindWindowA },
    { "USER32.dll", "SendMessageA", H_SendMessageA },
    { "USER32.dll", "PostMessageA", H_PostMessageA },
};

static BOOL resolve_imports(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    int hooked = 0, real = 0;
    for (; imp->Name; imp++) {
        const char *dll = (const char *)(base + imp->Name);
        HMODULE h = LoadLibraryA(dll);
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        DWORD old;
        if (!h) { logf("!! cannot load %s", dll); return FALSE; }
        for (; names->u1.AddressOfData; names++, iat++) {
            void *fn = NULL;
            const char *fname;
            int i;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                fn = GetProcAddress(h, (LPCSTR)IMAGE_ORDINAL(names->u1.Ordinal));
                fname = "(ordinal)";
            } else {
                fname = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
                for (i = 0; i < (int)(sizeof(g_hooks) / sizeof(g_hooks[0])); i++)
                    if (!_stricmp(g_hooks[i].dll, dll) && !strcmp(g_hooks[i].name, fname)) { fn = g_hooks[i].fn; hooked++; break; }
                if (!fn) { fn = GetProcAddress(h, fname); if (fn) real++; }
            }
            if (!fn) { logf("!! unresolved %s!%s", dll, fname); return FALSE; }
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD_PTR)fn;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
        }
    }
    logf("imports resolved: %d hooked, %d passthrough", hooked, real);
    return TRUE;
}

/* Map a relocation-stripped EXE at its fixed ImageBase by hand. */
static HMODULE map_fixed(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got, i;
    BYTE *file, *base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sh;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    file = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    ReadFile(f, file, size, &got, NULL);
    CloseHandle(f);
    nt = (IMAGE_NT_HEADERS *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    base = (BYTE *)VirtualAlloc((void *)(DWORD_PTR)nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage,
                                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base) /* reserved for us by the parent launcher: commit only */
        base = (BYTE *)VirtualAlloc((void *)(DWORD_PTR)nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage,
                                    MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base) {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void *)(DWORD_PTR)nt->OptionalHeader.ImageBase, &mbi, sizeof(mbi));
        logf("!! VirtualAlloc at 0x%08lx failed (%lu); region state=0x%lx alloc=%p type=0x%lx",
             nt->OptionalHeader.ImageBase, GetLastError(), mbi.State, mbi.AllocationBase, mbi.Type);
        return NULL;
    }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    sh = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sh[i].SizeOfRawData)
            memcpy(base + sh[i].VirtualAddress, file + sh[i].PointerToRawData,
                   min(sh[i].SizeOfRawData, sh[i].Misc.VirtualSize ? max(sh[i].Misc.VirtualSize, sh[i].SizeOfRawData) : sh[i].SizeOfRawData));
    HeapFree(GetProcessHeap(), 0, file);
    return (HMODULE)base;
}

static void fail(const char *msg)
{
    wchar_t w[600];
    DWORD err = GetLastError();
    logf("!! %s (err %lu)", msg, err);
    _snwprintf(w, 600, L"漢音工具箱無法啟動。\n\n原因:%S\n錯誤碼:%lu\n\nlog:%%LOCALAPPDATA%%\\HaninRevival\\data\\hanin_tools.log", msg, err);
    w[599] = 0;
    MessageBoxW(NULL, w, L"漢音工具箱", MB_ICONERROR);
    ExitProcess(1);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    char la[MAX_PATH], logp[MAX_PATH], *s;
    IMAGE_NT_HEADERS *nt;
    void (*entry)(void);

    GetModuleFileNameA(NULL, g_exeDir, MAX_PATH);
    s = strrchr(g_exeDir, '\\'); if (s) *s = 0;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) return 1;
    parse_cmdline();

    /* started from a containerised app: our profile writes would be virtualised */
    if (!g_isChild && !getenv("HANIN_RELAUNCHED") && hanin_in_package()) {
        SetEnvironmentVariableA("HANIN_RELAUNCHED", "1");
        if (hanin_relaunch_outside_package(g_exeDir)) return 0;
    }
    _snprintf(g_dataDir, MAX_PATH, "%s\\HaninRevival\\data", la);
    _snprintf(logp, MAX_PATH, "%s\\hanin_tools.log", g_dataDir);
    /* both launcher stages (and a toolbox left open) append to the same log */
    g_log = _fsopen(logp, "a", _SH_DENYNO);
    if (!g_log) {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        strcat(tmp, "hanin_tools.log");
        g_log = _fsopen(tmp, "a", _SH_DENYNO);
    }
    logf("=== hanin_tools start pid=%lu%s, data=%s ACP=%u cmd=[%s]", GetCurrentProcessId(),
         g_isChild ? " (child)" : "", g_dataDir, GetACP(), GetCommandLineA());
    if (GetFileAttributesA(g_dataDir) == INVALID_FILE_ATTRIBUTES && !g_isChild) {
        /* never typed with the IME yet: let the engine host create (or rescue) the data dir */
        char exe[MAX_PATH], line[MAX_PATH + 32], ini[MAX_PATH];
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        int i;
        _snprintf(exe, MAX_PATH, "%s\\hanin_host.exe", g_exeDir);
        _snprintf(line, sizeof(line), "\"%s\" --server", exe);
        logf("data directory missing - starting the engine host to create it");
        if (CreateProcessA(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_exeDir, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        _snprintf(ini, MAX_PATH, "%s\\whanin.ini", g_dataDir);
        for (i = 0; i < 60 && GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES; i++) Sleep(100);
        if (!g_log) g_log = _fsopen(logp, "a", _SH_DENYNO);
    }
    if (GetFileAttributesA(g_dataDir) == INVALID_FILE_ATTRIBUTES)
        fail("data directory missing and the engine host could not create it");
    if (GetACP() != 950)
        logf("warning: system ANSI code page is %u, not 950 (Big5); Tools95 text may be garbled", GetACP());

    /* Stage 1: something (not ours) is already mapped at 0x400000 by the time
     * WinMain runs. Re-launch ourselves suspended and reserve the range in the
     * child before any of its code runs. */
    if (!g_isChild) {
        char self[MAX_PATH], line[MAX_PATH + 600];
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        MEMORY_BASIC_INFORMATION mbi;
        void *r;
        GetModuleFileNameA(NULL, self, MAX_PATH);
        _snprintf(line, sizeof(line), "\"%s\" --child%s%s %s", self, g_app[0] ? " --app " : "", g_app, g_userArgs);
        if (!CreateProcessA(self, line, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) fail("cannot create child");
        VirtualQueryEx(pi.hProcess, (void *)0x400000, &mbi, sizeof(mbi));
        logf("child %lu: 0x400000 state=0x%lx type=0x%lx before reserve", pi.dwProcessId, mbi.State, mbi.Type);
        r = VirtualAllocEx(pi.hProcess, (void *)0x400000, 0x42000, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!r) {
            char mapped[MAX_PATH] = "";
            GetMappedFileNameA(pi.hProcess, (void *)0x400000, mapped, MAX_PATH);
            logf("!! reserve in child failed (%lu), mapped=%s", GetLastError(), mapped);
            TerminateProcess(pi.hProcess, 1);
            fail("cannot reserve 0x400000");
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (g_log) fclose(g_log);
        return 0;
    }

    {
        const char *target = !_stricmp(g_app, "query") ? "QUERY.exe" : "Tools95.exe";
        _snprintf(g_toolsPath, MAX_PATH, "%s\\%s", g_exeDir, target);
        if (GetFileAttributesA(g_toolsPath) == INVALID_FILE_ATTRIBUTES)
            _snprintf(g_toolsPath, MAX_PATH, "%s\\payload\\extras\\%s", g_exeDir, target);
        _snprintf(g_targetCmd, sizeof(g_targetCmd), "\"%s\"%s%s", g_toolsPath, g_userArgs[0] ? " " : "", g_userArgs);
        logf("target: %s", g_targetCmd);
    }

    /* must land exactly on 0x400000 (relocations stripped) */
    g_tools = map_fixed(g_toolsPath);
    if (!g_tools) { char m[300]; _snprintf(m, 300, "cannot map %s (err %lu)", g_toolsPath, GetLastError()); fail(m); }
    logf("Tools95 mapped at %p", g_tools);
    if ((DWORD_PTR)g_tools != 0x400000) fail("Tools95 not at its fixed base 0x400000");
    if (!resolve_imports(g_tools)) fail("import resolution failed");

    nt = (IMAGE_NT_HEADERS *)((BYTE *)g_tools + ((IMAGE_DOS_HEADER *)g_tools)->e_lfanew);
    entry = (void (*)(void))((BYTE *)g_tools + nt->OptionalHeader.AddressOfEntryPoint);
    logf("entering Tools95 at %p", entry);
    entry();               /* Tools95's own CRT startup; ends in (hooked) ExitProcess */
    H_ExitProcess(0);
    return 0;
}
