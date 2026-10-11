// ============================================================
// 遮挡式页签实验器 (embed_test3.c)
// 方案：游戏窗口零样式改动（只 SetParent + 平移，实验1/2已证明安全），
//       工具自己的"标题栏+页签条"叠在游戏原生标题栏上面，把它盖住。
//   容器（自绘标题栏 30px：可拖动 + 最小化/关闭）
//   ├── 页签条 capH px（盖住游戏原生标题栏区域）
//   └── 游戏窗口（含原生边框，位置只平移、尺寸不变）
// 按钮：
//   [1] 嵌入+遮挡   [2] 揭开页签条（露出游戏原生标题栏证明它还在）
//   [3] 重新盖上    [4] 恢复原样
// 验证点：
//   A. 鼠标在游戏内是否精准（应精准——没动游戏样式）
//   B. 页签条是否严丝合缝盖住原生标题栏
//   C. 拖动容器标题栏、最小化/关闭是否正常
// 编译: gcc -O2 -s -municode -mwindows embed_test3.c -o embed_test3.exe -lgdi32 -luser32
// ============================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <wchar.h>
#include <stdio.h>

#define TBAR_H 30      // 容器自绘标题栏高
#define APP    L"遮挡式页签实验器"

static HWND g_game = NULL, g_container = NULL, g_strip = NULL;
static RECT g_orig;
static LONG g_origStyle = 0;
static LONG g_capH = 31;          // 游戏原生标题栏(+顶边框)高，运行时计算
static LONG g_gw = 0, g_gh = 0;   // 游戏窗口应保持的尺寸
static int  g_step = 0;
static HWND g_log, g_h1, g_h2, g_h3, g_h4;
static HFONT g_font = NULL;

static void SyncStrip(void);   // 前置声明：页签条跟随
static void AttachGameInput(void);  // 前置声明：键盘输入焊接
static void DetachGameInput(void);  // 前置声明：解除键盘焊接

static void Log(const WCHAR *s)
{
    int n = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void EnableButtons(BOOL e)
{
    EnableWindow(g_h1, e && g_step == 0);
    EnableWindow(g_h2, e && g_step == 1);
    EnableWindow(g_h3, e && g_step == 1);
    EnableWindow(g_h4, e && g_step >= 1);
}

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
    if (!isGame || !IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) <= 0
        || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if (g_game) {
        RECT best;
        GetWindowRect(g_game, &best);
        if ((r.right - r.left) * (r.bottom - r.top) <=
            (best.right - best.left) * (best.bottom - best.top))
            return TRUE;
    }
    g_game = hwnd;
    (void)lp;
    return TRUE;
}

// ---- 页签条子窗口（自绘：两个页签 + 加号）----
// 页签矩形（客户区坐标），点击命中与绘制共用
static RECT g_tabRc[3];    // 0=1号 1=2号 2=加号

static void LayoutTabs(HDC dc, RECT rc)
{
    g_tabRc[0].left = rc.left + 4;   g_tabRc[0].top = rc.top + 4;
    g_tabRc[0].right = rc.left + 150; g_tabRc[0].bottom = rc.bottom;
    g_tabRc[1].left = g_tabRc[0].right + 2; g_tabRc[1].top = rc.top + 6;
    g_tabRc[1].right = g_tabRc[1].left + 148; g_tabRc[1].bottom = rc.bottom;
    g_tabRc[2].left = g_tabRc[1].right + 4; g_tabRc[2].top = rc.top;
    g_tabRc[2].right = g_tabRc[2].left + 30; g_tabRc[2].bottom = rc.bottom;
}

static LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(196, 200, 208));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        LayoutTabs(dc, rc);
        RECT t1 = g_tabRc[0], t2 = g_tabRc[1];
        t1.bottom = rc.bottom;
        HBRUSH act = CreateSolidBrush(RGB(255, 255, 255));
        HBRUSH ina = CreateSolidBrush(RGB(228, 230, 234));
        FillRect(dc, &t1, act);
        FillRect(dc, &t2, ina);
        DeleteObject(act);
        DeleteObject(ina);
        SetBkMode(dc, TRANSPARENT);
        RECT tr = t1;
        tr.right -= 22;
        DrawTextW(dc, L"大话西游 1号", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        tr = t2;
        tr.right -= 22;
        DrawTextW(dc, L"大话西游 2号", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SetTextColor(dc, RGB(60, 60, 60));
        DrawTextW(dc, L"+", -1, &g_tabRc[2], DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        return 1;
    }
    if (msg == WM_LBUTTONDOWN) {
        // 诊断：点击是否真的到达页签条
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_tabRc[0], pt)) Log(L"[页签] 点到了「1号」页签 —— 点击通路正常！");
        else if (PtInRect(&g_tabRc[1], pt)) Log(L"[页签] 点到了「2号」页签 —— 点击通路正常！");
        else if (PtInRect(&g_tabRc[2], pt)) Log(L"[页签] 点到了「+」号 —— 点击通路正常！");
        else {
            WCHAR b[128];
            swprintf(b, 128, L"[页签] 点到了页签条空白处 (%ld,%ld)", pt.x, pt.y);
            Log(b);
        }
        return 0;
    }
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD g_pid = 0;           // 游戏进程 PID（恢复时重找窗口用）
static WCHAR g_origCls[64] = L""; // 游戏主窗口类名（恢复时严格匹配，防止抓到 D3DFocusWindow 之类辅助窗口）

typedef struct {
    HWND strict;   // PID 相同 + 类名与原窗口一致（最佳）
    long sArea;
    HWND loose;    // PID 相同 + 有标题 + 非辅助类（兜底）
    long lArea;
} FindCtx;

static BOOL CALLBACK FindPidCb(HWND hwnd, LPARAM lp)
{
    // 恢复时重找游戏主窗口：按 PID + 原类名严格匹配，防止抓到
    // D3DFocusWindow 等渲染辅助窗口（它们可能同名同尺寸但不是主窗口）
    FindCtx *ctx = (FindCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_pid) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    WCHAR cls[64];
    GetClassNameW(hwnd, cls, 64);
    // 排除已知辅助窗口类
    if (wcsstr(cls, L"D3DFocusWindow") || wcsstr(cls, L"JavaAWT")
        || wcsstr(cls, L"MSCTFIME") || wcsstr(cls, L"Default IME")) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area <= 0) return TRUE;
    BOOL vis = IsWindowVisible(hwnd);
    long weight = area + (vis ? 0x40000000 : 0);   // 可见优先
    if (g_origCls[0] && wcscmp(cls, g_origCls) == 0) {
        if (!ctx->strict || weight > ctx->sArea) {
            ctx->strict = hwnd; ctx->sArea = weight;
        }
    }
    if (GetWindowTextLengthW(hwnd) > 0) {
        if (!ctx->loose || weight > ctx->lArea) {
            ctx->loose = hwnd; ctx->lArea = weight;
        }
    }
    return TRUE;
}

static HWND FindGameWindow(void)
{
    FindCtx ctx = { 0 };
    EnumWindows(FindPidCb, (LPARAM)&ctx);
    return ctx.strict ? ctx.strict : ctx.loose;
}

static DWORD WINAPI RestoreWatch(LPVOID)
{
    // 恢复后 3 秒内强制把游戏钉在原始位置并保持可见 + 每 0.5s 记录诊断
    DWORD start = GetTickCount();
    DWORD lastLog = 0;
    while (GetTickCount() - start < 3000) {
        HWND h = g_game;
        if (!h || !IsWindow(h)) {
            // AWT 可能在恢复过程中销毁旧窗口、重建新窗口——按 PID 重新找
            Sleep(300);
            HWND h2 = FindGameWindow();
            if (h2) {
                g_game = h2;
                WCHAR b[128];
                swprintf(b, 128, L"[恢复诊断] 检测到游戏重建窗口，新句柄=%p", h2);
                Log(b);
                h = h2;
            } else {
                Log(L"[恢复诊断] 游戏窗口句柄失效且未找到新窗口！");
                break;
            }
        }
        RECT r;
        GetWindowRect(h, &r);
        DWORD st = (DWORD)GetWindowLongW(h, GWL_STYLE);
        BOOL vis = IsWindowVisible(h);
        if (GetTickCount() - lastLog >= 500) {
            WCHAR cls[64], txt[64], b[512];
            GetClassNameW(h, cls, 64);
            GetWindowTextW(h, txt, 64);
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            swprintf(b, 512,
                L"[恢复诊断] 可见=%d 披风=%d 最小化=%d 位置=(%ld,%ld) 尺寸=(%ldx%ld) 样式=%08X 扩展=%08X 类=%s 标题=%s",
                vis, cloaked, IsIconic(h), r.left, r.top,
                r.right - r.left, r.bottom - r.top, st,
                (DWORD)GetWindowLongW(h, GWL_EXSTYLE), cls, txt);
            Log(b);
            lastLog = GetTickCount();
        }
        if (!vis || IsIconic(h) ||
            r.left != g_orig.left || r.top != g_orig.top ||
            (r.right - r.left) != g_gw || (r.bottom - r.top) != g_gh) {
            ShowWindow(h, SW_RESTORE);
            SetWindowPos(h, HWND_TOP, g_orig.left, g_orig.top, g_gw, g_gh,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
        // 顽固隐身：窗口自称可见、位置也对，却看不见——DWM 披风状态，硬复活
        {
            DWORD cloaked2 = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked2, sizeof(cloaked2));
            static int revived = 0;
            if (cloaked2 && revived < 3) {
                ShowWindow(h, SW_HIDE);
                Sleep(80);
                ShowWindow(h, SW_SHOW);
                SetForegroundWindow(h);
                revived++;
                Log(L"[恢复] 检测到披风隐藏，执行硬复活 (HIDE→SHOW→前台)");
            }
        }
        Sleep(100);
    }
    Log(L"[恢复诊断] 监视结束。");
    return 0;
}

static void DoRestore(void)
{
    if (!g_game) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(g_game, &pid);
    g_pid = pid;
    if (g_container && GetParent(g_game) == g_container) {
        KillTimer(g_container, 3);
        SetParent(g_game, NULL);
    }
    // AWT 放回桌面时可能销毁旧窗口、重建新 HWND——等一拍按 PID 重找
    Sleep(200);
    HWND h2 = FindGameWindow();
    if (h2 && h2 != g_game) {
        WCHAR cls[64];
        GetClassNameW(h2, cls, 64);
        WCHAR b[192];
        swprintf(b, 192, L"[恢复] 检测到游戏重建了窗口，切换到新句柄 %p 类=%s", h2, cls);
        Log(b);
        g_game = h2;    // 新窗口自带正常样式，不再改样式
    } else {
        SetWindowLongW(g_game, GWL_STYLE, g_origStyle);
    }
    SetWindowPos(g_game, HWND_TOP, g_orig.left, g_orig.top,
                 g_orig.right - g_orig.left, g_orig.bottom - g_orig.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(g_game, SW_RESTORE);
    ShowWindow(g_game, SW_SHOW);
    DetachGameInput();
    if (g_container) {
        DestroyWindow(g_container);
        g_container = NULL; g_strip = NULL;
    }
    g_step = 0;
    Log(L"[恢复] 游戏窗口已完全还原（3 秒内持续强制归位）。");
    CloseHandle(CreateThread(NULL, 0, RestoreWatch, NULL, 0, NULL));
    EnableButtons(TRUE);
}

// ---- 容器过程：自绘标题栏 + 拖动 + 最小化/关闭 + 钉扎循环 ----
static LRESULT CALLBACK ContProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        // 钉扎：游戏内部缓存了自己的屏幕坐标，被 SetParent 后会"跑位"——
        // 每当它挪走就按回 (0, TBAR_H)，尺寸钉回 g_gw x g_gh
        if (wp == 3 && g_game && GetParent(g_game) == hwnd) {
            POINT p0 = { 0, 0 };
            MapWindowPoints(g_game, hwnd, &p0, 1);
            RECT gr;
            GetWindowRect(g_game, &gr);
            LONG w = gr.right - gr.left, h = gr.bottom - gr.top;
            if (p0.x != 0 || p0.y != TBAR_H || w != g_gw || h != g_gh)
                SetWindowPos(g_game, NULL, 0, TBAR_H, g_gw, g_gh,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            // 页签条跟随（独立置顶窗，永远在游戏上面）
            SyncStrip();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        // 键盘焦点在容器/页签上时，把按键原样转发给游戏（恢复 ALT+E 等快捷键）
        if (g_game && IsWindow(g_game))
            PostMessageW(g_game, msg, wp, lp);
        return 0;
    case WM_PARENTNOTIFY:
        // 点进游戏时把键盘焦点交给游戏
        if (LOWORD(wp) == WM_LBUTTONDOWN && (HWND)lp == g_game)
            AttachGameInput();
        return 0;
    case WM_ACTIVATE:
        // 容器被激活时，焦点直接给游戏子窗口
        if (LOWORD(wp) != WA_INACTIVE && g_game && IsWindow(g_game))
            AttachGameInput();
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        SyncStrip();
        return 0;
    case WM_NCHITTEST: {
        LRESULT ht = DefWindowProcW(hwnd, msg, wp, lp);
        if (ht == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (pt.y < TBAR_H) {
                // 右上角按钮区
                if (pt.x > rc.right - 60) return HTCLIENT;
                return HTCAPTION;              // 标题栏拖动
            }
        }
        return ht;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (pt.y < TBAR_H && pt.x > rc.right - 60) {
            if (pt.x > rc.right - 30) {        // 关闭
                DoRestore();
                PostQuitMessage(0);
            } else {                            // 最小化
                ShowWindow(hwnd, SW_MINIMIZE);
            }
            return 0;
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT bar = rc;
        bar.bottom = TBAR_H;
        HBRUSH bg = CreateSolidBrush(RGB(52, 110, 190));
        FillRect(dc, &bar, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT tr = bar;
        tr.left += 10;
        tr.right -= 70;
        DrawTextW(dc, L"遮挡式页签实验器", -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT bmin = { rc.right - 60, 4, rc.right - 34, TBAR_H - 4 };
        RECT bx  = { rc.right - 30, 4, rc.right - 6, TBAR_H - 4 };
        DrawTextW(dc, L"—", -1, &bmin, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        DrawTextW(dc, L"×", -1, &bx,   DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top += TBAR_H;
        HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        return 1;
    }
    case WM_CLOSE:
        DoRestore();
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 把容器线程与游戏线程的输入队列连起来，否则跨进程 SetFocus 无效
static BOOL  g_inputAttached = FALSE;
static DWORD g_gameThread    = 0;

static void AttachGameInput(void)
{
    if (!g_game || !IsWindow(g_game)) return;
    g_gameThread = GetWindowThreadProcessId(g_game, NULL);
    DWORD mt = GetCurrentThreadId();
    if (!g_inputAttached) {
        if (AttachThreadInput(mt, g_gameThread, TRUE)) {
            g_inputAttached = TRUE;
            Log(L"[键盘] 已连接游戏线程输入队列 (AttachThreadInput)");
        } else {
            Log(L"[键盘] AttachThreadInput 失败！");
        }
    }
    SetFocus(g_game);
}

// 恢复时解除输入焊接——不解开会让游戏窗口的激活状态悬空
static void DetachGameInput(void)
{
    if (g_inputAttached && g_gameThread) {
        AttachThreadInput(GetCurrentThreadId(), g_gameThread, FALSE);
        g_inputAttached = FALSE;
        Log(L"[键盘] 已解除游戏线程输入焊接");
    }
}

// ---- 步骤1：嵌入 + 遮挡 ----
static void SyncStrip(void)
{
    // 页签条是独立置顶窗口：贴在容器客户区 (0, TBAR_H)，即游戏原生标题栏上
    if (!g_container || !g_strip) return;
    POINT p = { 0, TBAR_H };
    ClientToScreen(g_container, &p);
    SetWindowPos(g_strip, HWND_TOPMOST, p.x, p.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

static void Step1(void)
{
    if (!g_game) {
        EnumWindows(FindCb, 0);
        if (!g_game) { Log(L"没找到大话西游.exe 的窗口，请先开一个客户端。"); return; }
    }
    GetWindowRect(g_game, &g_orig);
    g_origStyle = GetWindowLongW(g_game, GWL_STYLE);
    GetClassNameW(g_game, g_origCls, 64);   // 记录主窗口类名，恢复时严格匹配

    // 计算游戏原生标题栏（含顶边框）高度 = 窗口顶到客户区的距离
    RECT pad = { 0, 0, 0, 0 };
    AdjustWindowRect(&pad, (DWORD)g_origStyle, FALSE);
    g_capH = -pad.top;

    LONG W = g_orig.right - g_orig.left;
    LONG H = g_orig.bottom - g_orig.top;
    g_gw = W;
    g_gh = H;

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW cc = {0};
    cc.lpfnWndProc = ContProc;
    cc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    cc.lpszClassName = L"EmbedT3C";
    cc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&cc);
    WNDCLASSW sc = {0};
    sc.lpfnWndProc = StripProc;
    sc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    sc.lpszClassName = L"EmbedT3Strip";
    RegisterClassW(&sc);

    g_container = CreateWindowExW(WS_EX_TOOLWINDOW, L"EmbedT3C", APP,
        WS_POPUP | WS_VISIBLE, g_orig.left, g_orig.top, W, TBAR_H + H,
        NULL, NULL, hi, NULL);

    // 游戏进容器：只平移到标题栏下方（尺寸不变，样式不变）
    SetParent(g_game, g_container);
    SetWindowPos(g_game, NULL, 0, TBAR_H, W, H, SWP_NOZORDER | SWP_NOACTIVATE);

    // 页签条 = 独立置顶小窗（NOACTIVATE 不抢焦点），层级高于容器内一切子窗口，
    // 游戏在容器里怎么提升自己都盖不住它
    g_strip = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"EmbedT3Strip", NULL, WS_POPUP | WS_VISIBLE,
        0, 0, W, g_capH, g_container, NULL, hi, NULL);
    SetWindowPos(g_strip, HWND_TOPMOST, 0, 0, W, g_capH, SWP_NOMOVE | SWP_NOACTIVATE);

    // 启动钉扎循环（防游戏自己跑位）
    SetTimer(g_container, 3, 200, NULL);
    SyncStrip();

    // 键盘：连接输入队列并把焦点交给游戏（恢复 ALT+E 等快捷键）
    AttachGameInput();

    g_step = 1;
    EnableButtons(TRUE);
    Log(L"[步骤1] 已嵌入+遮挡。游戏窗口只 SetParent+平移，样式未动。");
    Log(L"  验证 A：游戏内鼠标是否精准（应精准）。");
    Log(L"  验证 B：页签条是否严丝合缝盖住原生标题栏。");
    Log(L"  验证 C：拖动容器蓝色标题栏、右上角 —/× 是否正常。");
    Log(L"  揭开/重新盖上按钮可确认原生标题栏仍完好。");
}

static void Step2(void)   // 揭开：临时隐藏页签条，露出游戏原生标题栏
{
    if (!g_strip) return;
    ShowWindow(g_strip, SW_HIDE);
    Log(L"[步骤2] 已揭开：现在应能看到游戏原生标题栏原样存在。");
}

static void Step3(void)   // 重新盖上
{
    if (!g_strip) return;
    ShowWindow(g_strip, SW_SHOWNOACTIVATE);
    SyncStrip();
    Log(L"[步骤3] 已重新盖上。");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_h1 = CreateWindowW(L"BUTTON", L"1 嵌入+遮挡", WS_CHILD | WS_VISIBLE,
                             10, 10, 120, 30, hwnd, (HMENU)1, NULL, NULL);
        g_h2 = CreateWindowW(L"BUTTON", L"2 揭开页签条", WS_CHILD | WS_VISIBLE,
                             140, 10, 120, 30, hwnd, (HMENU)2, NULL, NULL);
        g_h3 = CreateWindowW(L"BUTTON", L"3 重新盖上", WS_CHILD | WS_VISIBLE,
                             270, 10, 110, 30, hwnd, (HMENU)3, NULL, NULL);
        g_h4 = CreateWindowW(L"BUTTON", L"4 恢复原样", WS_CHILD | WS_VISIBLE,
                             390, 10, 110, 30, hwnd, (HMENU)4, NULL, NULL);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                  10, 50, 620, 210, hwnd, (HMENU)5, NULL, NULL);
        HWND c = GetWindow(hwnd, GW_CHILD);
        while (c) { SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
                    c = GetWindow(c, GW_HWNDNEXT); }
        Log(L"先开 1 个游戏客户端，点 [1 嵌入+遮挡]，然后验证 A/B/C 三项。");
        EnableButtons(TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1) Step1();
        else if (id == 2) Step2();
        else if (id == 3) Step3();
        else if (id == 4) DoRestore();
        return 0;
    }
    case WM_DESTROY:
        if (g_game && g_step >= 1) DoRestore();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show)
{
    (void)hPrev; (void)cmd;
    // DPI 感知：游戏(Java)是 DPI-aware 的，若我们不感知，坐标会差一个缩放系数
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *pSetCtx)(HANDLE) =
        (BOOL (WINAPI *)(HANDLE))GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (pSetCtx) pSetCtx((HANDLE)-4);       // PER_MONITOR_AWARE_V2
    else SetProcessDPIAware();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EmbedTest3Main";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"EmbedTest3Main", APP,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 660, 330,
        NULL, NULL, hInst, NULL);
    (void)hwnd;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
