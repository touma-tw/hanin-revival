// HaninTip.cpp - TSF text input processor front-end for the revived 漢音 engine.
//
// This DLL is loaded into every application (x64 and x86 builds). It owns no
// conversion logic: keys are forwarded over a named pipe to hanin_host.exe
// (32-bit, runs the original HANIN95.IME engine), and the returned composition,
// committed text and candidate-grid snapshot are applied through TSF.
//
// Design rules:
//  * never block the app for long: every pipe call has a hard timeout and a
//    back-off, after which keys simply pass through to the application;
//  * the engine runs out-of-process, so an engine crash can't take the app down.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <msctf.h>
#include <olectl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <wchar.h>
#include <new>
#include "../common/protocol.h"
#include "resource.h"

// ---------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------
// {615F8932-792A-4588-8635-332CF2B7EC61}
static const CLSID CLSID_HaninTip = { 0x615f8932, 0x792a, 0x4588, { 0x86, 0x35, 0x33, 0x2c, 0xf2, 0xb7, 0xec, 0x61 } };
// {0A18306E-52DC-4413-BF02-07B97906B415}
static const GUID GUID_HaninProfile = { 0x0a18306e, 0x52dc, 0x4413, { 0xbf, 0x02, 0x07, 0xb9, 0x79, 0x06, 0xb4, 0x15 } };
// {4DE6FC38-80A8-4F2A-A93C-1D91C51E3D22}
static const GUID GUID_HaninDisplayAttrInput = { 0x4de6fc38, 0x80a8, 0x4f2a, { 0xa9, 0x3c, 0x1d, 0x91, 0xc5, 0x1e, 0x3d, 0x22 } };

static const wchar_t kTipName[] = L"漢音輸入法";
static const LANGID kLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL); // 0x0404

static HINSTANCE g_hInst;
static LONG g_cRefDll;

// ---------------------------------------------------------------------------
// diagnostics (errors only; never keystrokes)
// ---------------------------------------------------------------------------
static void TipLog(const wchar_t *fmt, ...)
{
    wchar_t msg[512], line[700], path[MAX_PATH], exe[MAX_PATH];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(msg, 512, fmt, ap);
    va_end(ap);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    const wchar_t *base = wcsrchr(exe, L'\\');
    SYSTEMTIME st;
    GetLocalTime(&st);
    StringCchPrintfW(line, 700, L"%04d-%02d-%02d %02d:%02d:%02d.%03d [%s:%lu/%lu] %s\r\n", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, base ? base + 1 : exe,
                     GetCurrentProcessId(), GetCurrentThreadId(), msg);
    OutputDebugStringW(line);
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return;
    StringCchCatW(path, MAX_PATH, L"\\HaninRevival\\tip.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char u8[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
    DWORD w;
    if (n > 1) WriteFile(h, u8, n - 1, &w, NULL);
    CloseHandle(h);
}

static int g_debug = -1;
#define TIPDBG(...) do { if (g_debug < 0) { wchar_t _e[8]; g_debug = GetEnvironmentVariableW(L"HANIN_TIP_DEBUG", _e, 8) ? 1 : 0; } if (g_debug) TipLog(__VA_ARGS__); } while (0)

static void ModuleDir(wchar_t *dir);

// Original 漢音 icon, exported at install time from icon group #2 of the user's
// own HANIN95.IME (hanin_host --export-icon). Never point Windows or the app
// processes at the IME binary itself: Defender flagged rundll32 (the "Text
// Services" dialog) reading it as Trojan:Win32/PShellDown.SB and quarantined it.
static bool OriginalIconFile(wchar_t *path)
{
    wchar_t dir[MAX_PATH];
    ModuleDir(dir);
    StringCchPrintfW(path, MAX_PATH, L"%s\\payload\\hanin.ico", dir);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
static const int kOriginalIconIndex = 0;

static void ModuleDir(wchar_t *dir)
{
    GetModuleFileNameW(g_hInst, dir, MAX_PATH);
    wchar_t *s = wcsrchr(dir, L'\\');
    if (s) *s = 0;
}

// ---------------------------------------------------------------------------
// engine client (named pipe, overlapped, hard timeouts)
// ---------------------------------------------------------------------------
class Engine {
public:
    HANIN_RESP resp;

    Engine() { m_evt = CreateEventW(NULL, TRUE, FALSE, NULL); ZeroMemory(&resp, sizeof(resp)); }
    ~Engine() { Close(); if (m_evt) CloseHandle(m_evt); }

    bool Call(UINT cmd, UINT vk = 0, LPARAM lp = 0, UINT flags = 0)
    {
        HANIN_REQ q;
        ZeroMemory(&q, sizeof(q));
        q.magic = HANIN_MAGIC;
        q.version = HANIN_PROTO_VER;
        q.cmd = cmd;
        q.flags = flags;
        q.vk = vk;
        q.lparam = (uint32_t)lp;
        q.clientPid = GetCurrentProcessId();
        q.clientTid = GetCurrentThreadId();
        GetKeyboardState(q.keystate);
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!Connect()) return false;
            DWORD n = 0;
            if (!Io(true, &q, sizeof(q), &n, 800) || n != sizeof(q)) { Fail(L"write"); continue; }
            if (!Io(false, &resp, sizeof(resp), &n, 1500) || n != sizeof(resp)) { Fail(L"read"); m_failUntil = GetTickCount() + 1500; return false; }
            if (resp.magic != HANIN_MAGIC || resp.status != 0) { TipLog(L"engine status %u", resp.status); return false; }
            return true;
        }
        return false;
    }

    bool Connected() const { return m_pipe != INVALID_HANDLE_VALUE; }

    void Close()
    {
        if (m_pipe != INVALID_HANDLE_VALUE) { CloseHandle(m_pipe); m_pipe = INVALID_HANDLE_VALUE; }
    }

private:
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_evt = NULL;
    DWORD m_failUntil = 0;
    DWORD m_lastLaunch = 0;

    void Fail(const wchar_t *what)
    {
        TipLog(L"pipe %s failed (err %lu)", what, GetLastError());
        Close();
    }

    bool Io(bool write, void *buf, DWORD len, DWORD *got, DWORD timeout)
    {
        OVERLAPPED ov = {};
        ov.hEvent = m_evt;
        ResetEvent(m_evt);
        BOOL ok = write ? WriteFile(m_pipe, buf, len, NULL, &ov) : ReadFile(m_pipe, buf, len, NULL, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(m_evt, timeout) != WAIT_OBJECT_0) {
            CancelIoEx(m_pipe, &ov);
            GetOverlappedResult(m_pipe, &ov, got, TRUE);
            SetLastError(WAIT_TIMEOUT);
            return false;
        }
        return GetOverlappedResult(m_pipe, &ov, got, FALSE) != FALSE;
    }

    static bool IsRestrictedProcess()
    {
        HANDLE tok;
        bool restricted = false;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return true;
        DWORD isAc = 0, n;
        if (GetTokenInformation(tok, TokenIsAppContainer, &isAc, sizeof(isAc), &n) && isAc) restricted = true;
        BYTE buf[64];
        if (!restricted && GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &n)) {
            PSID sid = ((TOKEN_MANDATORY_LABEL *)buf)->Label.Sid;
            DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
            if (rid < SECURITY_MANDATORY_MEDIUM_RID) restricted = true;
        }
        CloseHandle(tok);
        return restricted;
    }

    void LaunchHost()
    {
        DWORD now = GetTickCount();
        if (now - m_lastLaunch < 5000 && m_lastLaunch) return;
        m_lastLaunch = now;
        if (IsRestrictedProcess()) return;
        wchar_t dir[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 32];
        ModuleDir(dir);
        StringCchPrintfW(exe, MAX_PATH, L"%s\\hanin_host.exe", dir);
        StringCchPrintfW(cmd, MAX_PATH + 32, L"\"%s\" --server", exe);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW, NULL, dir, &si, &pi);
        if (!ok) ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi);
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); TipLog(L"launched host pid %lu", pi.dwProcessId); }
        else TipLog(L"cannot launch %s (err %lu)", exe, GetLastError());
    }

    bool Connect()
    {
        if (m_pipe != INVALID_HANDLE_VALUE) return true;
        if (m_failUntil && (LONG)(GetTickCount() - m_failUntil) < 0) return false;
        DWORD sess = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sess);
        wchar_t name[128];
        StringCchPrintfW(name, 128, HANIN_PIPE_FMT, sess);
        DWORD start = GetTickCount();
        bool launched = false;
        for (;;) {
            m_pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (m_pipe != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND && !launched) { LaunchHost(); launched = true; }
            if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 200);
            else Sleep(50);
            if (GetTickCount() - start > (launched ? 2500u : 300u) || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY)) {
                TipLog(L"connect failed (err %lu)", err);
                m_failUntil = GetTickCount() + 3000;
                return false;
            }
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(m_pipe, &mode, NULL, NULL);
        m_failUntil = 0;
        return true;
    }
};

// ---------------------------------------------------------------------------
// candidate window (DPI-aware redraw of the engine's selection grid)
// ---------------------------------------------------------------------------
class CandWindow {
public:
    void Update(const wchar_t *model, const RECT &anchor)
    {
        StringCchCopyW(m_model, HANIN_MAX_CAND, model);
        if (!m_hwnd) Create();
        if (!m_hwnd) return;
        SIZE sz = Measure();
        // place below the anchor, flip above if it does not fit
        HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        int x = anchor.left, y = anchor.bottom + 2;
        if (y + sz.cy > mi.rcWork.bottom) y = anchor.top - sz.cy - 2;
        if (x + sz.cx > mi.rcWork.right) x = mi.rcWork.right - sz.cx;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, sz.cx, sz.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(m_hwnd, NULL, TRUE);
        UpdateWindow(m_hwnd);
    }
    void Hide() { if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE); }
    void Destroy() { if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = NULL; } if (m_font) { DeleteObject(m_font); m_font = NULL; } if (m_small) { DeleteObject(m_small); m_small = NULL; } }
    ~CandWindow() { Destroy(); }

private:
    HWND m_hwnd = NULL;
    HFONT m_font = NULL, m_small = NULL;
    UINT m_dpi = 0;
    wchar_t m_model[HANIN_MAX_CAND] = {};

    static const wchar_t *ClassName() { return L"HaninRevivalCand"; }

    void Create()
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        if (!GetClassInfoExW(g_hInst, ClassName(), &wc)) {
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_IME | CS_DROPSHADOW;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.lpszClassName = ClassName();
            RegisterClassExW(&wc);
        }
        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName(), L"", WS_POPUP | WS_BORDER,
                                 0, 0, 10, 10, NULL, NULL, g_hInst, this);
        if (!m_hwnd) TipLog(L"candidate window creation failed (err %lu)", GetLastError());
    }

    void EnsureFonts()
    {
        typedef UINT(WINAPI * PFN)(HWND);
        static PFN pGetDpiForWindow = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        UINT dpi = pGetDpiForWindow ? pGetDpiForWindow(m_hwnd) : 96;
        if (!dpi) dpi = 96;
        if (dpi == m_dpi && m_font) return;
        m_dpi = dpi;
        if (m_font) DeleteObject(m_font);
        if (m_small) DeleteObject(m_small);
        m_font = CreateFontW(-MulDiv(18, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        m_small = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
    }

    int S(int v) const { return MulDiv(v, m_dpi ? m_dpi : 96, 96); }

    // model iteration ------------------------------------------------------
    struct Row { wchar_t kind; wchar_t fields[16][48]; int n; };
    int Parse(Row *rows, int max)
    {
        int count = 0;
        const wchar_t *p = m_model;
        while (*p && count < max) {
            const wchar_t *e = wcschr(p, L'\n');
            if (!e) e = p + wcslen(p);
            Row &r = rows[count];
            r.kind = *p; r.n = 0;
            const wchar_t *f = p + 1;
            while (f < e && r.n < 16) {
                if (*f == L'\t') f++;
                const wchar_t *fe = f;
                while (fe < e && *fe != L'\t') fe++;
                int len = (int)min(fe - f, 47);
                wmemcpy(r.fields[r.n], f, len);
                r.fields[r.n][len] = 0;
                r.n++;
                f = fe;
            }
            count++;
            p = *e ? e + 1 : e;
        }
        return count;
    }

    SIZE Measure()
    {
        EnsureFonts();
        Row rows[12];
        int n = Parse(rows, 12);
        HDC dc = GetDC(m_hwnd);
        HGDIOBJ old = SelectObject(dc, m_font);
        int cell = 0, cols = 0, nrows = 0;
        SIZE s;
        for (int i = 0; i < n; i++) {
            if (rows[i].kind != L'R') continue;
            nrows++;
            cols = max(cols, rows[i].n - 2);
            for (int j = 2; j < rows[i].n; j++) {
                GetTextExtentPoint32W(dc, rows[i].fields[j], (int)wcslen(rows[i].fields[j]), &s);
                cell = max(cell, (int)s.cx);
            }
        }
        GetTextExtentPoint32W(dc, L"漢", 1, &s);
        m_lineH = s.cy + S(6);
        m_cellW = cell + S(22);
        m_labelW = S(24);
        SelectObject(dc, m_small);
        GetTextExtentPoint32W(dc, L"(000 / 000)", 11, &s);
        m_smallH = s.cy + S(4);
        SelectObject(dc, old);
        ReleaseDC(m_hwnd, dc);
        SIZE out;
        out.cx = S(8) + m_labelW + max(cols, 1) * m_cellW + S(8);
        out.cx = max(out.cx, (LONG)(s.cx + S(120)));
        out.cy = S(4) + m_smallH + nrows * m_lineH + S(6);
        return out;
    }

    int m_lineH = 0, m_cellW = 0, m_labelW = 0, m_smallH = 0;

    void Paint(HDC dc)
    {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        Row rows[12];
        int n = Parse(rows, 12);
        const wchar_t *title = L"", *footer = L"";
        for (int i = 0; i < n; i++) {
            if (rows[i].kind == L'T' && rows[i].n) title = rows[i].fields[0];
            if (rows[i].kind == L'F' && rows[i].n) footer = rows[i].fields[0];
        }
        // header: title left, page indicator right
        HGDIOBJ old = SelectObject(dc, m_small);
        SetTextColor(dc, RGB(110, 110, 110));
        RECT hr = { S(8), S(3), rc.right - S(8), S(3) + m_smallH };
        DrawTextW(dc, title, -1, &hr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextW(dc, footer, -1, &hr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        int y = S(4) + m_smallH;
        for (int i = 0; i < n; i++) {
            if (rows[i].kind != L'R') continue;
            bool active = rows[i].n > 1 && rows[i].fields[1][0] == L'1';
            RECT rr = { S(3), y, rc.right - S(3), y + m_lineH };
            if (active) {
                HBRUSH hb = CreateSolidBrush(RGB(218, 234, 255));
                FillRect(dc, &rr, hb);
                DeleteObject(hb);
            }
            // row label (A..E)
            SelectObject(dc, m_small);
            SetTextColor(dc, RGB(140, 140, 140));
            RECT lr = { S(8), y, S(8) + m_labelW, y + m_lineH };
            DrawTextW(dc, rows[i].fields[0], -1, &lr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            for (int j = 2; j < rows[i].n; j++) {
                int x = S(8) + m_labelW + (j - 2) * m_cellW;
                if (active) {
                    wchar_t d[3] = { (wchar_t)(L'1' + (j - 2)), 0 };
                    SelectObject(dc, m_small);
                    SetTextColor(dc, RGB(30, 100, 200));
                    RECT dr = { x, y, x + S(14), y + m_lineH };
                    DrawTextW(dc, d, -1, &dr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                }
                SelectObject(dc, m_font);
                SetTextColor(dc, RGB(20, 20, 20));
                RECT tr = { x + S(14), y, x + m_cellW, y + m_lineH };
                DrawTextW(dc, rows[i].fields[j], -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            y += m_lineH;
        }
        SelectObject(dc, old);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
    {
        CandWindow *self = (CandWindow *)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (self) {
                RECT rc; GetClientRect(h, &rc);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                HGDIOBJ ob = SelectObject(mem, bmp);
                self->Paint(mem);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, ob);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DPICHANGED:
            return 0;
        }
        return DefWindowProcW(h, msg, w, l);
    }
};

// ---------------------------------------------------------------------------
// edit session helper
// ---------------------------------------------------------------------------
template <class F>
class EditSession : public ITfEditSession {
public:
    explicit EditSession(F f) : m_f(f) {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) { *ppv = (ITfEditSession *)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    STDMETHODIMP DoEditSession(TfEditCookie ec) { return m_f(ec); }
private:
    F m_f;
    LONG m_ref = 1;
};

template <class F>
static HRESULT RunEditSession(ITfContext *pic, TfClientId tid, DWORD flags, F f)
{
    EditSession<F> *es = new (std::nothrow) EditSession<F>(f);
    if (!es) return E_OUTOFMEMORY;
    HRESULT hrSession = E_FAIL;
    HRESULT hr = pic->RequestEditSession(tid, es, flags, &hrSession);
    es->Release();
    return SUCCEEDED(hr) ? hrSession : hr;
}

// ---------------------------------------------------------------------------
// display attribute (dotted underline under the composition)
// ---------------------------------------------------------------------------
class DisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) { *ppv = (ITfDisplayAttributeInfo *)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (!r) { delete this; InterlockedDecrement(&g_cRefDll); } return r; }
    DisplayAttributeInfo() { InterlockedIncrement(&g_cRefDll); }
    STDMETHODIMP GetGUID(GUID *pguid) { *pguid = GUID_HaninDisplayAttrInput; return S_OK; }
    STDMETHODIMP GetDescription(BSTR *pbstr) { *pbstr = SysAllocString(L"漢音組字"); return *pbstr ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE *p)
    {
        ZeroMemory(p, sizeof(*p));
        p->crText.type = TF_CT_NONE;
        p->crBk.type = TF_CT_NONE;
        p->lsStyle = TF_LS_DOT;
        p->fBoldLine = FALSE;
        p->crLine.type = TF_CT_NONE;
        p->bAttr = TF_ATTR_INPUT;
        return S_OK;
    }
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE *) { return E_NOTIMPL; }
    STDMETHODIMP Reset() { return S_OK; }
private:
    LONG m_ref = 1;
};

class EnumDisplayAttributeInfo : public IEnumTfDisplayAttributeInfo {
public:
    EnumDisplayAttributeInfo() { InterlockedIncrement(&g_cRefDll); }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) { *ppv = (IEnumTfDisplayAttributeInfo *)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (!r) { delete this; InterlockedDecrement(&g_cRefDll); } return r; }
    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo **pp)
    {
        EnumDisplayAttributeInfo *e = new (std::nothrow) EnumDisplayAttributeInfo();
        if (!e) return E_OUTOFMEMORY;
        e->m_pos = m_pos;
        *pp = e;
        return S_OK;
    }
    STDMETHODIMP Next(ULONG c, ITfDisplayAttributeInfo **rg, ULONG *fetched)
    {
        ULONG n = 0;
        while (n < c && m_pos < 1) {
            rg[n] = new (std::nothrow) DisplayAttributeInfo();
            if (!rg[n]) break;
            n++; m_pos++;
        }
        if (fetched) *fetched = n;
        return n == c ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Skip(ULONG c) { m_pos += c; return m_pos <= 1 ? S_OK : S_FALSE; }
private:
    LONG m_ref = 1;
    ULONG m_pos = 0;
};

// ---------------------------------------------------------------------------
// language bar / taskbar input-mode button: icon 中/英, left click toggles,
// right click opens a menu (toolbox, text query, toggle)
// ---------------------------------------------------------------------------
// {2C77A81E-41CC-4178-A3A7-5F8A987568E6} - declared but not defined by the SDK
static const GUID GUID_LBI_INPUTMODE_ = { 0x2c77a81e, 0x41cc, 0x4178, { 0xa3, 0xa7, 0x5f, 0x8a, 0x98, 0x75, 0x68, 0xe6 } };

enum { LBMENU_TOOLBOX = 1, LBMENU_QUERY = 2, LBMENU_TOGGLE = 3 };
// {18EB4019-FB75-4C2D-83BA-F938D2F0AADE} 設定/工具選單 on the floating language bar
static const GUID GUID_HaninLbiMenu = { 0x18eb4019, 0xfb75, 0x4c2d, { 0x83, 0xba, 0xf9, 0x38, 0xd2, 0xf0, 0xaa, 0xde } };
// {09D22BC6-2245-4DBC-B460-3B61DC478755} 中文/英文切換 on the floating language bar
static const GUID GUID_HaninLbiMode = { 0x09d22bc6, 0x2245, 0x4dbc, { 0xb4, 0x60, 0x3b, 0x61, 0xdc, 0x47, 0x87, 0x55 } };

// INDICATOR: GUID_LBI_INPUTMODE, shown by Windows in the taskbar input indicator
// MODE:      中文/英文切換 button on the floating (desktop) language bar
// MENU:      設定/工具選單 button on the floating language bar
enum LbKind { LB_INDICATOR, LB_MODE, LB_MENU };

struct LangBarHost {
    virtual void LbToggle() = 0;
    virtual void LbMenu(UINT id) = 0;
};

class LangBarButton : public ITfLangBarItemButton, public ITfSource {
public:
    LangBarButton(LangBarHost *host, LbKind kind) : m_host(host), m_kind(kind) { InterlockedIncrement(&g_cRefDll); }
    ~LangBarButton() { if (m_sink) m_sink->Release(); InterlockedDecrement(&g_cRefDll); }
    void Detach() { m_host = NULL; }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) *ppv = (ITfLangBarItemButton *)this;
        else if (riid == IID_ITfSource) *ppv = (ITfSource *)this;
        if (!*ppv) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO *p)
    {
        p->clsidService = CLSID_HaninTip;
        p->guidItem = m_kind == LB_MENU ? GUID_HaninLbiMenu : m_kind == LB_MODE ? GUID_HaninLbiMode : GUID_LBI_INPUTMODE_;
        p->dwStyle = (m_kind == LB_MENU ? TF_LBI_STYLE_BTN_MENU : TF_LBI_STYLE_BTN_BUTTON) | TF_LBI_STYLE_SHOWNINTRAY;
        p->ulSort = (ULONG)m_kind;
        StringCchCopyW(p->szDescription, TF_LBI_DESC_MAXLEN, m_kind == LB_MENU ? L"設定/工具選單" : L"中文/英文切換");
        return S_OK;
    }
    STDMETHODIMP GetStatus(DWORD *p) { *p = 0; return S_OK; }
    STDMETHODIMP Show(BOOL) { return E_NOTIMPL; }
    STDMETHODIMP GetTooltipString(BSTR *p)
    {
        *p = SysAllocString(m_kind == LB_MENU ? L"漢音設定/工具選單(工具箱、文字查詢)"
                            : m_zh ? L"漢音:中文(左鍵切換,右鍵開啟選單)" : L"漢音:英文(左鍵切換,右鍵開啟選單)");
        return *p ? S_OK : E_OUTOFMEMORY;
    }

    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT *)
    {
        if (!m_host) return S_OK;
        if (click == TF_LBI_CLK_LEFT && m_kind != LB_MENU) { m_host->LbToggle(); return S_OK; }
        UINT id = ShowMenu(pt);
        if (id && m_host) m_host->LbMenu(id);
        return S_OK;
    }
    STDMETHODIMP InitMenu(ITfMenu *menu)
    {
        struct { UINT id; DWORD flags; const wchar_t *text; } items[] = {
            { LBMENU_TOOLBOX, 0, L"漢音工具箱(屬性設定、自用辭庫)..." },
            { LBMENU_QUERY, 0, L"文字查詢..." },
            { 0, TF_LBMENUF_SEPARATOR, L"" },
            { LBMENU_TOGGLE, 0, m_zh ? L"切換為英文" : L"切換為中文" },
        };
        for (auto &it : items)
            menu->AddMenuItem(it.id, it.flags, NULL, NULL, it.text, (ULONG)wcslen(it.text), NULL);
        return S_OK;
    }
    STDMETHODIMP OnMenuSelect(UINT id) { if (m_host && id) m_host->LbMenu(id); return S_OK; }
    STDMETHODIMP GetIcon(HICON *p)
    {
        int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
        *p = NULL;
        if (m_kind == LB_MENU) {
            wchar_t ico[MAX_PATH];
            if (OriginalIconFile(ico)) *p = (HICON)LoadImageW(NULL, ico, IMAGE_ICON, cx, cy, LR_LOADFROMFILE);
        }
        if (!*p)
            *p = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(m_kind == LB_MENU ? IDI_HANIN : m_zh ? IDI_MODE_ZH : IDI_MODE_EN), IMAGE_ICON, cx, cy, 0);
        return *p ? S_OK : E_FAIL;
    }
    STDMETHODIMP GetText(BSTR *p)
    {
        *p = SysAllocString(m_kind == LB_MENU ? L"設定/工具選單" : m_kind == LB_MODE ? L"中文/英文切換" : m_zh ? L"中" : L"英");
        return *p ? S_OK : E_OUTOFMEMORY;
    }

    // ITfSource (ITfLangBarItemSink only)
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown *punk, DWORD *cookie)
    {
        if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
        if (m_sink) return CONNECT_E_ADVISELIMIT;
        if (FAILED(punk->QueryInterface(IID_ITfLangBarItemSink, (void **)&m_sink))) { m_sink = NULL; return E_NOINTERFACE; }
        *cookie = 0x4E414821;
        return S_OK;
    }
    STDMETHODIMP UnadviseSink(DWORD cookie)
    {
        if (cookie != 0x4E414821 || !m_sink) return CONNECT_E_NOCONNECTION;
        m_sink->Release();
        m_sink = NULL;
        return S_OK;
    }

    void SetChinese(bool zh)
    {
        if (zh == m_zh) return;
        m_zh = zh;
        if (m_sink && m_kind != LB_MENU) m_sink->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
    }

private:
    LONG m_ref = 1;
    LangBarHost *m_host;
    ITfLangBarItemSink *m_sink = NULL;
    bool m_zh = true;
    LbKind m_kind;

    UINT ShowMenu(POINT pt)
    {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, LBMENU_TOOLBOX, L"漢音工具箱(屬性設定、自用辭庫)...");
        AppendMenuW(m, MF_STRING, LBMENU_QUERY, L"文字查詢...");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, LBMENU_TOGGLE, m_zh ? L"切換為英文" : L"切換為中文");
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, pt.x, pt.y, 0, 0, NULL, NULL, g_hInst, NULL);
        if (owner) SetForegroundWindow(owner);   // lets the menu close when clicking elsewhere
        UINT id = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, owner, NULL);
        if (owner) DestroyWindow(owner);
        DestroyMenu(m);
        return id;
    }
};

static HRESULT CreateCategoryMgr(ITfCategoryMgr **pp)
{
    // TF_CreateCategoryMgr works without COM initialisation (TIPCAP_COMLESS)
    typedef HRESULT(WINAPI * PFN)(ITfCategoryMgr **);
    static PFN f = (PFN)GetProcAddress(GetModuleHandleW(L"msctf.dll"), "TF_CreateCategoryMgr");
    if (f && SUCCEEDED(f(pp))) return S_OK;
    return CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void **)pp);
}

// ---------------------------------------------------------------------------
// the text input processor
// ---------------------------------------------------------------------------
class HaninTip : public ITfTextInputProcessorEx,
                 public ITfThreadMgrEventSink,
                 public ITfKeyEventSink,
                 public ITfCompositionSink,
                 public ITfThreadFocusSink,
                 public ITfDisplayAttributeProvider,
                 public ITfCompartmentEventSink,
                 public LangBarHost {
public:
    HaninTip() { InterlockedIncrement(&g_cRefDll); }
    ~HaninTip() { InterlockedDecrement(&g_cRefDll); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor || riid == IID_ITfTextInputProcessorEx)
            *ppv = (ITfTextInputProcessorEx *)this;
        else if (riid == IID_ITfThreadMgrEventSink) *ppv = (ITfThreadMgrEventSink *)this;
        else if (riid == IID_ITfKeyEventSink) *ppv = (ITfKeyEventSink *)this;
        else if (riid == IID_ITfCompositionSink) *ppv = (ITfCompositionSink *)this;
        else if (riid == IID_ITfThreadFocusSink) *ppv = (ITfThreadFocusSink *)this;
        else if (riid == IID_ITfDisplayAttributeProvider) *ppv = (ITfDisplayAttributeProvider *)this;
        else if (riid == IID_ITfCompartmentEventSink) *ppv = (ITfCompartmentEventSink *)this;
        if (!*ppv) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    // ITfTextInputProcessor(Ex)
    STDMETHODIMP Activate(ITfThreadMgr *ptim, TfClientId tid) { return ActivateEx(ptim, tid, 0); }
    STDMETHODIMP ActivateEx(ITfThreadMgr *ptim, TfClientId tid, DWORD)
    {
        m_tim = ptim; m_tim->AddRef();
        m_tid = tid;
        ITfSource *src;
        if (SUCCEEDED(m_tim->QueryInterface(IID_ITfSource, (void **)&src))) {
            src->AdviseSink(IID_ITfThreadMgrEventSink, (ITfThreadMgrEventSink *)this, &m_tmCookie);
            src->AdviseSink(IID_ITfThreadFocusSink, (ITfThreadFocusSink *)this, &m_tfCookie);
            src->Release();
        }
        ITfKeystrokeMgr *km;
        if (SUCCEEDED(m_tim->QueryInterface(IID_ITfKeystrokeMgr, (void **)&km))) {
            HRESULT hr = km->AdviseKeyEventSink(m_tid, (ITfKeyEventSink *)this, TRUE);
            TIPDBG(L"ActivateEx tid=%u AdviseKeyEventSink hr=0x%08x", (UINT)m_tid, hr);
            km->Release();
        }
        SetCompartmentDword(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, 1);
        // 中/英 button for the language bar and the taskbar input indicator
        m_button = new (std::nothrow) LangBarButton(this, LB_INDICATOR);
        m_modeBtn = new (std::nothrow) LangBarButton(this, LB_MODE);
        m_menuBtn = new (std::nothrow) LangBarButton(this, LB_MENU);
        {
            ITfLangBarItemMgr *lbm;
            if (SUCCEEDED(m_tim->QueryInterface(IID_ITfLangBarItemMgr, (void **)&lbm))) {
                LangBarButton *all[] = { m_button, m_modeBtn, m_menuBtn };
                for (LangBarButton *b : all)
                    if (b && FAILED(lbm->AddItem(b))) TipLog(L"AddItem(lang bar button) failed");
                lbm->Release();
            }
        }
        // watch the 中/英 input-mode compartment (taskbar indicator clicks)
        ITfCompartmentMgr *cmgr;
        if (SUCCEEDED(m_tim->QueryInterface(IID_ITfCompartmentMgr, (void **)&cmgr))) {
            if (SUCCEEDED(cmgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &m_modeComp))) {
                ITfSource *cs;
                if (SUCCEEDED(m_modeComp->QueryInterface(IID_ITfSource, (void **)&cs))) {
                    cs->AdviseSink(IID_ITfCompartmentEventSink, (ITfCompartmentEventSink *)this, &m_modeCookie);
                    cs->Release();
                }
            }
            cmgr->Release();
        }
        if (m_engine.Connected() && m_engine.Call(HCMD_PING)) SyncInputMode();
        else { m_settingMode = true; SetCompartmentDword(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_SYMBOL); m_settingMode = false; }
        // register display attribute atom
        ITfCategoryMgr *cm;
        if (SUCCEEDED(CreateCategoryMgr(&cm))) {
            cm->RegisterGUID(GUID_HaninDisplayAttrInput, &m_attrAtom);
            cm->Release();
        }
        return S_OK;
    }
    STDMETHODIMP Deactivate()
    {
        TIPDBG(L"Deactivate");
        if (m_comp) {
            ITfContextOwnerCompositionServices *cs;
            if (m_compCtx && SUCCEEDED(m_compCtx->QueryInterface(IID_ITfContextOwnerCompositionServices, (void **)&cs))) {
                ITfCompositionView *cv;
                if (SUCCEEDED(m_comp->QueryInterface(IID_ITfCompositionView, (void **)&cv))) {
                    cs->TerminateComposition(cv);
                    cv->Release();
                }
                cs->Release();
            }
            ReleaseComposition();
        }
        if (m_engine.Connected()) m_engine.Call(HCMD_RESET);
        m_cand.Destroy();
        {
            ITfLangBarItemMgr *lbm = NULL;
            if (m_tim) m_tim->QueryInterface(IID_ITfLangBarItemMgr, (void **)&lbm);
            LangBarButton **btns[] = { &m_button, &m_modeBtn, &m_menuBtn };
            for (LangBarButton **b : btns) {
                if (!*b) continue;
                if (lbm) lbm->RemoveItem(*b);
                (*b)->Detach();
                (*b)->Release();
                *b = NULL;
            }
            if (lbm) lbm->Release();
        }
        if (m_modeComp) {
            ITfSource *cs;
            if (m_modeCookie != TF_INVALID_COOKIE && SUCCEEDED(m_modeComp->QueryInterface(IID_ITfSource, (void **)&cs))) {
                cs->UnadviseSink(m_modeCookie);
                cs->Release();
            }
            m_modeComp->Release();
            m_modeComp = NULL;
            m_modeCookie = TF_INVALID_COOKIE;
        }
        if (m_tim) {
            ITfSource *src;
            if (SUCCEEDED(m_tim->QueryInterface(IID_ITfSource, (void **)&src))) {
                if (m_tmCookie != TF_INVALID_COOKIE) src->UnadviseSink(m_tmCookie);
                if (m_tfCookie != TF_INVALID_COOKIE) src->UnadviseSink(m_tfCookie);
                src->Release();
            }
            ITfKeystrokeMgr *km;
            if (SUCCEEDED(m_tim->QueryInterface(IID_ITfKeystrokeMgr, (void **)&km))) {
                km->UnadviseKeyEventSink(m_tid);
                km->Release();
            }
            m_tim->Release();
            m_tim = NULL;
        }
        m_tmCookie = m_tfCookie = TF_INVALID_COOKIE;
        m_engine.Close();
        return S_OK;
    }

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr *) { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr *) { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr *focus, ITfDocumentMgr *prev)
    {
        if (prev && m_comp) FinishPendingComposition();
        m_cand.Hide();
        return S_OK;
    }
    STDMETHODIMP OnPushContext(ITfContext *) { return S_OK; }
    STDMETHODIMP OnPopContext(ITfContext *) { return S_OK; }

    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus()
    {
        // the engine's 中/英 state is global; refresh this thread's indicator
        m_lastMethod = -1;
        if (m_engine.Connected() && m_engine.Call(HCMD_PING)) SyncInputMode();
        return S_OK;
    }
    STDMETHODIMP OnKillThreadFocus()
    {
        if (m_comp) FinishPendingComposition();
        m_cand.Hide();
        return S_OK;
    }

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ec, ITfComposition *comp)
    {
        // the application ended our composition (click elsewhere, etc.)
        if (comp == m_comp) {
            ClearAttribute(ec);
            ReleaseComposition();
            if (m_engine.Connected()) m_engine.Call(HCMD_RESET);
            m_cand.Hide();
        }
        return S_OK;
    }

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo **pp)
    {
        *pp = new (std::nothrow) ::EnumDisplayAttributeInfo();
        return *pp ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo **pp)
    {
        if (guid != GUID_HaninDisplayAttrInput) { *pp = NULL; return E_INVALIDARG; }
        *pp = new (std::nothrow) DisplayAttributeInfo();
        return *pp ? S_OK : E_OUTOFMEMORY;
    }

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fg) { TIPDBG(L"keysink OnSetFocus(%d)", fg); return S_OK; }
    STDMETHODIMP OnTestKeyDown(ITfContext *pic, WPARAM vk, LPARAM lp, BOOL *eaten) { return Key(pic, vk, lp, false, true, eaten); }
    STDMETHODIMP OnTestKeyUp(ITfContext *pic, WPARAM vk, LPARAM lp, BOOL *eaten) { return Key(pic, vk, lp, true, true, eaten); }
    STDMETHODIMP OnKeyDown(ITfContext *pic, WPARAM vk, LPARAM lp, BOOL *eaten) { return Key(pic, vk, lp, false, false, eaten); }
    STDMETHODIMP OnKeyUp(ITfContext *pic, WPARAM vk, LPARAM lp, BOOL *eaten) { return Key(pic, vk, lp, true, false, eaten); }
    STDMETHODIMP OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) { *eaten = FALSE; return S_OK; }

private:
    LONG m_ref = 1;
    ITfThreadMgr *m_tim = NULL;
    TfClientId m_tid = TF_CLIENTID_NULL;
    DWORD m_tmCookie = TF_INVALID_COOKIE, m_tfCookie = TF_INVALID_COOKIE;
    TfGuidAtom m_attrAtom = TF_INVALID_GUIDATOM;
    ITfComposition *m_comp = NULL;
    ITfContext *m_compCtx = NULL;
    Engine m_engine;
    bool m_downEaten[256] = {};
    int m_lastMethod = -1;
    bool m_settingMode = false;
    ITfCompartment *m_modeComp = NULL;
    LangBarButton *m_button = NULL;
    LangBarButton *m_menuBtn = NULL;
    LangBarButton *m_modeBtn = NULL;
    DWORD m_modeCookie = TF_INVALID_COOKIE;
    CandWindow m_cand;

    void ReleaseComposition()
    {
        if (m_comp) { m_comp->Release(); m_comp = NULL; }
        if (m_compCtx) { m_compCtx->Release(); m_compCtx = NULL; }
    }

    bool GetCompartmentDword(IUnknown *owner, REFGUID guid, DWORD *val)
    {
        ITfCompartmentMgr *cm;
        bool ok = false;
        *val = 0;
        if (owner && SUCCEEDED(owner->QueryInterface(IID_ITfCompartmentMgr, (void **)&cm))) {
            ITfCompartment *c;
            if (SUCCEEDED(cm->GetCompartment(guid, &c))) {
                VARIANT v; VariantInit(&v);
                if (c->GetValue(&v) == S_OK && v.vt == VT_I4) { *val = (DWORD)v.lVal; ok = true; }
                c->Release();
            }
            cm->Release();
        }
        return ok;
    }
    void SetCompartmentDword(REFGUID guid, DWORD val)
    {
        ITfCompartmentMgr *cm;
        if (m_tim && SUCCEEDED(m_tim->QueryInterface(IID_ITfCompartmentMgr, (void **)&cm))) {
            ITfCompartment *c;
            if (SUCCEEDED(cm->GetCompartment(guid, &c))) {
                VARIANT v; v.vt = VT_I4; v.lVal = (LONG)val;
                c->SetValue(m_tid, &v);
                c->Release();
            }
            cm->Release();
        }
    }

    bool KeyboardUsable(ITfContext *pic)
    {
        DWORD v;
        if (GetCompartmentDword(m_tim, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &v) && !v) { TIPDBG(L"OPENCLOSE=0"); return false; }
        if (pic) {
            if (GetCompartmentDword(pic, GUID_COMPARTMENT_KEYBOARD_DISABLED, &v) && v) { TIPDBG(L"KEYBOARD_DISABLED"); return false; }
            if (GetCompartmentDword(pic, GUID_COMPARTMENT_EMPTYCONTEXT, &v) && v) { TIPDBG(L"EMPTYCONTEXT"); return false; }
        } else TIPDBG(L"no context");
        return true;
    }

    HRESULT Key(ITfContext *pic, WPARAM vk, LPARAM lp, bool up, bool test, BOOL *eaten)
    {
        *eaten = FALSE;
        if (!KeyboardUsable(pic)) { TIPDBG(L"key 0x%02x: keyboard not usable", (UINT)vk); return S_OK; }
        bool composing = m_comp != NULL;
        BYTE v = (BYTE)vk;
        bool isShift = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
        if (!composing && !up) {
            // Ctrl / Ctrl+Alt combinations go to the engine, which only takes its own
            // configurable hotkeys (Ctrl+` mode, Ctrl+3 symbols, ...) and verifiably
            // returns Ctrl+C/V/X/Z/A/S/... to the application. Plain Alt+key is menu
            // territory and never offered.
            bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
            if (alt && !ctrl && !isShift) { m_downEaten[v] = false; return S_OK; }
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS) return S_OK;
        }
        // key-up: only keys whose key-down the engine took (plus Shift, which toggles
        // Chinese/English on release) are offered to it - the app never loses a key-up
        // for a key-down it received.
        if (up && !isShift && !m_downEaten[v]) return S_OK;

        UINT flags = up ? HREQ_UP : 0;
        TIPDBG(L"%s%s vk=0x%02x composing=%d", test ? L"test" : L"key", up ? L"up" : L"down", (UINT)vk, composing);
        if (!m_engine.Call(test ? HCMD_TEST : HCMD_KEY, (UINT)vk, lp, flags)) {
            if (composing && !test) TipLog(L"engine unavailable while composing; passing key through");
            if (!up) m_downEaten[v] = false;
            return S_OK;
        }
        *eaten = m_engine.resp.eaten ? TRUE : FALSE;
        TIPDBG(L"  -> eaten=%d cand=%u method=%d", *eaten, m_engine.resp.candOpen, m_engine.resp.method);
        if (!test) {
            if (!up) m_downEaten[v] = *eaten != FALSE;
            else m_downEaten[v] = false;
            if (*eaten) Apply(pic);
        }
        SyncInputMode();   // a Shift release toggles 中/英 already during the test phase
        return S_OK;
    }

    // ---- Chinese/English indicator (taskbar 中/英) ------------------------
    void SyncInputMode()
    {
        int m = m_engine.resp.method;
        if (m < 0 || m == m_lastMethod) return;
        m_lastMethod = m;
        if (m_button) m_button->SetChinese(m == HMETHOD_CHINESE);
        if (m_menuBtn) m_menuBtn->SetChinese(m == HMETHOD_CHINESE);
        if (m_modeBtn) m_modeBtn->SetChinese(m == HMETHOD_CHINESE);
        m_settingMode = true;
        SetCompartmentDword(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                            m == HMETHOD_CHINESE ? (TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_SYMBOL) : TF_CONVERSIONMODE_ALPHANUMERIC);
        m_settingMode = false;
    }

    // LangBarHost: clicks on the 中/英 button
    void LbToggle()
    {
        if (m_engine.Call(HCMD_TOGGLE)) { m_lastMethod = -1; SyncInputMode(); }
    }
    void LbMenu(UINT id)
    {
        if (id == LBMENU_TOGGLE) { LbToggle(); return; }
        // the engine host (outside any app container) starts the program
        m_engine.Call(HCMD_LAUNCH, id == LBMENU_QUERY ? HLAUNCH_QUERY : HLAUNCH_TOOLBOX);
    }

    // ITfCompartmentEventSink: the user clicked the 中/英 indicator
    STDMETHODIMP OnChange(REFGUID guid)
    {
        if (guid != GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION || m_settingMode) return S_OK;
        DWORD conv;
        if (!GetCompartmentDword(m_tim, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &conv)) return S_OK;
        int want = (conv & TF_CONVERSIONMODE_NATIVE) ? HMETHOD_CHINESE : HMETHOD_ENGLISH;
        if (m_lastMethod >= 0 && want == m_lastMethod) return S_OK;
        if (m_engine.Call(HCMD_PING) && m_engine.resp.method >= 0 && m_engine.resp.method != want)
            m_engine.Call(HCMD_TOGGLE);
        m_lastMethod = -1;
        SyncInputMode();
        return S_OK;
    }

    void FinishPendingComposition()
    {
        ITfContext *pic = m_compCtx;
        if (!pic) return;
        pic->AddRef();
        if (m_engine.Call(HCMD_COMPLETE)) {
            HANIN_RESP *r = &m_engine.resp;
            wchar_t text[HANIN_MAX_RESULT];
            StringCchCopyW(text, HANIN_MAX_RESULT, r->result);
            RunEditSession(pic, m_tid, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, text](TfEditCookie ec) -> HRESULT {
                if (!m_comp) return S_OK;
                ITfRange *range;
                if (SUCCEEDED(m_comp->GetRange(&range))) {
                    if (text[0]) range->SetText(ec, 0, text, (LONG)wcslen(text));
                    ClearAttribute(ec);
                    range->Collapse(ec, TF_ANCHOR_END);
                    SetSelectionRange(ec, m_compCtx, range);
                    range->Release();
                }
                m_comp->EndComposition(ec);
                ReleaseComposition();
                return S_OK;
            });
        } else {
            ITfContextOwnerCompositionServices *cs;
            if (SUCCEEDED(pic->QueryInterface(IID_ITfContextOwnerCompositionServices, (void **)&cs))) {
                ITfCompositionView *cv;
                if (SUCCEEDED(m_comp->QueryInterface(IID_ITfCompositionView, (void **)&cv))) {
                    cs->TerminateComposition(cv);
                    cv->Release();
                }
                cs->Release();
            }
            ReleaseComposition();
        }
        pic->Release();
    }

    void SetSelectionRange(TfEditCookie ec, ITfContext *pic, ITfRange *range)
    {
        TF_SELECTION sel;
        sel.range = range;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        pic->SetSelection(ec, 1, &sel);
    }

    void ClearAttribute(TfEditCookie ec)
    {
        if (!m_comp || !m_compCtx) return;
        ITfRange *range;
        ITfProperty *prop;
        if (SUCCEEDED(m_comp->GetRange(&range))) {
            if (SUCCEEDED(m_compCtx->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) {
                prop->Clear(ec, range);
                prop->Release();
            }
            range->Release();
        }
    }

    // apply the engine's answer to the document
    void Apply(ITfContext *pic)
    {
        HANIN_RESP *r = &m_engine.resp;
        RECT anchor = {};
        bool haveAnchor = false;
        RunEditSession(pic, m_tid, TF_ES_SYNC | TF_ES_READWRITE, [&](TfEditCookie ec) -> HRESULT {
            // 1. committed text
            if (r->result[0]) {
                LONG len = (LONG)wcslen(r->result);
                if (m_comp) {
                    ITfRange *range;
                    if (SUCCEEDED(m_comp->GetRange(&range))) {
                        range->SetText(ec, 0, r->result, len);
                        ClearAttribute(ec);
                        range->Collapse(ec, TF_ANCHOR_END);
                        SetSelectionRange(ec, pic, range);
                        range->Release();
                    }
                    m_comp->EndComposition(ec);
                    ReleaseComposition();
                } else {
                    ITfInsertAtSelection *ias;
                    if (SUCCEEDED(pic->QueryInterface(IID_ITfInsertAtSelection, (void **)&ias))) {
                        ITfRange *range;
                        if (SUCCEEDED(ias->InsertTextAtSelection(ec, 0, r->result, len, &range))) {
                            range->Collapse(ec, TF_ANCHOR_END);
                            SetSelectionRange(ec, pic, range);
                            range->Release();
                        }
                        ias->Release();
                    }
                }
            }
            // 2. composition
            LONG clen = (LONG)wcslen(r->comp);
            if (clen) {
                if (!m_comp) {
                    ITfInsertAtSelection *ias;
                    ITfContextComposition *cc;
                    ITfRange *range = NULL;
                    if (SUCCEEDED(pic->QueryInterface(IID_ITfInsertAtSelection, (void **)&ias))) {
                        ias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0, &range);
                        ias->Release();
                    }
                    if (range && SUCCEEDED(pic->QueryInterface(IID_ITfContextComposition, (void **)&cc))) {
                        if (SUCCEEDED(cc->StartComposition(ec, range, (ITfCompositionSink *)this, &m_comp)) && m_comp) {
                            m_compCtx = pic;
                            m_compCtx->AddRef();
                        }
                        cc->Release();
                    }
                    if (range) range->Release();
                }
                if (m_comp) {
                    ITfRange *range;
                    if (SUCCEEDED(m_comp->GetRange(&range))) {
                        range->SetText(ec, 0, r->comp, clen);
                        if (m_attrAtom != TF_INVALID_GUIDATOM) {
                            ITfProperty *prop;
                            if (SUCCEEDED(pic->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) {
                                VARIANT v; v.vt = VT_I4; v.lVal = (LONG)m_attrAtom;
                                prop->SetValue(ec, range, &v);
                                prop->Release();
                            }
                        }
                        // caret inside the composition
                        ITfRange *caret;
                        if (SUCCEEDED(range->Clone(&caret))) {
                            LONG moved;
                            LONG cur = (LONG)min(r->compCursor, (uint32_t)clen);
                            caret->Collapse(ec, TF_ANCHOR_START);
                            caret->ShiftStart(ec, cur, &moved, NULL);
                            caret->Collapse(ec, TF_ANCHOR_START);
                            SetSelectionRange(ec, pic, caret);
                            // anchor rect for the candidate window: the character at the caret
                            ITfRange *ch;
                            if (SUCCEEDED(caret->Clone(&ch))) {
                                if (cur < clen) ch->ShiftEnd(ec, 1, &moved, NULL);
                                else ch->ShiftStart(ec, -1, &moved, NULL);
                                ITfContextView *view;
                                if (SUCCEEDED(pic->GetActiveView(&view))) {
                                    BOOL clipped;
                                    if (SUCCEEDED(view->GetTextExt(ec, ch, &anchor, &clipped))) haveAnchor = true;
                                    view->Release();
                                }
                                ch->Release();
                            }
                            caret->Release();
                        }
                        range->Release();
                    }
                }
            } else if (m_comp) {
                // composition emptied without a commit (ESC / backspace)
                ITfRange *range;
                if (SUCCEEDED(m_comp->GetRange(&range))) {
                    range->SetText(ec, 0, L"", 0);
                    range->Release();
                }
                m_comp->EndComposition(ec);
                ReleaseComposition();
            }
            return S_OK;
        });

        if (r->candOpen) {
            if (!haveAnchor || (anchor.left == 0 && anchor.top == 0 && anchor.bottom == 0)) {
                POINT pt;
                GetCaretPos(&pt);
                HWND fw = GetFocus();
                if (fw) ClientToScreen(fw, &pt);
                anchor.left = anchor.right = pt.x;
                anchor.top = pt.y; anchor.bottom = pt.y + 20;
            }
            m_cand.Update(r->cand, anchor);
        } else {
            m_cand.Hide();
        }
    }
};

// ---------------------------------------------------------------------------
// class factory & DLL exports
// ---------------------------------------------------------------------------
class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = (IClassFactory *)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { InterlockedIncrement(&g_cRefDll); return 2; }
    STDMETHODIMP_(ULONG) Release() { InterlockedDecrement(&g_cRefDll); return 1; }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv)
    {
        *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        HaninTip *t = new (std::nothrow) HaninTip();
        if (!t) return E_OUTOFMEMORY;
        HRESULT hr = t->QueryInterface(riid, ppv);
        t->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock)
    {
        if (lock) InterlockedIncrement(&g_cRefDll); else InterlockedDecrement(&g_cRefDll);
        return S_OK;
    }
};
static ClassFactory g_factory;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (clsid == CLSID_HaninTip) return g_factory.QueryInterface(riid, ppv);
    *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_cRefDll <= 0 ? S_OK : S_FALSE; }

static const GUID *kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    &GUID_TFCAT_TIPCAP_COMLESS,
};

static void ClsidKey(wchar_t *buf, size_t cch)
{
    wchar_t g[64];
    StringFromGUID2(CLSID_HaninTip, g, 64);
    StringCchPrintfW(buf, cch, L"Software\\Classes\\CLSID\\%s", g);
}

STDAPI DllRegisterServer()
{
    wchar_t key[128], path[MAX_PATH], sub[160];
    HKEY hk;
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    ClsidKey(key, 128);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)kTipName, (DWORD)(wcslen(kTipName) + 1) * 2);
    RegCloseKey(hk);
    StringCchPrintfW(sub, 160, L"%s\\InprocServer32", key);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)path, (DWORD)(wcslen(path) + 1) * 2);
    RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Apartment", 20);
    RegCloseKey(hk);

    HRESULT hr = CoInitialize(NULL);
    bool uninit = SUCCEEDED(hr);
    ITfInputProcessorProfileMgr *pm;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr, (void **)&pm);
    if (SUCCEEDED(hr)) {
        wchar_t icon[MAX_PATH];
        ULONG iconIndex = 0;
        if (OriginalIconFile(icon)) iconIndex = kOriginalIconIndex;
        else StringCchCopyW(icon, MAX_PATH, path);   // our own fallback icon (resource #0)
        hr = pm->RegisterProfile(CLSID_HaninTip, kLangId, GUID_HaninProfile, kTipName, (ULONG)wcslen(kTipName),
                                 icon, (ULONG)wcslen(icon), iconIndex, NULL, 0, TRUE, 0);
        pm->Release();
    }
    if (SUCCEEDED(hr)) {
        ITfCategoryMgr *cm;
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void **)&cm);
        if (SUCCEEDED(hr)) {
            for (const GUID *c : kCategories) cm->RegisterCategory(CLSID_HaninTip, *c, CLSID_HaninTip);
            cm->Release();
        }
    }
    if (uninit) CoUninitialize();
    return SUCCEEDED(hr) ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer()
{
    HRESULT hr = CoInitialize(NULL);
    bool uninit = SUCCEEDED(hr);
    ITfInputProcessorProfileMgr *pm;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr, (void **)&pm))) {
        pm->UnregisterProfile(CLSID_HaninTip, kLangId, GUID_HaninProfile, 0);
        pm->Release();
    }
    ITfCategoryMgr *cm;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void **)&cm))) {
        for (const GUID *c : kCategories) cm->UnregisterCategory(CLSID_HaninTip, *c, CLSID_HaninTip);
        cm->Release();
    }
    // removes the whole HKLM\SOFTWARE\Microsoft\CTF\TIP\{clsid} tree (no empty keys left behind)
    ITfInputProcessorProfiles *pp;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles, (void **)&pp))) {
        pp->Unregister(CLSID_HaninTip);
        pp->Release();
    }
    if (uninit) CoUninitialize();
    wchar_t key[128];
    ClsidKey(key, 128);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
    return S_OK;
}
