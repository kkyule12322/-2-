// ============================================================
// 嵌入分步实验器 (embed_test.c)
// 目的：定位"页签嵌入"翻车的具体环节。对一个大话西游客户端窗口
// 逐步施加改动，每步之后由用户在游戏内验证鼠标精度：
//   [1] SetParent 进容器（尺寸位置完全不动，零改动）
//   [2] 加页签条（容器加高34px，游戏窗口整体下移34px，尺寸不变）
//   [3] 去掉游戏边框标题栏（WS_POPUP，窗口外框不动，客户区变大）
//   [4] 一键恢复原样（还原父窗口/位置/样式）
// 编译: gcc -O2 -s -municode -mwindows embed_test.c -o embed_test.exe -lgdi32 -luser32
// ============================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdio.h>

#define STRIP_H 34
#define APP L"嵌入实验器"

static HWND g_game = NULL;        // 游戏主窗口
static HWND g_container = NULL;   // 容器
static RECT g_orig;               // 游戏原始屏幕矩形
static LONG g_origStyle = 0;      // 游戏原始样式
static int  g_step = 0;           // 当前步数
static HWND g_log = NULL, g_h1, g_h2, g_h3, g_h4, g_h5;
static HFONT g_font = NULL;

static void Log(const WCHAR *s)
{
    int n = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

// 找游戏窗口：大话西游.exe 的可见顶层主窗口（面积最大）
static BOOL CALLBACK FindCb(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return TRUE;
    WCHAR path[MAX_PATH];
    DWORD sz = MAX_PATH;
    BOOL isGame = FALSE;
    if (QueryFullProcessImageNameW(h, 0, path, &sz)) {
        const WCHAR *b = wcsrchr(path, L'\\');
        b = b ? b + 1 : path;
        isGame = (_wcsicmp(b, L"大话西游.exe") == 0);
    }
    CloseHandle(h);
    if (!isGame) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r, best;
    GetWindowRect(hwnd, &r);
    if (g_game) {
        GetWindowRect(g_game, &best);
        long a1 = (r.right - r.left) * (r.bottom - r.top);
        long a2 = (best.right - best.left) * (best.bottom - best.top);
        if (a1 <= a2) return TRUE;
    }
    g_game = hwnd;
    (void)lp;
    return TRUE;
}

static void EnableButtons(BOOL e)
{
    EnableWindow(g_h1, e && g_step == 0);
    EnableWindow(g_h2, e && g_step == 1);
    EnableWindow(g_h3, e && g_step == 2);
    EnableWindow(g_h5, e && g_step == 2);   // 与 3 二选一
    EnableWindow(g_h4, e && g_step >= 1);
}

// 步骤1：SetParent，游戏窗口零改动（容器客户区与游戏矩形完全重合）
static void Step1(void)
{
    if (!g_game) {
        EnumWindows(FindCb, 0);
        if (!g_game) { Log(L"没找到大话西游.exe 的窗口，请先开一个客户端。"); return; }
    }
    GetWindowRect(g_game, &g_orig);
    g_origStyle = GetWindowLongW(g_game, GWL_STYLE);
    WCHAR title[128];
    GetWindowTextW(g_game, title, 128);
    Log(title);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"EmbedTestC";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    // WS_POPUP 无边框容器：客户区 = 游戏矩形，游戏画面纹丝不动
    g_container = CreateWindowExW(WS_EX_TOOLWINDOW, L"EmbedTestC", APP,
        WS_POPUP | WS_VISIBLE, g_orig.left, g_orig.top,
        g_orig.right - g_orig.left, g_orig.bottom - g_orig.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetParent(g_game, g_container);
    g_step = 1;
    Log(L"[步骤1] 已 SetParent（游戏窗口尺寸/位置/样式全部未动）。");
    Log(L"  现在去游戏里验证：鼠标悬停NPC/物品，看高亮位置是否准确。");
    EnableButtons(TRUE);
}

// 步骤2：加页签条——容器加高34，游戏窗口整体下移34（尺寸不变，只平移）
static void Step2(void)
{
    if (g_step < 1) return;
    int w = g_orig.right - g_orig.left;
    int h = g_orig.bottom - g_orig.top;
    SetWindowPos(g_container, NULL, g_orig.left, g_orig.top - STRIP_H,
                 w, h + STRIP_H, SWP_NOZORDER);
    SetWindowPos(g_game, NULL, 0, STRIP_H, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_container, NULL, TRUE);
    g_step = 2;
    EnableButtons(TRUE);
    Log(L"[步骤2] 已加页签条：游戏窗口整体下移34px，宽高未变。");
    Log(L"  再去游戏里验证鼠标精度（重点：上下方向是否偏）。");
}

// 步骤3：去掉游戏边框标题栏（改 GWL_STYLE，窗口外框矩形不动）
static void Step3(void)
{
    if (g_step < 2) return;
    LONG st = GetWindowLongW(g_game, GWL_STYLE);
    SetWindowLongW(g_game, GWL_STYLE,
                   WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetWindowPos(g_game, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    g_step = 3;
    (void)st;
    Log(L"[步骤3] 已去掉游戏边框/标题栏（WS_POPUP）。");
    Log(L"  再验证鼠标精度，并看游戏画面顶部是否多出一条（客户区变高的表现）。");
}

// 步骤5（3的对照变体）：去边框，但把窗口矩形同步缩小，使客户区尺寸一字节不变
static void Step5(void)
{
    if (g_step != 2) return;
    // 当前客户区尺寸与客户区屏幕位置
    RECT cr;
    GetClientRect(g_game, &cr);
    POINT pc = { 0, 0 };
    ClientToScreen(g_game, &pc);
    LONG cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    // 原样式下边框占位
    RECT pad = { 0, 0, 0, 0 };
    AdjustWindowRect(&pad, (DWORD)g_origStyle, FALSE);
    SetWindowLongW(g_game, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
    // 无边框窗口：窗口矩形 = 客户区矩形；把客户区屏幕位置放回原处
    SetWindowPos(g_game, NULL, pc.x, pc.y, cw, ch,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_step = 5;
    EnableButtons(TRUE);
    Log(L"[步骤5] 去边框且客户区尺寸/屏幕位置保持不变（窗口矩形已缩小）。");
    Log(L"  验证鼠标精度与画面是否与步骤2完全一致。");
}

// 步骤4/恢复：还原一切
static void Restore(void)
{
    if (!g_game) return;
    if (g_container && GetParent(g_game) == g_container)
        SetParent(g_game, NULL);
    SetWindowLongW(g_game, GWL_STYLE, g_origStyle);
    SetWindowPos(g_game, NULL, g_orig.left, g_orig.top,
                 g_orig.right - g_orig.left, g_orig.bottom - g_orig.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
    if (g_container) { DestroyWindow(g_container); g_container = NULL; }
    g_step = 0;
    Log(L"[恢复] 游戏窗口已还原（父窗口/样式/位置/尺寸）。");
    EnableButtons(TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_h1 = CreateWindowW(L"BUTTON", L"1 嵌入(零改动)", WS_CHILD | WS_VISIBLE,
                             10, 10, 130, 30, hwnd, (HMENU)1, NULL, NULL);
        g_h2 = CreateWindowW(L"BUTTON", L"2 加页签条", WS_CHILD | WS_VISIBLE,
                             150, 10, 120, 30, hwnd, (HMENU)2, NULL, NULL);
        g_h3 = CreateWindowW(L"BUTTON", L"3 去边框", WS_CHILD | WS_VISIBLE,
                             280, 10, 100, 30, hwnd, (HMENU)3, NULL, NULL);
        g_h5 = CreateWindowW(L"BUTTON", L"5 去边框(保客户区)", WS_CHILD | WS_VISIBLE,
                             390, 10, 150, 30, hwnd, (HMENU)5, NULL, NULL);
        g_h4 = CreateWindowW(L"BUTTON", L"4 恢复原样", WS_CHILD | WS_VISIBLE,
                             550, 10, 110, 30, hwnd, (HMENU)4, NULL, NULL);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                  10, 50, 660, 200, hwnd, (HMENU)6, NULL, NULL);
        HWND c = GetWindow(hwnd, GW_CHILD);
        while (c) { SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
                    c = GetWindow(c, GW_HWNDNEXT); }
        Log(L"先开 1 个游戏客户端，再按 1→2→3 顺序点，每步去游戏里验证鼠标精度，");
        Log(L"记下从哪一步开始不准，最后点 4 恢复。");
        EnableButtons(TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1) Step1();
        else if (id == 2) Step2();
        else if (id == 3) Step3();
        else if (id == 5) Step5();
        else if (id == 4) Restore();
        return 0;
    }
    case WM_ERASEBKGND: {
        // 容器顶部 34px 画成页签条示意（灰底 + 两个假页签）
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_step >= 2) {
            RECT strip = rc;
            strip.bottom = strip.top + STRIP_H;
            HBRUSH bg = CreateSolidBrush(RGB(200, 205, 212));
            FillRect(dc, &strip, bg);
            DeleteObject(bg);
            RECT t1 = { strip.left + 4, strip.top + 4, strip.left + 150, strip.bottom - 3 };
            RECT t2 = { t1.right + 2, t1.top, t1.right + 148, t1.bottom };
            HBRUSH active = CreateSolidBrush(RGB(255, 255, 255));
            HBRUSH inact = CreateSolidBrush(RGB(230, 232, 235));
            FillRect(dc, &t1, active);
            FillRect(dc, &t2, inact);
            DeleteObject(active);
            DeleteObject(inact);
            SetBkMode(dc, TRANSPARENT);
            RECT tr = t1; tr.left += 10; tr.right -= 24;
            DrawTextW(dc, L"大话西游", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            tr = t2; tr.left += 10; tr.right -= 24;
            DrawTextW(dc, L"大话西游", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_game && g_step >= 1) Restore();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show)
{
    (void)hPrev; (void)cmd;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EmbedTestMain";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"EmbedTestMain", APP,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 660, 320,
        NULL, NULL, hInst, NULL);
    (void)hwnd;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
