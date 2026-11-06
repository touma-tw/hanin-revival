// tiptest.cpp - end-to-end test of the installed HaninTip through real TSF.
// Creates an EDIT control (TSF via CUAS), activates the 漢音 profile for this
// process and injects keys with ITfKeystrokeMgr (no SendInput, no foreground
// stealing). Prints the edit text after each key.
//   tiptest.exe "<keys>" [snapdir]
// Tokens: {ENTER} {ESC} {SPACE} {BS} {DOWN} {UP} {LEFT} {RIGHT} {SHIFT} {SNAP} {WAIT}
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <msctf.h>
#include <stdio.h>
#include <string>
#include <psapi.h>

static const CLSID CLSID_HaninTip = { 0x615f8932, 0x792a, 0x4588, { 0x86, 0x35, 0x33, 0x2c, 0xf2, 0xb7, 0xec, 0x61 } };
static const GUID GUID_HaninProfile = { 0x0a18306e, 0x52dc, 0x4413, { 0xbf, 0x02, 0x07, 0xb9, 0x79, 0x06, 0xb4, 0x15 } };

static HWND g_main, g_edit;
static ITfKeystrokeMgr *g_km;
static ITfThreadMgr *g_tim;
static const char *g_snapDir;
static int g_snapNo;

static std::string u8(const wchar_t *w)
{
    char b[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), NULL, NULL);
    return std::string(b, n > 0 ? n - 1 : 0);
}

static void pump(DWORD ms = 30)
{
    DWORD t = GetTickCount();
    MSG m;
    do {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(5);
    } while (GetTickCount() - t < ms);
}

static HWND find_cand()
{
    HWND h = NULL;
    while ((h = FindWindowExW(NULL, h, L"HaninRevivalCand", NULL)) != NULL) {
        DWORD pid;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return h;
    }
    return NULL;
}

static void save_bmp(HWND h, const char *path)
{
    RECT rc;
    GetWindowRect(h, &rc);
    int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    HDC sdc = GetDC(NULL), mem = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = hh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    void *bits;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ o = SelectObject(mem, bmp);
    PrintWindow(h, mem, 0);
    SelectObject(mem, o);
    int stride = ((w * 3 + 3) & ~3);
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + stride * hh;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f); fwrite(bits, stride * hh, 1, f); fclose(f); }
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, sdc);
}

static void report(const char *what)
{
    wchar_t text[2048];
    GetWindowTextW(g_edit, text, 2048);
    HWND c = find_cand();
    DWORD conv = 0xFFFF;
    ITfCompartmentMgr *cm;
    if (g_tim && SUCCEEDED(g_tim->QueryInterface(IID_ITfCompartmentMgr, (void **)&cm))) {
        ITfCompartment *cp;
        if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &cp))) {
            VARIANT v; VariantInit(&v);
            if (cp->GetValue(&v) == S_OK && v.vt == VT_I4) conv = v.lVal;
            cp->Release();
        }
        cm->Release();
    }
    printf("%-8s mode=%s edit='%s'%s\n", what, (conv == 0xFFFF ? "?" : (conv & 1) ? "中" : "英"), u8(text).c_str(), (c && IsWindowVisible(c)) ? "  [cand visible]" : "");
    fflush(stdout);
}

static bool g_abort;

static void send_vk(UINT vk, bool up)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, 0);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(in));
}

// Real keyboard input; refuses to type unless our window is still the foreground window.
static bool g_ctrl;
static void key(UINT vk, bool shift, char ch)
{
    if (g_abort) return;
    if (GetForegroundWindow() != g_main) {
        printf("!! foreground lost - aborting so no keys reach another window\n");
        g_abort = true;
        return;
    }
    if (g_ctrl) send_vk(VK_LCONTROL, false);
    if (shift) send_vk(VK_LSHIFT, false);
    send_vk(vk, false);
    pump(25);
    send_vk(vk, true);
    if (shift) send_vk(VK_LSHIFT, true);
    if (g_ctrl) send_vk(VK_LCONTROL, true);
    pump(60);
    char tag[16];
    sprintf(tag, "[%c]", ch ? ch : '?');
    report(tag);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SETFOCUS && g_edit) { SetFocus(g_edit); return 0; }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tiptest \"<keys>\" [snapdir]\n"); return 1; }
    g_snapDir = argc > 2 ? argv[2] : NULL;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HaninTipTest";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, L"HaninTipTest", L"Hanin TIP test", WS_OVERLAPPEDWINDOW, 200, 200, 700, 220, NULL, NULL, wc.hInstance, NULL);
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL, 10, 10, 660, 150, g_main, NULL, wc.hInstance, NULL);
    HFONT f = CreateFontW(-20, 0, 0, 0, 0, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft JhengHei UI");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)f, TRUE);
    // TSF only dispatches keystrokes to the thread that owns the foreground window
    HWND prevFg = GetForegroundWindow();
    ShowWindow(g_main, SW_SHOW);
    if (!SetForegroundWindow(g_main)) {
        DWORD fgTid = GetWindowThreadProcessId(prevFg, NULL);
        AttachThreadInput(GetCurrentThreadId(), fgTid, TRUE);
        SetForegroundWindow(g_main);
        AttachThreadInput(GetCurrentThreadId(), fgTid, FALSE);
    }
    printf("foreground ok=%d\n", GetForegroundWindow() == g_main);
    SetFocus(g_edit);
    pump(200);

    ITfThreadMgr *tim = NULL;
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void **)&tim);
    printf("ThreadMgr hr=0x%08lx\n", hr);
    if (FAILED(hr)) return 2;
    TfClientId appTid = 0;
    hr = tim->Activate(&appTid);   // become a TSF-aware client so app-fed keystrokes are routed
    printf("ThreadMgr::Activate hr=0x%08lx tid=%u\n", hr, appTid);
    ITfDocumentMgr *dim = NULL;
    tim->GetFocus(&dim);
    printf("focus docmgr=%p\n", dim);
    if (dim) dim->Release();

    ITfInputProcessorProfileMgr *pm = NULL;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr, (void **)&pm);
    if (SUCCEEDED(hr))
        hr = pm->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0404, CLSID_HaninTip, GUID_HaninProfile, NULL,
                                 TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
    printf("ActivateProfile hr=0x%08lx\n", hr);
    pump(300);
    TF_INPUTPROCESSORPROFILE prof = {};
    if (pm && SUCCEEDED(pm->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &prof))) {
        wchar_t g[64];
        StringFromGUID2(prof.clsid, g, 64);
        printf("active keyboard TIP: %s lang=0x%04x\n", u8(g).c_str(), prof.langid);
    }
    tim->QueryInterface(IID_ITfKeystrokeMgr, (void **)&g_km);
    g_tim = tim;
    if (!g_km) return 3;

    for (const char *s = argv[1]; *s;) {
        if (*s == '{') {
            const char *e = strchr(s, '}');
            std::string tok(s + 1, e - s - 1);
            s = e + 1;
            if (tok == "ENTER") key(VK_RETURN, false, 'E');
            else if (tok == "ESC") key(VK_ESCAPE, false, 'X');
            else if (tok == "SPACE") key(VK_SPACE, false, '_');
            else if (tok == "BS") key(VK_BACK, false, 'B');
            else if (tok == "DOWN") key(VK_DOWN, false, 'v');
            else if (tok == "UP") key(VK_UP, false, '^');
            else if (tok == "LEFT") key(VK_LEFT, false, '<');
            else if (tok == "RIGHT") key(VK_RIGHT, false, '>');
            else if (tok == "SHIFT") key(VK_SHIFT, false, 'S');
            else if (tok == "WAIT") pump(1500);
            else if (tok == "TASKBAR") {
                HMONITOR mon = MonitorFromWindow(g_main, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfoW(mon, &mi);
                RECT r = { mi.rcMonitor.right - 700, mi.rcWork.bottom, mi.rcMonitor.right, mi.rcMonitor.bottom };
                if (r.bottom - r.top < 20) r.top = r.bottom - 60;   // auto-hide / side taskbar
                int w = r.right - r.left, hh = r.bottom - r.top;
                HDC sdc = GetDC(NULL), mem = CreateCompatibleDC(sdc);
                BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = hh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
                void *bits; HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
                HGDIOBJ o = SelectObject(mem, bmp);
                BitBlt(mem, 0, 0, w, hh, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT);
                SelectObject(mem, o);
                char p[MAX_PATH]; sprintf(p, "%s\\taskbar%d.bmp", g_snapDir ? g_snapDir : ".", g_snapNo++);
                int stride = ((w * 3 + 3) & ~3);
                BITMAPFILEHEADER fh = {}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader); fh.bfSize = fh.bfOffBits + stride * hh;
                FILE *f = fopen(p, "wb");
                if (f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f); fwrite(bits, stride * hh, 1, f); fclose(f); }
                DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, sdc);
                printf("taskbar -> %s\n", p);
            }
            else if (tok == "LBTEST" || tok == "LBMENU1") {
                static const GUID lbi = { 0x2c77a81e, 0x41cc, 0x4178, { 0xa3, 0xa7, 0x5f, 0x8a, 0x98, 0x75, 0x68, 0xe6 } };
                ITfLangBarItemMgr *lbm = NULL;
                ITfLangBarItem *item = NULL;
                ITfLangBarItemButton *btn = NULL;
                HRESULT h1 = g_tim->QueryInterface(IID_ITfLangBarItemMgr, (void **)&lbm);
                HRESULT h2 = lbm ? lbm->GetItem(lbi, &item) : E_FAIL;
                if (item) item->QueryInterface(IID_ITfLangBarItemButton, (void **)&btn);
                printf("langbar: mgr=0x%08lx item=0x%08lx button=%p\n", h1, h2, btn);
                if (btn) {
                    BSTR t = NULL; HICON ic = NULL;
                    btn->GetText(&t); btn->GetIcon(&ic);
                    printf("  text='%s' icon=%p\n", t ? u8(t).c_str() : "", ic);
                    if (t) SysFreeString(t);
                    if (tok == "LBTEST") {
                        POINT pt = { 0, 0 };
                        btn->OnClick(TF_LBI_CLK_LEFT, pt, NULL);
                        pump(100);
                        t = NULL; btn->GetText(&t);
                        printf("  after left click: text='%s'\n", t ? u8(t).c_str() : "");
                        if (t) SysFreeString(t);
                    } else {
                        btn->OnMenuSelect(1);   // LBMENU_TOOLBOX
                        printf("  menu: toolbox requested\n");
                    }
                    btn->Release();
                }
                if (item) item->Release();
                if (lbm) lbm->Release();
            }
            else if (tok.size() == 2 && tok[0] == 0x5E) { SHORT r = VkKeyScanA(tok[1]); g_ctrl = true; key(r & 0xFF, (r >> 8) & 1, tok[1]); g_ctrl = false; }
            else if (tok == "SCREEN") {
                // what the user actually sees: screen pixels around the test window
                RECT r; GetWindowRect(g_main, &r);
                r.bottom += 320; r.right += 80;
                int w = r.right - r.left, hh = r.bottom - r.top;
                HDC sdc = GetDC(NULL), mem = CreateCompatibleDC(sdc);
                BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = hh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
                void *bits; HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
                HGDIOBJ o = SelectObject(mem, bmp);
                BitBlt(mem, 0, 0, w, hh, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT);
                SelectObject(mem, o);
                char p[MAX_PATH]; sprintf(p, "%s\\screen%d.bmp", g_snapDir ? g_snapDir : ".", g_snapNo++);
                int stride = ((w * 3 + 3) & ~3);
                BITMAPFILEHEADER fh = {}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader); fh.bfSize = fh.bfOffBits + stride * hh;
                FILE *f = fopen(p, "wb");
                if (f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f); fwrite(bits, stride * hh, 1, f); fclose(f); }
                DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, sdc);
                printf("screen -> %s\n", p);
            }
            else if (tok == "SNAP") {
                HWND c = find_cand();
                char p[MAX_PATH];
                if (g_snapDir && c) {
                    sprintf(p, "%s\\cand%d.bmp", g_snapDir, g_snapNo++);
                    save_bmp(c, p);
                    printf("snap -> %s\n", p);
                }
                if (g_snapDir) {
                    sprintf(p, "%s\\main%d.bmp", g_snapDir, g_snapNo++);
                    save_bmp(g_main, p);
                    printf("snap -> %s\n", p);
                }
            }
        } else {
            SHORT r = VkKeyScanA(*s);
            key(r & 0xFF, (r >> 8) & 1, *s);
            s++;
        }
    }
    report("FINAL");
    {
        HMODULE mods[512]; DWORD need;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
            for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
                wchar_t n[MAX_PATH];
                GetModuleFileNameW(mods[i], n, MAX_PATH);
                if (wcsstr(n, L"Hanin") || wcsstr(n, L"hanin")) printf("module: %s\n", u8(n).c_str());
            }
    }
    g_km->Release();
    if (pm) pm->Release();
    tim->Release();
    DestroyWindow(g_main);
    if (prevFg && IsWindow(prevFg)) SetForegroundWindow(prevFg);
    CoUninitialize();
    return 0;
}
