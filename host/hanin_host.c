/*
 * hanin_host.c - 32-bit engine host for the legacy HANIN95.IME (Panasonic 漢音 5.02).
 *
 * The IME DLL is mapped without the loader resolving its imports; we fill the
 * import table ourselves so that every IMM32 service it needs is provided by
 * us, and every dangerous/legacy API it calls (SetCursorPos, keybd_event,
 * WinExec, hard-coded C:\HANIN paths, ...) is redirected, neutralised or logged.
 *
 * Modes:
 *   hanin_host.exe [--server] [--data <dir>] [--verbose]
 *       Named-pipe server used by the TSF front-end (HaninTip*.dll).
 *   hanin_host.exe --test <dataDir> "<key script>" [--verbose]
 *       Drives the engine through exactly the same request path as the server.
 *       Script: plain chars are keys; {ENTER} {ESC} {SPACE} {BS} {DOWN} {UP}
 *       {LEFT} {RIGHT} {SHIFT} {PGDN} {PGUP} {RESET} {COMPLETE} tokens.
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include <sddl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <intrin.h>
#include <share.h>
#include <ctype.h>
#include "../common/protocol.h"
#include "../common/outside_pkg.h"

/* ---- immdev.h subset ---- */
typedef struct tagCOMPOSITIONSTRING {
    DWORD dwSize;
    DWORD dwCompReadAttrLen, dwCompReadAttrOffset;
    DWORD dwCompReadClauseLen, dwCompReadClauseOffset;
    DWORD dwCompReadStrLen, dwCompReadStrOffset;
    DWORD dwCompAttrLen, dwCompAttrOffset;
    DWORD dwCompClauseLen, dwCompClauseOffset;
    DWORD dwCompStrLen, dwCompStrOffset;
    DWORD dwCursorPos, dwDeltaStart;
    DWORD dwResultReadClauseLen, dwResultReadClauseOffset;
    DWORD dwResultReadStrLen, dwResultReadStrOffset;
    DWORD dwResultClauseLen, dwResultClauseOffset;
    DWORD dwResultStrLen, dwResultStrOffset;
    DWORD dwPrivateSize, dwPrivateOffset;
} COMPOSITIONSTRING;

typedef struct tagINPUTCONTEXTA {
    HWND hWnd;
    BOOL fOpen;
    POINT ptStatusWndPos;
    POINT ptSoftKbdPos;
    DWORD fdwConversion;
    DWORD fdwSentence;
    union { LOGFONTA A; LOGFONTW W; } lfFont;
    COMPOSITIONFORM cfCompForm;
    CANDIDATEFORM cfCandForm[4];
    HIMCC hCompStr, hCandInfo, hGuideLine, hPrivate;
    DWORD dwNumMsgBuf;
    HIMCC hMsgBuf;
    DWORD fdwInit;
    DWORD dwReserve[3];
} INPUTCONTEXT, *LPINPUTCONTEXT;

typedef struct tagIMEINFO {
    DWORD dwPrivateDataSize, fdwProperty, fdwConversionCaps, fdwSentenceCaps,
          fdwUICaps, fdwSCSCaps, fdwSelectCaps;
} IMEINFO, *LPIMEINFO;

typedef struct { UINT message; WPARAM wParam; LPARAM lParam; } TRANSMSG;

#define IMMGWL_IMC 0
#define IME_PROP_KBD_CHAR_FIRST 0x00000002
#define INIT_STATUSWNDPOS 0x01
#define INIT_CONVERSION   0x02
#define INIT_SENTENCE     0x04
#define INIT_LOGFONT      0x08
#define INIT_COMPFORM     0x10

typedef BOOL (WINAPI *PFN_ImeInquire)(LPIMEINFO, LPSTR, DWORD);
typedef BOOL (WINAPI *PFN_ImeSelect)(HIMC, BOOL);
typedef BOOL (WINAPI *PFN_ImeSetActiveContext)(HIMC, BOOL);
typedef BOOL (WINAPI *PFN_ImeProcessKey)(HIMC, UINT, LPARAM, CONST LPBYTE);
typedef UINT (WINAPI *PFN_ImeToAsciiEx)(UINT, UINT, CONST LPBYTE, LPDWORD, UINT, HIMC);
typedef BOOL (WINAPI *PFN_NotifyIME)(HIMC, DWORD, DWORD, DWORD);
typedef BOOL (WINAPI *PFN_ImeDestroy)(UINT);
typedef BOOL (WINAPI *PFN_DllMain)(HINSTANCE, DWORD, LPVOID);

static PFN_ImeInquire pImeInquire;
static PFN_ImeSelect pImeSelect;
static PFN_ImeSetActiveContext pImeSetActiveContext;
static PFN_ImeProcessKey pImeProcessKey;
static PFN_ImeToAsciiEx pImeToAsciiEx;
static PFN_NotifyIME pNotifyIME;
static PFN_ImeDestroy pImeDestroy;

/* ---- globals ---- */
static FILE   *g_log;
static int     g_verbose;
static int     g_testMode;
static char    g_dataDir[MAX_PATH];
static HMODULE g_ime;
static IMEINFO g_imeInfo;
static char    g_uiClass[64];
static HWND    g_hwndApp, g_hwndUI, g_hwndCtl;
static BYTE    g_keyState[256];
static char    g_committed[1024];
static int     g_committedLen;
static volatile LONG  g_busy;
static volatile DWORD g_busyTick;
static DWORD   g_ownerTid;
static CRITICAL_SECTION g_reqLock;

#define WM_APP_REQ   (WM_APP + 1)
#define WM_APP_GONE  (WM_APP + 2)

/* ======================================================================= */
/* logging                                                                 */
/* ======================================================================= */
static void vlog(const char *fmt, va_list ap)
{
    char buf[2048];
    SYSTEMTIME st;
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    GetLocalTime(&st);
    if (g_log) {
        fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
        fflush(g_log);
    }
    if (g_testMode) { fprintf(stdout, "%s\n", buf); fflush(stdout); }
}
static void logf(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
static void logv(const char *fmt, ...)  { va_list ap; if (!g_verbose) return; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }

static void open_log(void)
{
    char p[MAX_PATH], old[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    _snprintf(p, MAX_PATH, "%s\\hanin_host.log", g_dataDir);
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 4 * 1024 * 1024) {
        _snprintf(old, MAX_PATH, "%s\\hanin_host.old.log", g_dataDir);
        MoveFileExA(p, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _fsopen(p, g_testMode ? "w" : "a", _SH_DENYWR);
}

/* Big5 (cp950) -> UTF-8 for logging */
static const char *b5(const char *s, int len)
{
    static char out[8][1024];
    static int slot;
    WCHAR w[512];
    int n;
    char *o = out[slot++ & 7];
    if (!s) return "(null)";
    if (len < 0) len = (int)strlen(s);
    n = MultiByteToWideChar(950, 0, s, len, w, 511);
    n = WideCharToMultiByte(CP_UTF8, 0, w, n, o, 1023, NULL, NULL);
    o[n] = 0;
    return o;
}
static const char *w8(const wchar_t *w)
{
    static char out[4][4096];
    static int slot;
    char *o = out[slot++ & 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, o, 4095, NULL, NULL);
    o[n > 0 ? n - 1 : 0] = 0;
    return o;
}

/* ======================================================================= */
/* fake IMM32 (HIMCC == movable LocalAlloc handle, as in the real IMM32)   */
/* ======================================================================= */
typedef struct { DWORD magic; INPUTCONTEXT ic; DWORD lock; } FAKEIMC;
#define IMC_MAGIC 0x434D4948
static FAKEIMC *g_imc;

HIMCC WINAPI F_ImmCreateIMCC(DWORD size) { return (HIMCC)LocalAlloc(LHND, size < 4 ? 4 : size); }
HIMCC WINAPI F_ImmDestroyIMCC(HIMCC h)   { return (HIMCC)LocalFree((HLOCAL)h); }
LPVOID WINAPI F_ImmLockIMCC(HIMCC h)
{
    LPVOID p = LocalLock((HLOCAL)h);
    if (!p) logf("!! ImmLockIMCC(%p) failed err=%lu ret=%p", h, GetLastError(), _ReturnAddress());
    return p;
}
BOOL WINAPI F_ImmUnlockIMCC(HIMCC h) { return LocalUnlock((HLOCAL)h); }
HIMCC WINAPI F_ImmReSizeIMCC(HIMCC h, DWORD size)
{
    HIMCC r = (HIMCC)LocalReAlloc((HLOCAL)h, size < 4 ? 4 : size, LMEM_MOVEABLE | LMEM_ZEROINIT);
    if (!r) logf("!! ImmReSizeIMCC(%p,%u) failed err=%lu", h, size, GetLastError());
    return r;
}
LPINPUTCONTEXT WINAPI F_ImmLockIMC(HIMC h)
{
    FAKEIMC *c = (FAKEIMC *)h;
    if (!c || c->magic != IMC_MAGIC) { logf("!! ImmLockIMC bad HIMC %p ret=%p", h, _ReturnAddress()); return NULL; }
    c->lock++;
    return &c->ic;
}
BOOL WINAPI F_ImmUnlockIMC(HIMC h)
{
    FAKEIMC *c = (FAKEIMC *)h;
    if (!c || c->magic != IMC_MAGIC) return FALSE;
    if (c->lock) c->lock--;
    return TRUE;
}

static void dispatch_msgs(const TRANSMSG *m, DWORD n, const char *src);

BOOL WINAPI F_ImmGenerateMessage(HIMC h)
{
    FAKEIMC *c = (FAKEIMC *)h;
    DWORD n;
    TRANSMSG *m, copy[64];
    if (!c || c->magic != IMC_MAGIC) return FALSE;
    n = c->ic.dwNumMsgBuf;
    if (!n || !c->ic.hMsgBuf) return TRUE;
    m = (TRANSMSG *)F_ImmLockIMCC(c->ic.hMsgBuf);
    if (!m) return FALSE;
    if (n > 64) n = 64;
    memcpy(copy, m, n * sizeof(TRANSMSG));
    F_ImmUnlockIMCC(c->ic.hMsgBuf);
    c->ic.dwNumMsgBuf = 0;
    dispatch_msgs(copy, n, "ImmGenerateMessage");
    return TRUE;
}

/* ======================================================================= */
/* KERNEL32 / USER32 / GDI32 interception                                  */
/* ======================================================================= */
static const char *redirect(const char *path, char *buf)
{
    static const char pre[] = "C:\\HANIN";
    if (!path) return path;
    if (_strnicmp(path, pre, sizeof(pre) - 1) == 0 && (path[8] == 0 || path[8] == '\\')) {
        _snprintf(buf, MAX_PATH, "%s%s", g_dataDir, path + 8);
        return buf;
    }
    if (!strchr(path, '\\') && !strchr(path, '/')) {
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
    int tries = 0;
    /* a previous engine instance (being restarted) may still hold the file */
    while (r == HFILE_ERROR && of->nErrCode == ERROR_SHARING_VIOLATION && tries++ < 15) {
        Sleep(100);
        r = OpenFile(p, of, style);
    }
    if (r == HFILE_ERROR && *name) logf("[k32] OpenFile(%s -> %s, 0x%x) failed (err %u, %d retries)", b5(name, -1), p, style, of->nErrCode, tries);
    else logv("[k32] OpenFile(%s -> %s, 0x%x) = %d", b5(name, -1), p, style, r);
    return r;
}
HFILE WINAPI H__lopen(LPCSTR name, int mode)
{
    char b[MAX_PATH]; const char *p = redirect(name, b);
    HFILE r = _lopen(p, mode);
    if (r == HFILE_ERROR) logf("[k32] _lopen(%s -> %s, %d) failed", b5(name, -1), p, mode);
    else logv("[k32] _lopen(%s -> %s) = %d", b5(name, -1), p, r);
    return r;
}
DWORD WINAPI H_GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file)
{
    char b[MAX_PATH]; const char *p = redirect(file, b);
    DWORD r = GetPrivateProfileStringA(app, key, def, out, n, p);
    logv("[k32] GetPrivateProfileString [%s] %s = \"%s\"", app ? app : "(null)", key ? key : "(null)", b5(out, -1));
    return r;
}
UINT WINAPI H_GetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file)
{
    char b[MAX_PATH]; const char *p = redirect(file, b);
    return GetPrivateProfileIntA(app, key, def, p);
}
BOOL WINAPI H_WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file)
{
    char b[MAX_PATH]; const char *p = redirect(file, b);
    logv("[k32] WritePrivateProfileString [%s] %s = \"%s\"", app ? app : "", key ? key : "", val ? b5(val, -1) : "(null)");
    return WritePrivateProfileStringA(app, key, val, p);
}
/* The engine starts its companions with WinExec("C:\HANIN\tools95 <arg>") (Ctrl+6)
 * and WinExec("C:\HANIN\query95 <arg>") (Ctrl+2). Run our launcher instead,
 * passing the original argument through; anything else stays blocked. */
/* start hanin_tools.exe (toolbox, or "--app query " for 文字查詢) next to this exe */
static BOOL launch_companion(const char *appArg, const char *args)
{
    char exe[MAX_PATH], line[MAX_PATH + 600], dir[MAX_PATH], *sl;
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    sl = strrchr(dir, '\\'); if (sl) *sl = 0;
    _snprintf(exe, MAX_PATH, "%s\\hanin_tools.exe", dir);
    _snprintf(line, sizeof(line), "\"%s\" %s%s", exe, appArg, args ? args : "");
    if (!CreateProcessA(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        logf("launch %s failed (%lu)", line, GetLastError());
        return FALSE;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    logf("launched %s", line);
    return TRUE;
}

UINT WINAPI H_WinExec(LPCSTR cmd, UINT show)
{
    char lower[512];
    const char *app = NULL, *args;
    size_t i;
    if (!cmd) return 2;
    for (i = 0; i < sizeof(lower) - 1 && cmd[i]; i++) lower[i] = (char)tolower((unsigned char)cmd[i]);
    lower[i] = 0;
    if (strstr(lower, "\\tools95")) app = "";
    else if (strstr(lower, "\\query95")) app = "--app query ";
    if (!app) { logf("[k32] WinExec(\"%s\") BLOCKED", b5(cmd, -1)); return 2; }
    args = strchr(cmd, ' ');
    logf("[k32] WinExec(\"%s\") redirected", b5(cmd, -1));
    return launch_companion(app, args ? args + 1 : "") ? 33 : 2;
}

SHORT WINAPI H_GetKeyState(int vk) { return (SHORT)(((g_keyState[vk & 0xFF] & 0x80) ? 0x8000 : 0) | (g_keyState[vk & 0xFF] & 1)); }
BOOL  WINAPI H_GetKeyboardState(PBYTE ks) { memcpy(ks, g_keyState, 256); return TRUE; }
VOID  WINAPI H_keybd_event(BYTE vk, BYTE scan, DWORD flags, ULONG_PTR extra) { logf("[u32] keybd_event(vk=0x%02x, flags=0x%x) BLOCKED", vk, flags); }
BOOL WINAPI H_SetForegroundWindow(HWND h) { logv("[u32] SetForegroundWindow ignored"); return TRUE; }
BOOL WINAPI H_SetCursorPos(int x, int y) { logv("[u32] SetCursorPos(%d,%d) BLOCKED", x, y); return TRUE; }
BOOL WINAPI H_ClipCursor(const RECT *r) { logf("[u32] ClipCursor BLOCKED"); return TRUE; }
HWND WINAPI H_SetCapture(HWND h) { logv("[u32] SetCapture ignored"); return NULL; }
int  WINAPI H_MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type)
{
    logf("[u32] MessageBoxA(\"%s\", \"%s\", 0x%x) suppressed", b5(cap ? cap : "", -1), b5(text ? text : "", -1), type);
    return IDOK;
}
BOOL WINAPI H_MessageBeep(UINT t) { logv("[u32] MessageBeep(%u)", t); return TRUE; }
HWND WINAPI H_GetFocus(void) { return g_hwndApp; }
HWND WINAPI H_FindWindowA(LPCSTR cls, LPCSTR title)
{
    logv("[u32] FindWindowA(%s, %s)", cls && HIWORD(cls) ? cls : "(atom)", title ? b5(title, -1) : "(null)");
    return FindWindowA(cls, title);
}
BOOL WINAPI H_EnumWindows(WNDENUMPROC fn, LPARAM l) { logv("[u32] EnumWindows ignored"); return TRUE; }
BOOL WINAPI H_WinHelpA(HWND h, LPCSTR f, UINT c, ULONG_PTR d) { logv("[u32] WinHelpA ignored"); return TRUE; }
BOOL WINAPI H_GetSaveFileNameA(LPVOID p) { logf("[cdlg] GetSaveFileNameA ignored"); return FALSE; }
BOOL WINAPI H_PostMessageA(HWND h, UINT m, WPARAM w, LPARAM l)
{
    logv("[u32] PostMessageA(%p, 0x%04x, 0x%x, 0x%x)", h, m, (unsigned)w, (unsigned)l);
    return PostMessageA(h, m, w, l);
}

/* The IME's own windows (composition, status, candidate grid, ...) are real
 * windows of this process. They must never be seen, take focus or catch the
 * mouse: they become fully transparent, click-through, non-activating tool
 * windows. They still paint (layered windows are redirected), which is how
 * the candidate grid is scraped. */
HWND WINAPI H_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD st, int x, int y, int w, int hh,
                              HWND parent, HMENU menu, HINSTANCE inst, LPVOID p)
{
    HWND r;
    BOOL top = !(st & WS_CHILD);
    if (top) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    ex &= ~WS_EX_APPWINDOW;
    r = CreateWindowExA(ex, cls, title, st, x, y, w, hh, parent, menu, inst, p);
    if (r && top) SetLayeredWindowAttributes(r, 0, 0, LWA_ALPHA);
    logv("[u32] CreateWindowExA(%s, \"%s\", st=0x%x) = %p", cls && HIWORD(cls) ? cls : "(atom)", title ? b5(title, -1) : "", st, r);
    if (!r) logf("!! CreateWindowExA(%s) failed err=%lu", cls && HIWORD(cls) ? cls : "(atom)", GetLastError());
    return r;
}
BOOL WINAPI H_ShowWindow(HWND h, int cmd)
{
    if (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE) cmd = SW_SHOWNOACTIVATE;
    return ShowWindow(h, cmd);
}
BOOL WINAPI H_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    return SetWindowPos(h, after, x, y, cx, cy, f | SWP_NOACTIVATE);
}
BOOL WINAPI H_BringWindowToTop(HWND h) { return TRUE; }

/* ---- candidate-grid scraping ---- */
typedef struct { int x, y; char s[96]; } CAPITEM;
static CAPITEM g_cap[512];
static int     g_ncap;
static int     g_capturing;

static void cap_add(int x, int y, LPCSTR s, int n)
{
    if (!g_capturing || g_ncap >= 512 || n <= 0) return;
    if (n > 95) n = 95;
    g_cap[g_ncap].x = x; g_cap[g_ncap].y = y;
    memcpy(g_cap[g_ncap].s, s, n); g_cap[g_ncap].s[n] = 0;
    g_ncap++;
}
BOOL WINAPI H_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
    if (n < 0) n = (int)strlen(s);
    cap_add(x, y, s, n);
    return TextOutA(dc, x, y, s, n);
}
int WINAPI H_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT r, UINT f)
{
    if (n < 0) n = (int)strlen(s);
    if (r) cap_add(r->left, r->top, s, n);
    return DrawTextA(dc, s, n, r, f);
}

typedef struct { const char *dll, *name; void *fn; } HOOK;
static const HOOK g_hooks[] = {
    { "IMM32.dll", "ImmLockIMC", F_ImmLockIMC },
    { "IMM32.dll", "ImmUnlockIMC", F_ImmUnlockIMC },
    { "IMM32.dll", "ImmCreateIMCC", F_ImmCreateIMCC },
    { "IMM32.dll", "ImmDestroyIMCC", F_ImmDestroyIMCC },
    { "IMM32.dll", "ImmLockIMCC", F_ImmLockIMCC },
    { "IMM32.dll", "ImmUnlockIMCC", F_ImmUnlockIMCC },
    { "IMM32.dll", "ImmReSizeIMCC", F_ImmReSizeIMCC },
    { "IMM32.dll", "ImmGenerateMessage", F_ImmGenerateMessage },
    { "KERNEL32.dll", "GetWindowsDirectoryA", H_GetWindowsDirectoryA },
    { "KERNEL32.dll", "OpenFile", H_OpenFile },
    { "KERNEL32.dll", "_lopen", H__lopen },
    { "KERNEL32.dll", "GetPrivateProfileStringA", H_GetPrivateProfileStringA },
    { "KERNEL32.dll", "GetPrivateProfileIntA", H_GetPrivateProfileIntA },
    { "KERNEL32.dll", "WritePrivateProfileStringA", H_WritePrivateProfileStringA },
    { "KERNEL32.dll", "WinExec", H_WinExec },
    { "USER32.dll", "GetKeyState", H_GetKeyState },
    { "USER32.dll", "GetKeyboardState", H_GetKeyboardState },
    { "USER32.dll", "keybd_event", H_keybd_event },
    { "USER32.dll", "SetForegroundWindow", H_SetForegroundWindow },
    { "USER32.dll", "SetCursorPos", H_SetCursorPos },
    { "USER32.dll", "ClipCursor", H_ClipCursor },
    { "USER32.dll", "SetCapture", H_SetCapture },
    { "USER32.dll", "MessageBoxA", H_MessageBoxA },
    { "USER32.dll", "MessageBeep", H_MessageBeep },
    { "USER32.dll", "GetFocus", H_GetFocus },
    { "USER32.dll", "FindWindowA", H_FindWindowA },
    { "USER32.dll", "EnumWindows", H_EnumWindows },
    { "USER32.dll", "WinHelpA", H_WinHelpA },
    { "USER32.dll", "PostMessageA", H_PostMessageA },
    { "USER32.dll", "CreateWindowExA", H_CreateWindowExA },
    { "USER32.dll", "ShowWindow", H_ShowWindow },
    { "USER32.dll", "SetWindowPos", H_SetWindowPos },
    { "USER32.dll", "BringWindowToTop", H_BringWindowToTop },
    { "USER32.dll", "DrawTextA", H_DrawTextA },
    { "GDI32.dll", "TextOutA", H_TextOutA },
    { "comdlg32.dll", "GetSaveFileNameA", H_GetSaveFileNameA },
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

/* ======================================================================= */
/* message plumbing (what IMM32 + DefWindowProc would normally do)         */
/* ======================================================================= */
static void commit_bytes(const char *p, int n)
{
    if (g_committedLen + n < (int)sizeof(g_committed) - 1) {
        memcpy(g_committed + g_committedLen, p, n);
        g_committedLen += n;
        g_committed[g_committedLen] = 0;
    }
}

static void dispatch_msgs(const TRANSMSG *m, DWORD n, const char *src)
{
    DWORD i;
    for (i = 0; i < n; i++) {
        logv("  <%s> msg 0x%04x w=0x%x l=0x%x", src, m[i].message, (unsigned)m[i].wParam, (unsigned)m[i].lParam);
        SendMessageA(g_hwndApp, m[i].message, m[i].wParam, m[i].lParam);
    }
}

static LRESULT CALLBACK AppWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_IME_CHAR: {
        char c[2]; int n = 0;
        if (w > 0xFF) c[n++] = (char)(w >> 8);
        c[n++] = (char)(w & 0xFF);
        commit_bytes(c, n);
        return 0;
    }
    case WM_CHAR: {
        char c = (char)w;
        commit_bytes(&c, 1);
        return 0;
    }
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_NOTIFY:
    case WM_IME_SETCONTEXT:
    case WM_IME_SELECT:
    case WM_IME_CONTROL:
    case WM_IME_COMPOSITIONFULL:
        if (msg == WM_IME_COMPOSITION && (l & GCS_RESULTSTR) && g_imc) {
            COMPOSITIONSTRING *cs = (COMPOSITIONSTRING *)F_ImmLockIMCC(g_imc->ic.hCompStr);
            if (cs && cs->dwResultStrLen)
                commit_bytes((char *)cs + cs->dwResultStrOffset, cs->dwResultStrLen);
            F_ImmUnlockIMCC(g_imc->ic.hCompStr);
            if (g_hwndUI) SendMessageA(g_hwndUI, msg, w, l & ~GCS_RESULTSTR);
            return 0;
        }
        if (g_hwndUI) return SendMessageA(g_hwndUI, msg, w, l);
        return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

static void pump(void)
{
    MSG m;
    int guard = 0;
    while (guard++ < 1000 && PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

/* ======================================================================= */
/* engine                                                                  */
/* ======================================================================= */
static void fatal_restart(const char *why)
{
    logf("!! FATAL: %s -> terminating host (front-end will restart it)", why);
    if (g_log) fflush(g_log);
    if (!g_testMode) TerminateProcess(GetCurrentProcess(), 3);
}

#define SEH_CALL(expr, what, failval, out) \
    __try { out = (expr); } __except (EXCEPTION_EXECUTE_HANDLER) { \
        char _b[128]; _snprintf(_b, 128, "exception 0x%08x in %s", GetExceptionCode(), what); \
        out = failval; fatal_restart(_b); }

static BOOL engine_init(void)
{
    char path[MAX_PATH];
    PFN_DllMain entry;
    IMAGE_NT_HEADERS *nt;
    WNDCLASSA wc = {0};
    BOOL ok;

    _snprintf(path, MAX_PATH, "%s\\HANIN95.IME", g_dataDir);
    g_ime = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!g_ime) { logf("!! LoadLibraryEx(%s) failed %lu", path, GetLastError()); return FALSE; }
    if (!resolve_imports(g_ime)) return FALSE;

    nt = (IMAGE_NT_HEADERS *)((BYTE *)g_ime + ((IMAGE_DOS_HEADER *)g_ime)->e_lfanew);
    entry = (PFN_DllMain)((BYTE *)g_ime + nt->OptionalHeader.AddressOfEntryPoint);
    SEH_CALL(entry((HINSTANCE)g_ime, DLL_PROCESS_ATTACH, NULL), "DllMain", FALSE, ok);
    if (!ok) { logf("!! DllMain failed"); return FALSE; }

#define GETP(v, n) v = (void *)GetProcAddress(g_ime, n); if (!v) { logf("!! missing export %s", n); return FALSE; }
    GETP(pImeInquire, "ImeInquire");
    GETP(pImeSelect, "ImeSelect");
    GETP(pImeSetActiveContext, "ImeSetActiveContext");
    GETP(pImeProcessKey, "ImeProcessKey");
    GETP(pImeToAsciiEx, "ImeToAsciiEx");
    GETP(pNotifyIME, "NotifyIME");
    GETP(pImeDestroy, "ImeDestroy");

    SEH_CALL(pImeInquire(&g_imeInfo, g_uiClass, 0), "ImeInquire", FALSE, ok);
    if (!ok) return FALSE;
    logf("ImeInquire ok: uiClass='%s' prop=0x%08x", g_uiClass, g_imeInfo.fdwProperty);

    wc.lpfnWndProc = AppWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "HaninHostApp";
    RegisterClassA(&wc);
    g_hwndApp = CreateWindowExA(WS_EX_TOOLWINDOW, "HaninHostApp", "Hanin host", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);

    g_imc = (FAKEIMC *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FAKEIMC));
    g_imc->magic = IMC_MAGIC;
    g_imc->ic.hWnd = g_hwndApp;
    g_imc->ic.fOpen = TRUE;
    g_imc->ic.fdwConversion = IME_CMODE_NATIVE;
    g_imc->ic.fdwSentence = IME_SMODE_PHRASEPREDICT;
    g_imc->ic.lfFont.A.lfHeight = -16;
    g_imc->ic.lfFont.A.lfCharSet = CHINESEBIG5_CHARSET;
    g_imc->ic.cfCompForm.dwStyle = CFS_DEFAULT;
    g_imc->ic.fdwInit = INIT_CONVERSION | INIT_SENTENCE | INIT_LOGFONT | INIT_STATUSWNDPOS | INIT_COMPFORM;
    g_imc->ic.hCompStr = F_ImmCreateIMCC(sizeof(COMPOSITIONSTRING));
    g_imc->ic.hCandInfo = F_ImmCreateIMCC(sizeof(DWORD) * 36);
    g_imc->ic.hGuideLine = F_ImmCreateIMCC(12 * sizeof(DWORD));
    g_imc->ic.hPrivate = F_ImmCreateIMCC(g_imeInfo.dwPrivateDataSize);
    g_imc->ic.hMsgBuf = F_ImmCreateIMCC(sizeof(TRANSMSG));

    if (g_uiClass[0]) {
        g_hwndUI = CreateWindowExA(WS_EX_TOOLWINDOW, g_uiClass, "", WS_POPUP | WS_DISABLED, 0, 0, 0, 0, NULL, NULL, (HINSTANCE)g_ime, NULL);
        if (g_hwndUI) SetWindowLongA(g_hwndUI, IMMGWL_IMC, (LONG)(LONG_PTR)g_imc);
        else logf("!! UI window creation failed err=%lu", GetLastError());
    }

    SEH_CALL(pImeSelect((HIMC)g_imc, TRUE), "ImeSelect", FALSE, ok);
    if (!ok) return FALSE;
    SEH_CALL(pImeSetActiveContext((HIMC)g_imc, TRUE), "ImeSetActiveContext", FALSE, ok);
    if (g_hwndUI) {
        SendMessageA(g_hwndUI, WM_IME_SELECT, TRUE, 0);
        SendMessageA(g_hwndUI, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
    }
    pump();
    logf("engine ready (conv=0x%x)", g_imc->ic.fdwConversion);
    return TRUE;
}

static BOOL is_ui_class(const char *c)
{
    static const char *ignore[] = { "CompWnd", "HaninState", "kbc", "HaninMouse", "Hanin95", "HaninHostApp", NULL };
    int i;
    for (i = 0; ignore[i]; i++) if (!strcmp(c, ignore[i])) return FALSE;
    return TRUE;
}

typedef struct { HWND found; } FINDCAND;
static BOOL CALLBACK find_cand_proc(HWND h, LPARAM l)
{
    char c[64];
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, c, 64);
    if (is_ui_class(c)) { ((FINDCAND *)l)->found = h; return FALSE; }
    return TRUE;
}

static int cap_cmp(const void *a, const void *b)
{
    const CAPITEM *x = (const CAPITEM *)a, *y = (const CAPITEM *)b;
    if (x->y != y->y) return x->y - y->y;
    return x->x - y->x;
}

static void wcat(wchar_t *dst, int cap, const wchar_t *src)
{
    int n = (int)wcslen(dst);
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
}
static void wcat_b5(wchar_t *dst, int cap, const char *s)
{
    wchar_t w[128];
    int n = MultiByteToWideChar(950, 0, s, -1, w, 127);
    w[n > 0 ? n - 1 : 0] = 0;
    wcat(dst, cap, w);
}

/* Re-paint the visible candidate window synchronously, capture what it draws,
 * and turn it into the row/footer model of protocol.h. */
static void snapshot_candidates(HANIN_RESP *r)
{
    FINDCAND fc = { NULL };
    char title[128];
    int i, j;
    EnumThreadWindows(GetCurrentThreadId(), find_cand_proc, (LPARAM)&fc);
    r->candOpen = fc.found != NULL;
    r->cand[0] = 0;
    if (!fc.found) return;

    g_ncap = 0;
    g_capturing = 1;
    RedrawWindow(fc.found, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    g_capturing = 0;
    qsort(g_cap, g_ncap, sizeof(CAPITEM), cap_cmp);

    GetWindowTextA(fc.found, title, sizeof(title));
    wcat(r->cand, HANIN_MAX_CAND, L"T\t");
    wcat_b5(r->cand, HANIN_MAX_CAND, title);
    wcat(r->cand, HANIN_MAX_CAND, L"\n");

    for (i = 0; i < g_ncap; ) {
        int y = g_cap[i].y, end = i, active = 0;
        char label[8] = "";
        while (end < g_ncap && g_cap[end].y == y) end++;
        if (end - i == 1 && g_cap[i].s[0] == '(') {
            wcat(r->cand, HANIN_MAX_CAND, L"F\t");
            wcat_b5(r->cand, HANIN_MAX_CAND, g_cap[i].s);
            wcat(r->cand, HANIN_MAX_CAND, L"\n");
            i = end;
            continue;
        }
        j = i;
        if (g_cap[j].s[1] == 0 && g_cap[j].s[0] >= 'A' && g_cap[j].s[0] <= 'Z' && end - i > 1) { strcpy(label, g_cap[j].s); j++; }
        /* digit labels: a single digit drawn just left of the next item */
        for (; j < end; j++) {
            if (g_cap[j].s[1] == 0 && g_cap[j].s[0] >= '0' && g_cap[j].s[0] <= '9' && j + 1 < end && g_cap[j + 1].x - g_cap[j].x <= 12) {
                active = 1; g_cap[j].s[0] = 1; /* mark as label */
            }
        }
        wcat(r->cand, HANIN_MAX_CAND, L"R\t");
        wcat_b5(r->cand, HANIN_MAX_CAND, label);
        wcat(r->cand, HANIN_MAX_CAND, active ? L"\t1" : L"\t0");
        for (j = label[0] ? i + 1 : i; j < end; j++) {
            if (g_cap[j].s[0] == 1) continue;
            wcat(r->cand, HANIN_MAX_CAND, L"\t");
            wcat_b5(r->cand, HANIN_MAX_CAND, g_cap[j].s);
        }
        wcat(r->cand, HANIN_MAX_CAND, L"\n");
        i = end;
    }
}

/* Hanin keeps its Chinese/English state in its private block (hPrivate holds a
 * HGLOBAL): word at +0x5F2 is the active input method, toggled by a Shift tap. */
static int engine_method(int *saved)
{
    int m = -1;
    HGLOBAL h;
    HWND ui = NULL;
    DWORD *pp = (DWORD *)F_ImmLockIMCC(g_imc->ic.hPrivate);
    if (pp) { ui = (HWND)(DWORD_PTR)*pp; F_ImmUnlockIMCC(g_imc->ic.hPrivate); }
    if (!ui || !IsWindow(ui)) ui = g_hwndUI;
    h = (HGLOBAL)(LONG_PTR)GetWindowLongA(ui, 4 /* IMMGWL_PRIVATE */);
    if (!h) return -1;
    __try {
        BYTE *priv = (BYTE *)GlobalLock(h);
        if (priv) {
            m = *(WORD *)(priv + 0x5F2);
            if (saved) *saved = *(WORD *)(priv + 0x5F4);
            GlobalUnlock(h);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m = -2;
    }
    return m;
}

static void snapshot(HANIN_RESP *r)
{
    COMPOSITIONSTRING *cs;
    int n;
    r->method = engine_method(NULL);
    r->open = g_imc->ic.fOpen;
    r->convMode = g_imc->ic.fdwConversion;

    n = MultiByteToWideChar(950, 0, g_committed, g_committedLen, r->result, HANIN_MAX_RESULT - 1);
    r->result[n] = 0;

    r->comp[0] = 0; r->compCursor = 0;
    cs = (COMPOSITIONSTRING *)F_ImmLockIMCC(g_imc->ic.hCompStr);
    if (cs && cs->dwSize && cs->dwCompStrLen) {
        const char *s = (const char *)cs + cs->dwCompStrOffset;
        DWORD cur = cs->dwCursorPos > cs->dwCompStrLen ? cs->dwCompStrLen : cs->dwCursorPos;
        n = MultiByteToWideChar(950, 0, s, cs->dwCompStrLen, r->comp, HANIN_MAX_COMP - 1);
        r->comp[n] = 0;
        r->compCursor = cur ? MultiByteToWideChar(950, 0, s, cur, NULL, 0) : 0;
    }
    F_ImmUnlockIMCC(g_imc->ic.hCompStr);
    snapshot_candidates(r);
}

static BOOL comp_empty(void)
{
    COMPOSITIONSTRING *cs = (COMPOSITIONSTRING *)F_ImmLockIMCC(g_imc->ic.hCompStr);
    BOOL e = !(cs && cs->dwSize && cs->dwCompStrLen);
    F_ImmUnlockIMCC(g_imc->ic.hCompStr);
    return e;
}

static struct { UINT vk; BOOL up; BOOL result; BOOL valid; } g_lastTest;

static BOOL do_process_key(UINT vk, LPARAM lk)
{
    BOOL r;
    SEH_CALL(pImeProcessKey((HIMC)g_imc, vk, lk, g_keyState), "ImeProcessKey", FALSE, r);
    return r;
}

static UINT do_to_ascii(UINT vk, LPARAM lk, BOOL up)
{
    DWORD buf[1 + 3 * 64];
    WORD ch = 0;
    UINT uvk = vk, n;
    if (!up && (g_imeInfo.fdwProperty & IME_PROP_KBD_CHAR_FIRST)) {
        if (ToAscii(vk, (lk >> 16) & 0xFF, g_keyState, &ch, 0) == 1)
            uvk |= ((UINT)(BYTE)ch) << 8;   /* ANSI IME: char code in bits 8-15 */
    }
    buf[0] = 64;
    SEH_CALL(pImeToAsciiEx(uvk, HIWORD(lk), g_keyState, buf, 0, (HIMC)g_imc), "ImeToAsciiEx", 0, n);
    if (n > 64) F_ImmGenerateMessage((HIMC)g_imc);
    else if (n) dispatch_msgs((TRANSMSG *)(buf + 1), n, "ToAsciiEx");
    return n;
}

static void do_reset(void)
{
    BOOL r;
    int i;
    SEH_CALL(pNotifyIME((HIMC)g_imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0), "NotifyIME(CANCEL)", FALSE, r);
    pump();
    /* fallback: ESC until the composition and any selection window are gone */
    for (i = 0; i < 4; i++) {
        FINDCAND fc = { NULL };
        EnumThreadWindows(GetCurrentThreadId(), find_cand_proc, (LPARAM)&fc);
        if (comp_empty() && !fc.found) break;
        memset(g_keyState, 0, 256);
        g_keyState[VK_ESCAPE] = 0x80;
        if (do_process_key(VK_ESCAPE, 1 | (1 << 16))) do_to_ascii(VK_ESCAPE, 1 | (1 << 16), FALSE);
        g_keyState[VK_ESCAPE] = 0;
        pump();
    }
    g_committedLen = 0; g_committed[0] = 0;   /* a cancel never commits */
}

/* Single entry point for all requests; runs on the thread that owns the IME windows. */
/* whanin.ini HotKey= lists 13 hotkeys as (mods << 8 | vk), mods: 1=Alt 2=Shift 4=Ctrl,
 * in the order of the toolbox's 快速鍵 page. These ones open the engine's own
 * floating windows (quick buttons, on-screen keyboard, WinHelp, button layout),
 * which cannot be shown/used through the TSF front-end yet: give such keys back
 * to the application instead of swallowing them silently. */
static const int kDeadHotkeyIdx[] = { 5 /* 快速功能鈕 */, 9 /* 螢幕鍵盤 */, 11 /* 線上說明 */, 12 /* 橫/直排 */ };
static int g_hotkeys[16], g_nHotkeys = -1;
static FILETIME g_iniTime;

static void load_hotkeys(void)
{
    char ini[MAX_PATH], buf[256], *p;
    WIN32_FILE_ATTRIBUTE_DATA a;
    _snprintf(ini, MAX_PATH, "%s\\whanin.ini", g_dataDir);
    if (!GetFileAttributesExA(ini, GetFileExInfoStandard, &a)) return;
    if (g_nHotkeys >= 0 && !CompareFileTime(&a.ftLastWriteTime, &g_iniTime)) return;
    g_iniTime = a.ftLastWriteTime;
    GetPrivateProfileStringA("Hanin IME", "HotKey", "", buf, sizeof(buf), ini);
    g_nHotkeys = 0;
    for (p = buf; *p && g_nHotkeys < 16; ) {
        g_hotkeys[g_nHotkeys++] = atoi(p);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    logv("hotkeys reloaded (%d)", g_nHotkeys);
}

static BOOL is_dead_hotkey(const HANIN_REQ *q)
{
    int mods = 0, code, i;
    if (q->flags & HREQ_UP) return FALSE;
    load_hotkeys();
    if (q->keystate[VK_MENU] & 0x80) mods |= 1;
    if (q->keystate[VK_SHIFT] & 0x80) mods |= 2;
    if (q->keystate[VK_CONTROL] & 0x80) mods |= 4;
    code = (mods << 8) | (q->vk & 0xFF);
    for (i = 0; i < (int)(sizeof(kDeadHotkeyIdx) / sizeof(kDeadHotkeyIdx[0])); i++)
        if (kDeadHotkeyIdx[i] < g_nHotkeys && g_hotkeys[kDeadHotkeyIdx[i]] == code) return TRUE;
    return FALSE;
}

static void engine_process(const HANIN_REQ *q, HANIN_RESP *r)
{
    BOOL up = (q->flags & HREQ_UP) != 0;
    memset(r, 0, sizeof(*r));
    r->magic = HANIN_MAGIC;
    if (q->magic != HANIN_MAGIC || q->version != HANIN_PROTO_VER) { r->status = 1; return; }

    g_busyTick = GetTickCount();
    InterlockedExchange(&g_busy, 1);
    g_committedLen = 0; g_committed[0] = 0;

    /* another client (thread) starts typing while a composition is pending */
    if ((q->cmd == HCMD_TEST || q->cmd == HCMD_KEY) && q->clientTid != g_ownerTid) {
        if (g_ownerTid && !comp_empty()) { logv("owner change %lu -> %lu: reset", g_ownerTid, q->clientTid); do_reset(); }
        g_ownerTid = q->clientTid;
        g_lastTest.valid = FALSE;
    }

    if ((q->cmd == HCMD_TEST || q->cmd == HCMD_KEY) && is_dead_hotkey(q)) {
        logv("vk 0x%02x is a hotkey for an unsupported floating window: passed to the app", q->vk);
        g_lastTest.valid = FALSE;
        snapshot(r);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    switch (q->cmd) {
    case HCMD_PING:
        break;
    case HCMD_LAUNCH:
        launch_companion(q->vk == HLAUNCH_QUERY ? "--app query " : "", "");
        break;
    case HCMD_TEST:
        memcpy(g_keyState, q->keystate, 256);
        if (up) g_keyState[q->vk & 0xFF] &= ~0x80; else g_keyState[q->vk & 0xFF] |= 0x80;
        r->eaten = do_process_key(q->vk, (LPARAM)q->lparam);
        g_lastTest.vk = q->vk; g_lastTest.up = up; g_lastTest.result = r->eaten; g_lastTest.valid = TRUE;
        logv("TEST vk=0x%02x%s -> %d", q->vk, up ? " up" : "", r->eaten);
        break;
    case HCMD_KEY:
        memcpy(g_keyState, q->keystate, 256);
        if (up) g_keyState[q->vk & 0xFF] &= ~0x80; else g_keyState[q->vk & 0xFF] |= 0x80;
        if (g_lastTest.valid && g_lastTest.vk == q->vk && g_lastTest.up == up) r->eaten = g_lastTest.result;
        else r->eaten = do_process_key(q->vk, (LPARAM)q->lparam);
        g_lastTest.valid = FALSE;
        if (r->eaten) do_to_ascii(q->vk, (LPARAM)q->lparam, up);
        pump();
        logv("KEY vk=0x%02x%s -> eaten=%d", q->vk, up ? " up" : "", r->eaten);
        break;
    case HCMD_RESET:
        do_reset();
        g_lastTest.valid = FALSE;
        break;
    case HCMD_COMPLETE: {
        /* NOT NotifyIME(CPS_COMPLETE): Hanin implements that with a global
         * keybd_event(VK_RETURN). Feed ESC (close selection window) / ENTER
         * (commit) through the engine instead. */
        int i;
        for (i = 0; i < 4; i++) {
            FINDCAND fc = { NULL };
            UINT vk;
            EnumThreadWindows(GetCurrentThreadId(), find_cand_proc, (LPARAM)&fc);
            if (fc.found) vk = VK_ESCAPE;
            else if (!comp_empty()) vk = VK_RETURN;
            else break;
            memset(g_keyState, 0, 256);
            g_keyState[vk] = 0x80;
            if (do_process_key(vk, 1 | (MapVirtualKeyA(vk, 0) << 16)))
                do_to_ascii(vk, 1 | (MapVirtualKeyA(vk, 0) << 16), FALSE);
            g_keyState[vk] = 0;
            pump();
        }
        if (!comp_empty()) {
            char keep[1024]; int kl = g_committedLen;
            memcpy(keep, g_committed, kl + 1);
            do_reset();
            memcpy(g_committed, keep, kl + 1); g_committedLen = kl;
        }
        g_lastTest.valid = FALSE;
        break;
    }
    case HCMD_TOGGLE: {
        /* a lone Shift tap, exactly like the user pressing it */
        LPARAM lk = 1 | (MapVirtualKeyA(VK_SHIFT, 0) << 16);
        memset(g_keyState, 0, 256);
        g_keyState[VK_SHIFT] = g_keyState[VK_LSHIFT] = 0x80;
        if (do_process_key(VK_SHIFT, lk)) do_to_ascii(VK_SHIFT, lk, FALSE);
        g_keyState[VK_SHIFT] = g_keyState[VK_LSHIFT] = 0;
        if (do_process_key(VK_SHIFT, lk | 0xC0000000)) do_to_ascii(VK_SHIFT, lk | 0xC0000000, TRUE);
        pump();
        g_lastTest.valid = FALSE;
        break;
    }
    default:
        r->status = 2;
    }
    snapshot(r);
    if (g_verbose && (q->cmd != HCMD_TEST)) {
        logv("  -> result='%s' comp='%s' cur=%u cand=%d", w8(r->result), w8(r->comp), r->compCursor, r->candOpen);
        if (r->candOpen) logv("  cand: %s", w8(r->cand));
    }
    InterlockedExchange(&g_busy, 0);
}

/* ======================================================================= */
/* server                                                                  */
/* ======================================================================= */
static LRESULT CALLBACK CtlWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_APP_REQ) { engine_process((const HANIN_REQ *)w, (HANIN_RESP *)l); return 0; }
    if (msg == WM_APP_GONE) {
        if ((DWORD)w == g_ownerTid && g_ownerTid) {
            HANIN_REQ q = {0}; HANIN_RESP r;
            q.magic = HANIN_MAGIC; q.version = HANIN_PROTO_VER; q.cmd = HCMD_RESET; q.clientTid = (DWORD)w;
            engine_process(&q, &r);
            g_ownerTid = 0;
        }
        return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

static DWORD WINAPI client_thread(LPVOID p)
{
    HANDLE pipe = (HANDLE)p;
    HANIN_REQ q;
    HANIN_RESP *r = (HANIN_RESP *)HeapAlloc(GetProcessHeap(), 0, sizeof(HANIN_RESP));
    DWORD n, lastTid = 0;
    for (;;) {
        if (!ReadFile(pipe, &q, sizeof(q), &n, NULL) || n != sizeof(q)) break;
        lastTid = q.clientTid;
        EnterCriticalSection(&g_reqLock);
        SendMessageA(g_hwndCtl, WM_APP_REQ, (WPARAM)&q, (LPARAM)r);
        LeaveCriticalSection(&g_reqLock);
        if (!WriteFile(pipe, r, sizeof(*r), &n, NULL)) break;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    if (lastTid) {
        EnterCriticalSection(&g_reqLock);
        SendMessageA(g_hwndCtl, WM_APP_GONE, lastTid, 0);
        LeaveCriticalSection(&g_reqLock);
    }
    HeapFree(GetProcessHeap(), 0, r);
    return 0;
}

static SECURITY_ATTRIBUTES *pipe_sa(void)
{
    static SECURITY_ATTRIBUTES sa;
    static PSECURITY_DESCRIPTOR sd;
    HANDLE tok;
    BYTE buf[256];
    DWORD n;
    LPWSTR sid = NULL;
    wchar_t sddl[512];
    if (sd) return &sa;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n))
            ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid);
        CloseHandle(tok);
    }
    /* SYSTEM + this user + AppContainers (Store apps, Start search), low-IL label */
    _snwprintf(sddl, 512, L"D:P(A;;GA;;;SY)(A;;GA;;;%s)(A;;GRGW;;;AC)S:(ML;;NW;;;LW)", sid ? sid : L"OW");
    if (sid) LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) {
        logf("!! SDDL failed %lu", GetLastError());
        return NULL;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

static wchar_t g_pipeName[128];

static DWORD WINAPI listener_thread(LPVOID first)
{
    HANDLE pipe = (HANDLE)first;
    for (;;) {
        BOOL ok = ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (ok) {
            HANDLE th = CreateThread(NULL, 0, client_thread, pipe, 0, NULL);
            if (th) CloseHandle(th); else CloseHandle(pipe);
        } else {
            CloseHandle(pipe);
        }
        pipe = CreateNamedPipeW(g_pipeName, PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, sizeof(HANIN_RESP), sizeof(HANIN_REQ), 0, pipe_sa());
        if (pipe == INVALID_HANDLE_VALUE) { logf("!! CreateNamedPipe failed %lu", GetLastError()); Sleep(1000); }
    }
}

static DWORD WINAPI watchdog_thread(LPVOID p)
{
    for (;;) {
        Sleep(500);
        if (g_busy && GetTickCount() - g_busyTick > 4000) fatal_restart("engine hung > 4s");
    }
}

/* ======================================================================= */
/* data directory                                                          */
/* ======================================================================= */
static void copy_tree(const char *src, const char *dst)
{
    char pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    CreateDirectoryA(dst, NULL);
    _snprintf(pat, MAX_PATH, "%s\\*", src);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf(s, MAX_PATH, "%s\\%s", src, fd.cFileName);
        _snprintf(d, MAX_PATH, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(s, d);
        else CopyFileA(s, d, TRUE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static char g_rescuedFrom[MAX_PATH];

static BOOL init_data_dir(const char *explicitDir)
{
    char exe[MAX_PATH], payload[MAX_PATH], probe[MAX_PATH], ini[MAX_PATH], orig[MAX_PATH], *slash;
    if (explicitDir) { GetFullPathNameA(explicitDir, MAX_PATH, g_dataDir, NULL); }
    else {
        char la[MAX_PATH];
        if (!GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) return FALSE;
        _snprintf(g_dataDir, MAX_PATH, "%s\\HaninRevival", la);
        CreateDirectoryA(g_dataDir, NULL);
        strcat(g_dataDir, "\\data");
        _snprintf(probe, MAX_PATH, "%s\\HANIN95.IME", g_dataDir);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            char virt[MAX_PATH];
            /* first run in the real profile: rescue settings + learned words that an
             * earlier, packaged (virtualised) host instance wrote, else start fresh */
            if (!hanin_in_package() && hanin_find_virtualized_data(virt, MAX_PATH)) {
                copy_tree(virt, g_dataDir);
                strcpy(g_rescuedFrom, virt);
            }
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            slash = strrchr(exe, '\\'); if (slash) *slash = 0;
            _snprintf(payload, MAX_PATH, "%s\\payload", exe);
            copy_tree(payload, g_dataDir);   /* fills anything missing; never overwrites */
        }
    }
    CreateDirectoryA(g_dataDir, NULL);
    _snprintf(probe, MAX_PATH, "%s\\USR_FILE", g_dataDir);
    CreateDirectoryA(probe, NULL);
    _snprintf(ini, MAX_PATH, "%s\\whanin.ini", g_dataDir);
    _snprintf(orig, MAX_PATH, "%s\\whanin.ini.orig", g_dataDir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) CopyFileA(orig, ini, TRUE);
    _snprintf(probe, MAX_PATH, "%s\\HANIN95.IME", g_dataDir);
    return GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES;
}

/* ======================================================================= */
/* test driver (same request path as the server)                           */
/* ======================================================================= */
static HANIN_RESP g_tr;

static int g_testCtrl;
static void test_req(UINT cmd, UINT vk, BOOL up, BOOL shift)
{
    HANIN_REQ q;
    memset(&q, 0, sizeof(q));
    q.magic = HANIN_MAGIC; q.version = HANIN_PROTO_VER; q.cmd = cmd;
    q.vk = vk; q.flags = up ? HREQ_UP : 0;
    q.lparam = 1 | (MapVirtualKeyA(vk, 0) << 16) | (up ? 0xC0000000 : 0);
    q.clientPid = GetCurrentProcessId(); q.clientTid = 4242;
    if (shift) q.keystate[VK_SHIFT] = q.keystate[VK_LSHIFT] = 0x80;
    if (g_testCtrl) q.keystate[VK_CONTROL] = q.keystate[VK_LCONTROL] = 0x80;
    engine_process(&q, &g_tr);
}

static void test_key(UINT vk, BOOL shift, char ch)
{
    test_req(HCMD_TEST, vk, FALSE, shift);
    if (g_tr.eaten) {
        test_req(HCMD_KEY, vk, FALSE, shift);
        if (g_tr.result[0]) printf("  COMMIT '%s'\n", w8(g_tr.result));
    } else {
        printf("  (key 0x%02x not eaten -> app)\n", vk);
    }
    test_req(HCMD_TEST, vk, TRUE, shift);
    if (g_tr.eaten) test_req(HCMD_KEY, vk, TRUE, shift);
    printf("[%c] method=%d conv=0x%x open=%u comp='%s' cur=%u%s\n", ch ? ch : '?', (int)g_tr.method, g_tr.convMode, g_tr.open, w8(g_tr.comp), g_tr.compCursor, g_tr.candOpen ? " +CAND" : "");
    if (g_tr.candOpen) printf("%s", w8(g_tr.cand));
    if (g_tr.result[0]) printf("  COMMIT(up) '%s'\n", w8(g_tr.result));
}

static BYTE *g_memSnap; static DWORD g_memVa, g_memSize;
static void mem_section(void)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g_ime + ((IMAGE_DOS_HEADER *)g_ime)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    int i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sh[i].Name, ".data", 5)) { g_memVa = sh[i].VirtualAddress; g_memSize = sh[i].Misc.VirtualSize; }
}
static void mem_snap(void)
{
    if (!g_memVa) mem_section();
    if (!g_memSnap) g_memSnap = (BYTE *)malloc(g_memSize);
    memcpy(g_memSnap, (BYTE *)g_ime + g_memVa, g_memSize);
}
static void mem_diff(void)
{
    BYTE *cur = (BYTE *)g_ime + g_memVa;
    DWORD i; int n = 0;
    for (i = 0; i < g_memSize && n < 60; i++) if (!(((g_memVa + i) & 0x1f) == 0x0d || ((g_memVa + i) & 0x1f) == 0x0e || ((g_memVa + i) & 0x1f) == 0x14))
        if (cur[i] != g_memSnap[i]) { printf("  diff RVA 0x%05lx: %02x -> %02x\n", g_memVa + i, g_memSnap[i], cur[i]); n++; }
    printf("  (%d diffs)\n", n);
}

static void run_script(const char *s)
{
    while (*s) {
        if (*s == '{') {
            const char *e = strchr(s, '}');
            char tok[32]; int len;
            if (!e) break;
            len = (int)(e - s - 1); if (len > 31) len = 31;
            memcpy(tok, s + 1, len); tok[len] = 0;
            s = e + 1;
            if (!_stricmp(tok, "ENTER")) test_key(VK_RETURN, FALSE, 'E');
            else if (!_stricmp(tok, "ESC")) test_key(VK_ESCAPE, FALSE, 'X');
            else if (!_stricmp(tok, "SPACE")) test_key(VK_SPACE, FALSE, '_');
            else if (!_stricmp(tok, "BS")) test_key(VK_BACK, FALSE, 'B');
            else if (!_stricmp(tok, "DOWN")) test_key(VK_DOWN, FALSE, 'v');
            else if (!_stricmp(tok, "UP")) test_key(VK_UP, FALSE, '^');
            else if (!_stricmp(tok, "LEFT")) test_key(VK_LEFT, FALSE, '<');
            else if (!_stricmp(tok, "RIGHT")) test_key(VK_RIGHT, FALSE, '>');
            else if (!_stricmp(tok, "PGDN")) test_key(VK_NEXT, FALSE, 'N');
            else if (!_stricmp(tok, "PGUP")) test_key(VK_PRIOR, FALSE, 'P');
            else if (!_stricmp(tok, "SHIFT")) test_key(VK_SHIFT, FALSE, 'S');
            else if (tok[0] == '^' && tok[1]) {   /* {^x}: Ctrl + key */
                SHORT r = VkKeyScanA(tok[1]);
                g_testCtrl = 1;
                test_key(r & 0xFF, (r >> 8) & 1, tok[1]);
                g_testCtrl = 0;
            }
            else if (!_stricmp(tok, "S-ENTER")) test_key(VK_RETURN, TRUE, 'R');
            else if (!_stricmp(tok, "S-DEL")) test_key(VK_DELETE, TRUE, 'D');
            else if (!_stricmp(tok, "TAB")) test_key(VK_TAB, FALSE, 'T');
            else if (!_stricmp(tok, "MEMSNAP")) mem_snap();
            else if (!_stricmp(tok, "MEMDIFF")) mem_diff();
            else if (!_stricmp(tok, "TOGGLE")) { test_req(HCMD_TOGGLE, 0, FALSE, FALSE); printf("[TOGGLE] method=%d\n", (int)g_tr.method); }
            else if (!_stricmp(tok, "RESET")) { test_req(HCMD_RESET, 0, FALSE, FALSE); printf("[RESET] comp='%s' cand=%d\n", w8(g_tr.comp), g_tr.candOpen); }
            else if (!_stricmp(tok, "COMPLETE")) { test_req(HCMD_COMPLETE, 0, FALSE, FALSE); printf("[COMPLETE] result='%s' comp='%s'\n", w8(g_tr.result), w8(g_tr.comp)); }
            else printf("?? unknown token {%s}\n", tok);
        } else {
            SHORT r = VkKeyScanA(*s);
            test_key(r & 0xFF, (r >> 8) & 1, *s);
            s++;
        }
    }
}

/* ======================================================================= */
/* --export-icon: copy the original 漢音 icon out of the user's HANIN95.IME   */
/* ======================================================================= */
/* The TIP's profile icon and language-bar icon used to point straight at
 * payload\HANIN95.IME, so every process that shows input-method icons
 * (Settings, the rundll32-hosted "Text Services" dialog, ...) opened the
 * 1997 IME binary. Microsoft Defender flagged that access pattern as
 * Trojan:Win32/PShellDown.SB (a false positive: a static scan of the same,
 * SHA-verified file is clean). The installer now runs this once so that only
 * hanin_host / hanin_tools ever touch the IME itself. */
#pragma pack(push, 2)
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes; WORD id; } GRPICONENTRY;
typedef struct { WORD res, type, count; } ICONHDR;
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes, offset; } ICONFILEENTRY;
#pragma pack(pop)

typedef struct { int want, seen; LPWSTR name; WCHAR buf[64]; } GROUPPICK;
static BOOL CALLBACK pick_group(HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR p)
{
    GROUPPICK *g = (GROUPPICK *)p;
    (void)m; (void)type;
    if (g->seen++ != g->want) return TRUE;
    if (IS_INTRESOURCE(name)) g->name = name;
    else { lstrcpynW(g->buf, name, 64); g->name = g->buf; }
    return FALSE;
}

static int export_icon(const char *ime, const char *out, int index)
{
    HMODULE m = LoadLibraryExA(ime, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    GROUPPICK g = { index, 0, NULL };
    HRSRC r; const BYTE *grp; const ICONHDR *gh; const GRPICONENTRY *ge;
    ICONHDR fh; ICONFILEENTRY fe; DWORD off, wr; HANDLE f; int k, rc = 0;
    if (!m) return 10;
    EnumResourceNamesW(m, (LPCWSTR)RT_GROUP_ICON, pick_group, (LONG_PTR)&g);
    if (!g.name) { FreeLibrary(m); return 11; }
    r = FindResourceW(m, g.name, (LPCWSTR)RT_GROUP_ICON);
    grp = r ? (const BYTE *)LockResource(LoadResource(m, r)) : NULL;
    if (!grp) { FreeLibrary(m); return 12; }
    gh = (const ICONHDR *)grp;
    ge = (const GRPICONENTRY *)(grp + sizeof(ICONHDR));

    f = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { FreeLibrary(m); return 13; }
    fh = *gh;
    WriteFile(f, &fh, sizeof fh, &wr, NULL);
    off = sizeof(ICONHDR) + gh->count * sizeof(ICONFILEENTRY);
    for (k = 0; k < gh->count; k++) {      /* directory */
        HRSRC ri = FindResourceW(m, MAKEINTRESOURCEW(ge[k].id), (LPCWSTR)RT_ICON);
        DWORD sz = ri ? SizeofResource(m, ri) : 0;
        memcpy(&fe, &ge[k], 12);           /* w,h,colors,res,planes,bpp,bytes */
        fe.bytes = sz;
        fe.offset = off;
        off += sz;
        WriteFile(f, &fe, sizeof fe, &wr, NULL);
        if (!sz) rc = 14;
    }
    for (k = 0; k < gh->count; k++) {      /* images */
        HRSRC ri = FindResourceW(m, MAKEINTRESOURCEW(ge[k].id), (LPCWSTR)RT_ICON);
        if (ri) WriteFile(f, LockResource(LoadResource(m, ri)), SizeofResource(m, ri), &wr, NULL);
    }
    CloseHandle(f);
    FreeLibrary(m);
    if (rc) DeleteFileA(out);
    return rc;
}

/* ======================================================================= */
/* main                                                                    */
/* ======================================================================= */
int main(int argc, char **argv)
{
    const char *dataArg = NULL, *script = NULL;
    int i;
    /* hanin_host --export-icon <HANIN95.IME> <out.ico>   (run by the installer) */
    if (argc == 4 && !strcmp(argv[1], "--export-icon"))
        return export_icon(argv[2], argv[3], 1);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) dataArg = argv[++i];
        else if (!strcmp(argv[i], "--test") && i + 2 < argc) { g_testMode = 1; dataArg = argv[++i]; script = argv[++i]; }
        else if (!strcmp(argv[i], "--server")) {}
    }
    /* never run the server inside an MSIX container: its profile writes would be
     * virtualised into that app's private folder (see outside_pkg.h) */
    if (!g_testMode && !dataArg && hanin_in_package()) {
        if (!getenv("HANIN_RELAUNCHED")) {
            char dir[MAX_PATH], *sl;
            GetModuleFileNameA(NULL, dir, MAX_PATH);
            sl = strrchr(dir, '\\'); if (sl) *sl = 0;
            SetEnvironmentVariableA("HANIN_RELAUNCHED", "1");
            if (hanin_relaunch_outside_package(dir)) return 0;
        }
    }
    if (!init_data_dir(dataArg)) { MessageBoxA(NULL, "Hanin data directory missing", "hanin_host", MB_OK); return 1; }
    if (getenv("HANIN_VERBOSE")) g_verbose = 1;
    open_log();
    logf("=== hanin_host start pid=%lu data=%s mode=%s verbose=%d packaged=%d", GetCurrentProcessId(), g_dataDir,
         g_testMode ? "test" : "server", g_verbose, hanin_in_package());
    if (g_rescuedFrom[0]) logf("rescued settings + user dictionary from virtualised copy %s", g_rescuedFrom);
    if (!g_testMode && hanin_in_package()) logf("WARNING: still inside an app package; data writes are virtualised");

    if (g_testMode) {
        if (!engine_init()) return 2;
        run_script(script);
        logf("=== done");
        return 0;
    }

    /* single instance per session: first pipe instance wins */
    {
        DWORD sess = 0;
        HANDLE first;
        WNDCLASSA wc = {0};
        MSG m;
        ProcessIdToSessionId(GetCurrentProcessId(), &sess);
        _snwprintf(g_pipeName, 128, HANIN_PIPE_FMT, sess);
        first = CreateNamedPipeW(g_pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 PIPE_UNLIMITED_INSTANCES, sizeof(HANIN_RESP), sizeof(HANIN_REQ), 0, pipe_sa());
        if (first == INVALID_HANDLE_VALUE) {
            logf("another host already owns the pipe (err %lu) - exiting", GetLastError());
            return 0;
        }
        InitializeCriticalSection(&g_reqLock);
        if (!engine_init()) { logf("!! engine init failed"); return 2; }

        wc.lpfnWndProc = CtlWndProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "HaninHostCtl";
        RegisterClassA(&wc);
        g_hwndCtl = CreateWindowExA(0, "HaninHostCtl", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

        CloseHandle(CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, listener_thread, first, 0, NULL));
        logf("listening on %s", w8(g_pipeName));

        while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    logf("=== exit");
    return 0;
}
