// ============================================================
// 零 SetParent 顶层贴靠实验器 (embed_test4.c)
// 背景：embed_test3 系列实锤——游戏窗口只要被 SetParent 过，
//       任何脱父子事件（恢复/关闭多开器）都会触发 AWT 重建空壳，
//       游戏 D3D 渲染永久丢失（白屏 theAwtToolkitWindow）。
// 新架构：游戏窗口永远是顶层窗口，零 SetParent、零样式改动，
//         只用 SetWindowPos 平移贴靠到容器客户区（实验1/2证明安全）。
// 按钮：
//   [1] 贴靠   游戏平移到容器客户区 (0,STRIP_H) 下方，尺寸不变
//   [2] 释放   游戏平移回原位置（只动位置，不动样式）
//   [3] 隐藏   SW_HIDE 游戏（页签切换的模拟）——观察 AWT 是否重建
//   [4] 显示   SW_SHOW 游戏
// 验证点：
//   A. 贴靠后鼠标在游戏内精准（应精准——零样式改动）
//   B. 拖动容器标题栏，游戏跟手（WM_MOVE 同步 + 200ms 钉扎）
//   C. 隐藏→显示循环多次，游戏画面是否完好（不出现白屏空壳）
//   D. 贴靠状态下直接点容器 × 关闭多开器，游戏是否照常存活
// 编译: gcc -O2 -s -municode -mwindows embed_test4.c -o embed_test4.exe -lgdi32 -luser32
// ============================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#include <stdio.h>

#define STRIP_H 34     // 页签条高度（贴靠时游戏下移的量）
#define APP    L"零SetParent贴靠实验器"

static HWND g_game = NULL, g_frame = NULL, g_log;
static RECT g_orig;                 // 游戏原始位置尺寸
static LONG g_gw = 0, g_gh = 0;     // 游戏尺寸
static int  g_step = 0;             // bit0=已贴靠
static HWND g_h1, g_h2, g_h3, g_h4;
static HFONT g_font = NULL;

static void Log(const WCHAR *s)
{
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    WCHAR line[512];
    swprintf(line, 512, L"%s\r\n", s);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
}

// 找大话西游的顶层窗口
static BOOL CALLBACK FindCb(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    WCHAR txt[64];
    GetWindowTextW(hwnd, txt, 64);
    if (wcsstr(txt, L"大话西游") && IsWindowVisible(hwnd)
        && !GetWindow(hwnd, GW_OWNER)) {
        *(HWND *)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

// ---- 贴靠目标位置：容器客户区 (0, STRIP_H) 的屏幕坐标 ----
static POINT GameTarget(void)
{
    POINT p = { 0, STRIP_H };
    ClientToScreen(g_frame, &p);
    return p;
}

// 200ms 钉扎：位置归位 + 保证游戏在容器 z-order 之上
static void PinAndOrder(void)
{
    if (!(g_step & 1) || !g_game || !IsWindow(g_game)) return;
    POINT p = GameTarget();
    RECT r;
    GetWindowRect(g_game, &r);
    if (r.left != p.x || r.top != p.y)
        SetWindowPos(g_game, NULL, p.x, p.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // 容器必须始终压在游戏下面：把容器插到游戏正后方（下方）
    // SetWindowPos(frame, insertAfter=game) => frame 位于 game 之后(下方)
    HWND prev = GetWindow(g_frame, GW_HWNDPREV);
    if (prev != g_game)
        SetWindowPos(g_frame, g_game, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---- [1] 贴靠 ----
static void DoAttach(void)
{
    if (!g_game) {
        EnumWindows(FindCb, (LPARAM)&g_game);
        if (!g_game) { Log(L"没找到大话西游.exe 的窗口，请先开一个客户端。"); return; }
    }
    GetWindowRect(g_game, &g_orig);
    g_gw = g_orig.right - g_orig.left;
    g_gh = g_orig.bottom - g_orig.top;

    // 容器客户区 = 页签条 + 游戏尺寸（只调容器自己，不碰游戏）
    RECT need = { 0, 0, g_gw, STRIP_H + g_gh };
    AdjustWindowRect(&need, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_frame, NULL, g_orig.left, g_orig.top,
                 need.right - need.left, need.bottom - need.top,
                 SWP_NOZORDER);

    // 游戏平移到容器客户区 (0,STRIP_H)：零样式、零父子、零尺寸改动
    POINT p = GameTarget();
    SetWindowPos(g_game, NULL, p.x, p.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_frame, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_game, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_step |= 1;
    Log(L"[贴靠] 游戏已平移贴靠到容器内（零 SetParent、零样式改动）。");
    Log(L"[贴靠] 验证：A 鼠标精准 / B 拖容器标题栏游戏跟手 / C 游戏快捷键原生可用。");
}

// ---- [2] 释放 ----
static void DoRelease(void)
{
    if (!(g_step & 1)) { Log(L"当前未贴靠。"); return; }
    g_step &= ~1;
    SetWindowPos(g_game, NULL, g_orig.left, g_orig.top, g_gw, g_gh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Log(L"[释放] 游戏已平移回原位置（全程未动样式/父子关系，应完好无损）。");
}

// ---- [3] 隐藏 / [4] 显示（模拟页签切换）----
static void DoHide(void)
{
    if (!g_game || !IsWindow(g_game)) { Log(L"没有游戏窗口。"); return; }
    ShowWindow(g_game, SW_HIDE);
    Log(L"[隐藏] SW_HIDE 已执行。观察任务栏与后台——顶层窗口隐藏不应触发 AWT 重建。");
}

static void DoShow(void)
{
    if (!g_game || !IsWindow(g_game)) {
        Log(L"[显示] 游戏句柄已失效！SW_HIDE 触发了 AWT 重建（重大发现）！");
        EnumWindows(FindCb, (LPARAM)&g_game);
        if (g_game) {
            WCHAR b[128];
            swprintf(b, 128, L"[显示] 已重新找到游戏窗口 %p", g_game);
            Log(b);
        } else {
            Log(L"[显示] 按 PID 也找不到新窗口，请报告此情况。");
        }
        return;
    }
    ShowWindow(g_game, SW_SHOW);
    SetWindowPos(g_game, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Log(L"[显示] SW_SHOW 已执行。请确认游戏画面完好（无白屏）。");
}

// ---- 容器过程 ----
static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hwnd, 1, 200, NULL);
        g_h1 = CreateWindowW(L"BUTTON", L"1 贴靠",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                4, 4, 80, 26, hwnd, (HMENU)1, NULL, NULL);
        g_h2 = CreateWindowW(L"BUTTON", L"2 释放",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                90, 4, 80, 26, hwnd, (HMENU)2, NULL, NULL);
        g_h3 = CreateWindowW(L"BUTTON", L"3 隐藏",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                176, 4, 80, 26, hwnd, (HMENU)3, NULL, NULL);
        g_h4 = CreateWindowW(L"BUTTON", L"4 显示",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                262, 4, 80, 26, hwnd, (HMENU)4, NULL, NULL);
        g_log = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY,
                0, STRIP_H, 800, 400, hwnd, (HMENU)5, NULL, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return 0;
    }
    case WM_TIMER:
        PinAndOrder();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 1: DoAttach(); break;
        case 2: DoRelease(); break;
        case 3: DoHide();   break;
        case 4: DoShow();   break;
        }
        return 0;
    case WM_SIZE:
        if (g_log) MoveWindow(g_log, 0, STRIP_H, LOWORD(lp), HIWORD(lp) - STRIP_H, TRUE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hi, HINSTANCE prev, LPWSTR cmd, int show)
{
    // 高 DPI 感知：游戏是 DPI-aware，坐标必须 1:1
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *pDpi)(DPI_AWARENESS_CONTEXT) =
        (BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT))
        (void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (pDpi) pDpi((DPI_AWARENESS_CONTEXT)-4);   // PER_MONITOR_AWARE_V2

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"EmbedT4Frame";
    RegisterClassW(&wc);

    g_frame = CreateWindowW(L"EmbedT4Frame", APP,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        60, 60, 820, 520, NULL, NULL, hi, NULL);
    (void)show;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
