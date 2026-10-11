// ============================================================
// 零 SetParent 页签多开实验器 2号机 (embed_test5.c)
// 目标：验证真实多开工作流——
//   ① [开新号]：目录隔离启动一个新游戏实例（exe副本+junction+工作目录绕单实例锁）
//      （也可以自己手动开游戏，实验器会自动"收编"）
//   ② 登录该号
//   ③ 点 [1号]/[2号] 页签：把该实例的游戏窗口"贴靠收编"进容器（零SetParent，只平移）
//   ④ 点另一个页签：隐藏当前号、显示另一个号（SW_HIDE/SW_SHOW，test4已验证安全）
//   ⑤ [杀N]：模拟页签 × 关闭 = 结束该实例进程树
// 数据模型：每号记录 pid + hwnd + 原始位置；同一时刻只有一个号"激活"（可见）。
// 编译: gcc -O2 -s -municode -mwindows embed_test5.c -o embed_test5.exe -lgdi32 -luser32 -lshell32 -lcomctl32
// ============================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <wchar.h>
#include <stdio.h>

#define STRIP_H 34
#define APP     L"页签多开实验器2号机"
#define EXE_NAME    L"大话西游.exe"
#define INST_SUBDIR L"_instances"
#define MAX_INST 4

static const WCHAR *g_linkDirs[] = {
    L"font", L"img", L"inkImg", L"jre", L"resource", L"skin"
};
#define N_LINK_DIRS 6

typedef struct {
    DWORD pid;          // 实例进程（0=未启动未收编）
    HWND  hwnd;         // 已收编的游戏窗口
    RECT  orig;         // 收编前的原始位置尺寸
    BOOL  attached;     // 是否已收编
    BOOL  selfLaunched; // 是否由[开新号]启动
} Inst;

static Inst  g_inst[MAX_INST];      // 固定槽位 = 页签编号
static int   g_active = -1;         // 当前激活页签
static int   g_lastTab = -1;        // 上一个激活页签（当前号关闭时自动切过去）
static WCHAR g_gameDir[MAX_PATH];   // 游戏主目录
static WCHAR g_instRoot[MAX_PATH];
static WCHAR g_cfgIni[MAX_PATH];
static HWND  g_frame, g_log;
static HWND  g_hBtn[9];
static HFONT g_font = NULL;

// ---------------------------------------------------------------- 日志
static void Log(const WCHAR *s)
{
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    WCHAR line[600];
    swprintf(line, 600, L"%s\r\n", s);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
}
static void LogF(const WCHAR *fmt, ...)
{
    WCHAR buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 512, fmt, ap);
    va_end(ap);
    Log(buf);
}

// ---------------------------------------------------------------- 配置
static void LoadConfig(void)
{
    WCHAR base[MAX_PATH], buf[MAX_PATH];
    GetModuleFileNameW(NULL, base, MAX_PATH);
    WCHAR *p = wcsrchr(base, L'\\');
    if (p) *(p + 1) = 0;
    swprintf(g_cfgIni, MAX_PATH, L"%s贴靠实验.ini", base);
    GetPrivateProfileStringW(L"config", L"game_dir", L"", buf, MAX_PATH, g_cfgIni);
    if (buf[0]) lstrcpynW(g_gameDir, buf, MAX_PATH);
    else        lstrcpynW(g_gameDir, base, MAX_PATH);   // 默认=本exe所在目录
    swprintf(g_instRoot, MAX_PATH, L"%s\\%s", g_gameDir, INST_SUBDIR);
}
static void SaveConfig(void)
{
    WritePrivateProfileStringW(L"config", L"game_dir", g_gameDir, g_cfgIni);
}

static void PickGameDir(void)
{
    BROWSEINFOW bi;
    WCHAR path[MAX_PATH];
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g_frame;
    bi.lpszTitle = L"选择游戏主目录（含 大话西游.exe）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, path)) {
        lstrcpynW(g_gameDir, path, MAX_PATH);
        swprintf(g_instRoot, MAX_PATH, L"%s\\%s", g_gameDir, INST_SUBDIR);
        SaveConfig();
        LogF(L"[目录] 游戏主目录 = %s", g_gameDir);
    }
    CoTaskMemFree(pidl);
}

// ---------------------------------------------------------------- 目录隔离启动（搬自 duokaiqi.c，已验证）
static void MakeJunction(const WCHAR *dst, const WCHAR *src)
{
    WCHAR cmd[MAX_PATH * 4 + 64];
    swprintf(cmd, MAX_PATH * 4 + 64, L"/c mklink /J \"%s\" \"%s\"", dst, src);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static BOOL PrepareInstance(int idx, WCHAR *exeOut)
{
    WCHAR dir[MAX_PATH], srcExe[MAX_PATH], dstExe[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%s\\%02d", g_instRoot, idx);
    CreateDirectoryW(dir, NULL);
    swprintf(srcExe, MAX_PATH, L"%s\\%s", g_gameDir, EXE_NAME);
    swprintf(dstExe, MAX_PATH, L"%s\\%s", dir, EXE_NAME);
    BOOL need = TRUE;
    WIN32_FIND_DATAW fs, fd;
    HANDLE h = FindFirstFileW(srcExe, &fs);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        HANDLE d = FindFirstFileW(dstExe, &fd);
        if (d != INVALID_HANDLE_VALUE) {
            FindClose(d);
            if (fs.nFileSizeLow == fd.nFileSizeLow &&
                fs.nFileSizeHigh == fd.nFileSizeHigh &&
                CompareFileTime(&fs.ftLastWriteTime, &fd.ftLastWriteTime) <= 0)
                need = FALSE;
        }
    }
    if (need && !CopyFileW(srcExe, dstExe, FALSE)) return FALSE;
    for (int i = 0; i < N_LINK_DIRS; i++) {
        WCHAR s[MAX_PATH], t[MAX_PATH];
        swprintf(s, MAX_PATH, L"%s\\%s", g_gameDir, g_linkDirs[i]);
        swprintf(t, MAX_PATH, L"%s\\%s", dir, g_linkDirs[i]);
        if (GetFileAttributesW(t) == INVALID_FILE_ATTRIBUTES)
            MakeJunction(t, s);
    }
    lstrcpynW(exeOut, dstExe, MAX_PATH);
    return TRUE;
}

static void LaunchNew(void)
{
    // 找一个空槽位
    int slot = -1;
    for (int i = 0; i < MAX_INST; i++)
        if (!g_inst[i].pid) { slot = i; break; }
    if (slot < 0) { Log(L"[开新号] 页签已满。"); return; }

    WCHAR exe[MAX_PATH];
    if (!PrepareInstance(slot, exe)) {
        LogF(L"[开新号] %d 号：准备实例目录失败（检查游戏目录设置）。", slot + 1);
        return;
    }
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = exe;
    sei.lpDirectory = g_gameDir;   // 工作目录=游戏主目录（绕单实例锁的关键）
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        LogF(L"[开新号] 启动失败（错误码 %d）。", GetLastError());
        return;
    }
    g_inst[slot].pid = GetProcessId(sei.hProcess);
    g_inst[slot].selfLaunched = TRUE;
    CloseHandle(sei.hProcess);
    LogF(L"[开新号] %d 号已启动 (pid=%u)。请登录，然后点 [%d号] 页签贴靠收编。",
         slot + 1, g_inst[slot].pid, slot + 1);
}

// ---------------------------------------------------------------- 收编/切换
static POINT GameTarget(void)
{
    POINT p = { 0, STRIP_H };
    ClientToScreen(g_frame, &p);
    return p;
}

typedef struct {
    DWORD skip[8]; int nSkip;
    HWND found;
} AdoptCtx;

static BOOL CALLBACK AdoptCb(HWND hwnd, LPARAM lp)
{
    AdoptCtx *c = (AdoptCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    for (int i = 0; i < c->nSkip; i++)
        if (c->skip[i] == pid) return TRUE;      // 已被其他页签占用
    WCHAR txt[64];
    GetWindowTextW(hwnd, txt, 64);
    if (!wcsstr(txt, L"大话西游")) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if ((r.right - r.left) < 400 || (r.bottom - r.top) < 300) return TRUE; // 排除小窗
    RECT b;
    if (c->found) {
        GetWindowRect(c->found, &b);
        if ((r.right - r.left) * (r.bottom - r.top) <=
            (b.right - b.left) * (b.bottom - b.top)) return TRUE;
    }
    c->found = hwnd;
    return TRUE;
}

// 按 pid 找窗口；pid=0 则"收编"一个未被占用的游戏窗口
static HWND FindInstWindow(int slot)
{
    AdoptCtx c;
    ZeroMemory(&c, sizeof(c));
    for (int i = 0; i < MAX_INST; i++)
        if (i != slot && g_inst[i].pid) c.skip[c.nSkip++] = g_inst[i].pid;
    EnumWindows(AdoptCb, (LPARAM)&c);
    if (g_inst[slot].pid && c.found) {
        DWORD pid = 0;
        GetWindowThreadProcessId(c.found, &pid);
        if (pid != g_inst[slot].pid) return NULL;   // pid 对不上就不收
    }
    return c.found;
}

static void FitFrame(BOOL reposition)
{
    RECT r;
    GetWindowRect(g_inst[g_active].hwnd, &r);
    LONG W = r.right - r.left, H = r.bottom - r.top;
    RECT need = { 0, 0, W, STRIP_H + H };
    AdjustWindowRect(&need, WS_OVERLAPPEDWINDOW, FALSE);
    LONG x, y;
    if (reposition) {
        // 仅首次收编：容器放到游戏旁边
        POINT tp = GameTarget();
        x = tp.x; y = tp.y - STRIP_H;
    } else {
        // 之后只调尺寸不挪位置：游戏迁就容器，避免 AWT 把隐藏窗口
        // 挪回原始位置时把容器一路"拖"向屏幕下方
        RECT f;
        GetWindowRect(g_frame, &f);
        x = f.left; y = f.top;
    }
    SetWindowPos(g_frame, NULL, x, y,
                 need.right - need.left, need.bottom - need.top,
                 SWP_NOZORDER);
}

static BOOL g_framePlaced = FALSE;   // 容器是否已定位过

static void ActivateTab(int slot)
{
    Inst *t = &g_inst[slot];
    // 找窗口：selfLaunched 按 pid 精确找；手动开的按"未占用"收编
    if (!t->hwnd || !IsWindow(t->hwnd)) {
        t->hwnd = FindInstWindow(slot);
        if (!t->hwnd) {
            if (t->pid) LogF(L"[%d号] 还没找到游戏窗口（可能仍在启动/登录中），稍后再点。", slot + 1);
            else        LogF(L"[%d号] 该页签未绑定实例。点[开新号]或自己开一个游戏后再点本页签收编。", slot + 1);
            return;
        }
        GetWindowThreadProcessId(t->hwnd, &t->pid);
    }
    // 隐藏当前激活页（记下它是"上一个激活的"，当前号关闭时自动切回）
    if (g_active >= 0 && g_active != slot &&
        g_inst[g_active].hwnd && IsWindow(g_inst[g_active].hwnd)) {
        ShowWindow(g_inst[g_active].hwnd, SW_HIDE);
        g_lastTab = g_active;
    }
    // 首次收编：记原始位置、定位并调整容器
    if (!t->attached) {
        GetWindowRect(t->hwnd, &t->orig);
        t->attached = TRUE;
        LogF(L"[%d号] 已收编窗口 %p（零SetParent，只平移）。", slot + 1, t->hwnd);
    }
    g_active = slot;
    FitFrame(!g_framePlaced);
    g_framePlaced = TRUE;
    POINT p = GameTarget();
    SetWindowPos(t->hwnd, NULL, p.x, p.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(t->hwnd, SW_SHOW);
    // z-order：游戏在最上、容器紧贴其下
    SetWindowPos(g_frame, t->hwnd, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    LogF(L"[%d号] 已激活显示。点其他页签切换，右键流程用[杀N]按钮模拟×。", slot + 1);
}

// ---------------------------------------------------------------- 关闭页签 = 用户手动关游戏
// （杀进程按钮已按用户要求移除：直接关闭游戏，句柄失效后页签自动释放更可靠）

// ---------------------------------------------------------------- 钉扎循环

// 激活号意外死亡（用户手动关闭）→ 自动切到最近激活的其他存活号
static void AutoSwitchAfterClose(int deadSlot)
{
    // 首选上一个激活的页签
    if (g_lastTab >= 0 && g_lastTab != deadSlot &&
        g_inst[g_lastTab].attached && g_inst[g_lastTab].hwnd &&
        IsWindow(g_inst[g_lastTab].hwnd)) {
        LogF(L"[%d号] 已关闭，自动切换到 [%d号]。", deadSlot + 1, g_lastTab + 1);
        ActivateTab(g_lastTab);
        return;
    }
    // 兜底：扫描任意存活的已收编号
    for (int i = 0; i < MAX_INST; i++) {
        if (i == deadSlot) continue;
        if (g_inst[i].attached && g_inst[i].hwnd && IsWindow(g_inst[i].hwnd)) {
            LogF(L"[%d号] 已关闭，自动切换到 [%d号]。", deadSlot + 1, i + 1);
            ActivateTab(i);
            return;
        }
    }
    WCHAR msg[64];
    swprintf(msg, 64, L"[%d号] 已关闭，没有其他已开启的号。", deadSlot + 1);
    Log(msg);
}

static void PinLoop(void)
{
    if (g_active < 0) return;
    Inst *t = &g_inst[g_active];
    if (!t->hwnd || !IsWindow(t->hwnd)) {
        LogF(L"[%d号] 游戏窗口已关闭。页签释放。", g_active + 1);
        int dead = g_active;
        ZeroMemory(t, sizeof(Inst));
        g_active = -1;
        AutoSwitchAfterClose(dead);
        return;
    }
    POINT p = GameTarget();
    RECT r;
    GetWindowRect(t->hwnd, &r);
    if (r.left != p.x || r.top != p.y)
        SetWindowPos(t->hwnd, NULL, p.x, p.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // 容器始终压在激活游戏下方
    if (GetWindow(g_frame, GW_HWNDPREV) != t->hwnd)
        SetWindowPos(g_frame, t->hwnd, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- 容器过程
#define ID_BTN_NEW   100
#define ID_BTN_TAB1  101
#define ID_BTN_TAB2  102
#define ID_BTN_TAB3  103
#define ID_BTN_TAB4  104
#define ID_BTN_DIR   120

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hwnd, 1, 200, NULL);
        int x = 4;
        g_hBtn[0] = CreateWindowW(L"BUTTON", L"开新号",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, 4, 76, 26, hwnd, (HMENU)ID_BTN_NEW, NULL, NULL);
        x += 82;
        for (int i = 0; i < MAX_INST; i++) {
            WCHAR t[8];
            swprintf(t, 8, L"%d号", i + 1);
            g_hBtn[1 + i] = CreateWindowW(L"BUTTON", t,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x, 4, 50, 26, hwnd, (HMENU)(ID_BTN_TAB1 + i), NULL, NULL);
            x += 56;
        }
        CreateWindowW(L"BUTTON", L"选目录",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x + 4, 4, 64, 26, hwnd, (HMENU)ID_BTN_DIR, NULL, NULL);
        g_log = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_READONLY,
            0, STRIP_H, 900, 400, hwnd, (HMENU)200, NULL, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return 0;
    }
    case WM_TIMER:
        PinLoop();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_NEW:   LaunchNew(); break;
        case ID_BTN_DIR:   PickGameDir(); break;
        case ID_BTN_TAB1: case ID_BTN_TAB2:
        case ID_BTN_TAB3: case ID_BTN_TAB4:
            ActivateTab(LOWORD(wp) - ID_BTN_TAB1);
            break;
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *pDpi)(DPI_AWARENESS_CONTEXT) =
        (BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT))
        (void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (pDpi) pDpi((DPI_AWARENESS_CONTEXT)-4);

    LoadConfig();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"EmbedT5Frame";
    RegisterClassW(&wc);

    g_frame = CreateWindowW(L"EmbedT5Frame", APP,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        60, 60, 940, 540, NULL, NULL, hi, NULL);
    LogF(L"[就绪] 游戏主目录 = %s", g_gameDir);
    Log(L"[流程] ①点[开新号]（或自己手动开游戏）→ ②登录 → ③点[1号]/[2号]页签收编贴靠");
    Log(L"[流程] ④点另一页签切换显示 → ⑤关闭游戏直接在游戏内退出，页签自动释放并切换。");
    Log(L"[提示] 若目录不对，点[选目录]选择含 大话西游.exe 的游戏主目录。");
    (void)show;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
