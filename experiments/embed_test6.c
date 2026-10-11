// ============================================================
// 页签多开实验器 3号机 (embed_test6.c) —— UI 规格落地版
// 基于 test5d（零SetParent贴靠+自动切换，已验证）+ 第七章 UI 规格：
//   - 自绘页签条兼标题栏（32px 深色）：状态点(绿在线/黄启动中/灰掉线)
//   - 页签：左键切换/收编 · 中键=收起该号 · 悬停显示× · 双击改名
//   - + = 开新号（目录隔离启动）；掉线号页签变灰保留，点击原槽位重开
//   - 右上 —/× 最小化到托盘；☰ 菜单：诊断日志 / 帮助 / 退出
//   - 托盘：左键还原，右键菜单（各号直切/诊断/帮助/退出）
//   - 快捷键：Ctrl+1..4 切换 · Ctrl+T 开新号 · Ctrl+W 收起当前号
//   - 零动画：切换 = 一次 ShowWindow
//   - 诊断日志 = 独立弹出窗，☰ 或 Ctrl+Shift+P 呼出
// 编译: gcc -O2 -s -municode -mwindows embed_test6.c -o embed_test6.exe -lgdi32 -luser32 -lshell32 -lole32
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

#define STRIP_H   32
#define APP       L"页签多开器 3号机"
#define EXE_NAME    L"大话西游.exe"
#define INST_SUBDIR L"_instances"
#define MAX_INST 6
#define WM_TRAY  (WM_APP + 1)
#define WM_APP_HOTKEY (WM_APP + 3)   // 实例间快捷键转发
#define OWN_PROP L"YunHaiTabOwned"   // 收编归属标记（值为收编者容器 HWND，跨实例防重复收编+快捷键路由）

// 页签状态
#define ST_EMPTY 0   // 空槽
#define ST_LAUNCH 1  // 已启动未收编（黄）
#define ST_ALIVE 2   // 已收编（绿）
#define ST_DEAD  3   // 掉线/被关（灰，保留可重开）

typedef struct {
    DWORD pid;
    HWND  hwnd;
    RECT  orig;
    BOOL  attached;
    int   state;
    WCHAR name[32];
} Inst;

static Inst  g_inst[MAX_INST];
static int   g_active = -1, g_lastTab = -1;
static WCHAR g_gameDir[MAX_PATH], g_instRoot[MAX_PATH], g_cfgIni[MAX_PATH];
static HWND  g_frame, g_logWin, g_logEdit, g_renameEdit = NULL;
static int   g_renameSlot = -1;
static int   g_hoverTab = -1;       // 悬停的页签（画×用）
static BOOL  g_balloonShown = FALSE;
static BOOL  g_framePlaced = FALSE;
static HHOOK g_hKbdHook = NULL;     // 低级键盘钩子（每实例独立监听 Ctrl 快捷键，无转发）

static void ForceForeground(HWND h);   // 置前辅助（定义在钩子段，ActivateTab 先用）
static void ForegroundGuardStart(int slot);  // 二次置前守护（异步抢前台的对手存在）
static int  g_fgRetry = 0, g_fgSlot = -1;
static HFONT g_font = NULL;
static NOTIFYICONDATAW g_nid;

void AutoSwitchAfterClose(int deadSlot);   // 前置声明：收起时自动切号

// ---------------------------------------------------------------- 日志
static void Log(const WCHAR *s)
{
    if (!g_logEdit) return;
    int len = GetWindowTextLengthW(g_logEdit);
    SendMessageW(g_logEdit, EM_SETSEL, len, len);
    WCHAR line[600];
    swprintf(line, 600, L"%s\r\n", s);
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
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
static int   g_instIdx = 1;    // 实例编号（锁文件认领，1=主实例用旧 ini 名）
static HANDLE g_lock = NULL;   // 本实例的锁文件句柄（退出自动释放）

static void LoadConfig(void)
{
    WCHAR base[MAX_PATH], buf[MAX_PATH];
    GetModuleFileNameW(NULL, base, MAX_PATH);
    WCHAR *p = wcsrchr(base, L'\\');
    if (p) *(p + 1) = 0;
    if (g_instIdx == 1) swprintf(g_cfgIni, MAX_PATH, L"%s页签多开器.ini", base);
    else                swprintf(g_cfgIni, MAX_PATH, L"%s页签多开器%d.ini", base, g_instIdx);
    GetPrivateProfileStringW(L"config", L"game_dir", L"", buf, MAX_PATH, g_cfgIni);
    if (!buf[0]) {
        // 兼容：从 2号机的 ini 继承游戏目录
        WCHAR old[MAX_PATH];
        swprintf(old, MAX_PATH, L"%s贴靠实验.ini", base);
        GetPrivateProfileStringW(L"config", L"game_dir", L"", buf, MAX_PATH, old);
    }
    if (buf[0]) lstrcpynW(g_gameDir, buf, MAX_PATH);
    else        lstrcpynW(g_gameDir, base, MAX_PATH);
    swprintf(g_instRoot, MAX_PATH, L"%s\\%s", g_gameDir, INST_SUBDIR);
}
static void SaveConfig(void)
{
    WritePrivateProfileStringW(L"config", L"game_dir", g_gameDir, g_cfgIni);
    WritePrivateProfileStringW(L"names", NULL, NULL, g_cfgIni);   // 清旧节
    for (int i = 0; i < MAX_INST; i++) {
        WCHAR key[8];
        swprintf(key, 8, L"slot%d", i);
        WritePrivateProfileStringW(L"names", key, g_inst[i].name, g_cfgIni);
    }
}

// ---------------------------------------------------------------- 目录隔离启动（同 test5）
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
    static const WCHAR *linkDirs[] = { L"font", L"img", L"inkImg", L"jre", L"resource", L"skin" };
    for (int i = 0; i < 6; i++) {
        WCHAR s[MAX_PATH], t[MAX_PATH];
        swprintf(s, MAX_PATH, L"%s\\%s", g_gameDir, linkDirs[i]);
        swprintf(t, MAX_PATH, L"%s\\%s", dir, linkDirs[i]);
        if (GetFileAttributesW(t) == INVALID_FILE_ATTRIBUTES)
            MakeJunction(t, s);
    }
    lstrcpynW(exeOut, dstExe, MAX_PATH);
    return TRUE;
}
static void LaunchNew(int slot)
{
    if (slot < 0) {
        for (int i = 0; i < MAX_INST; i++)
            if (g_inst[i].state == ST_EMPTY || g_inst[i].state == ST_DEAD) { slot = i; break; }
        if (slot < 0) { Log(L"[开新号] 没有空闲页签（掉线的页签点击可原槽位重开）。"); return; }
    }
    if (g_inst[slot].state == ST_ALIVE || g_inst[slot].state == ST_LAUNCH) return;
    WCHAR exe[MAX_PATH];
    if (!PrepareInstance(slot, exe)) {
        LogF(L"[开新号] %d 号：准备实例目录失败（游戏目录不对？当前=%s）。", slot + 1, g_gameDir);
        // 日志窗可能隐藏着——失败必须弹窗让用户看见
        if (!IsWindowVisible(g_logWin))
            MessageBoxW(NULL, L"开新号失败：请点 ☰ 菜单选「选目录」设置游戏主目录（含 大话西游.exe）。\r\n（详情见诊断日志）",
                        APP, MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        return;
    }
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = exe;
    sei.lpDirectory = g_gameDir;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        LogF(L"[开新号] 启动失败（错误码 %d）。", GetLastError());
        return;
    }
    g_inst[slot].pid = GetProcessId(sei.hProcess);
    g_inst[slot].hwnd = NULL;
    g_inst[slot].attached = FALSE;
    g_inst[slot].state = ST_LAUNCH;
    if (!g_inst[slot].name[0])
        swprintf(g_inst[slot].name, 32, L"%d号", slot + 1);
    CloseHandle(sei.hProcess);
    LogF(L"[%s] 已启动 (pid=%u)，登录后点该页签收编。", g_inst[slot].name, g_inst[slot].pid);
    InvalidateRect(g_frame, NULL, FALSE);
}

// ---------------------------------------------------------------- 收编/切换（同 test5 逻辑 + 状态）
static POINT GameTarget(void)
{
    POINT p = { 0, STRIP_H };
    ClientToScreen(g_frame, &p);
    return p;
}
typedef struct { DWORD skip[8]; int nSkip; HWND found; } AdoptCtx;
static BOOL CALLBACK AdoptCb(HWND hwnd, LPARAM lp)
{
    AdoptCtx *c = (AdoptCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    for (int i = 0; i < c->nSkip; i++)
        if (c->skip[i] == pid) return TRUE;
    // 已被任何多开器实例收编的窗口不再收编（跨实例防重复）
    if (GetPropW(hwnd, OWN_PROP)) return TRUE;
    WCHAR txt[64];
    GetWindowTextW(hwnd, txt, 64);
    if (!wcsstr(txt, L"大话西游")) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if ((r.right - r.left) < 400 || (r.bottom - r.top) < 300) return TRUE;
    RECT b;
    if (c->found) {
        GetWindowRect(c->found, &b);
        if ((r.right - r.left) * (r.bottom - r.top) <=
            (b.right - b.left) * (b.bottom - b.top)) return TRUE;
    }
    c->found = hwnd;
    return TRUE;
}
static HWND FindInstWindow(int slot)
{
    AdoptCtx c;
    ZeroMemory(&c, sizeof(c));
    for (int i = 0; i < MAX_INST; i++)
        if (i != slot && g_inst[i].hwnd) c.skip[c.nSkip++] = g_inst[i].pid;
    EnumWindows(AdoptCb, (LPARAM)&c);
    if (g_inst[slot].pid && c.found) {
        DWORD pid = 0;
        GetWindowThreadProcessId(c.found, &pid);
        if (pid != g_inst[slot].pid) return NULL;
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
        POINT tp = GameTarget();
        x = tp.x; y = tp.y - STRIP_H;
    } else {
        RECT f;
        GetWindowRect(g_frame, &f);
        x = f.left; y = f.top;
    }
    SetWindowPos(g_frame, NULL, x, y,
                 need.right - need.left, need.bottom - need.top,
                 SWP_NOZORDER);
}
// 收起：游戏放回原位，页签保留（绿→黄，仍绑定 pid 可再次收编）
static void ReleaseSlot(int slot, BOOL keepBinding)
{
    Inst *t = &g_inst[slot];
    if (t->attached && t->hwnd && IsWindow(t->hwnd)) {
        SetWindowPos(t->hwnd, NULL, t->orig.left, t->orig.top,
                     t->orig.right - t->orig.left, t->orig.bottom - t->orig.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    t->attached = FALSE;
    if (!keepBinding) t->hwnd = NULL;
    if (g_active == slot) {
        g_active = -1;
        AutoSwitchAfterClose(slot);
    }
    InvalidateRect(g_frame, NULL, FALSE);
}
// 进程是否还活着（查退出码）
static BOOL PidAlive(DWORD pid)
{
    if (!pid) return FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code == STILL_ACTIVE;
}

// 二次置前守护：切换后有异步对手（游戏 AWT 线程对失活的延迟反应）会抢回前台，
// 80ms 一拍盯着，被抢就夺回，最多 5 拍（0.4s），成功或超时即停
static void ForegroundGuardStart(int slot)
{
    g_fgSlot = slot;
    g_fgRetry = 0;
    SetTimer(g_frame, 2, 80, NULL);
}
static void ForegroundGuardTick(void)
{
    if (g_fgSlot < 0 || g_fgSlot >= MAX_INST) { KillTimer(g_frame, 2); return; }
    Inst *t = &g_inst[g_fgSlot];
    if (!t->attached || !t->hwnd || !IsWindow(t->hwnd)) { KillTimer(g_frame, 2); g_fgSlot = -1; return; }
    if (GetForegroundWindow() == t->hwnd) {
        if (g_fgRetry) LogF(L"[诊断] %s 二次置前 %d 次后稳定。", t->name, g_fgRetry);
        KillTimer(g_frame, 2);
        g_fgSlot = -1;
        return;
    }
    if (++g_fgRetry >= 5) {
        LogF(L"[诊断] %s 二次置前 5 次仍未夺回前台，放弃（fg 被外部占用）。", t->name);
        KillTimer(g_frame, 2);
        g_fgSlot = -1;
        return;
    }
    ForceForeground(t->hwnd);
}

static void ActivateTab(int slot)
{
    Inst *t = &g_inst[slot];
    if (t->state == ST_EMPTY) {
        // 空槽：先尝试收编一个未被占用的手动游戏；没有才开新号
        HWND h0 = FindInstWindow(slot);
        if (!h0) { LaunchNew(slot); return; }
        t->hwnd = h0;
        GetWindowThreadProcessId(h0, &t->pid);
        t->state = ST_LAUNCH;
        LogF(L"[空槽] 发现未收编的游戏窗口 %p，收编到本页签。", h0);
    } else if (t->state == ST_DEAD) {
        LaunchNew(slot);   // 掉线页签：点击 = 原槽位重开
        return;
    }
    if (!t->hwnd || !IsWindow(t->hwnd)) {
        t->hwnd = FindInstWindow(slot);
        if (!t->hwnd) {
            // 启动中但找不到窗口：检查进程是否已被关掉（收编前关闭游戏会走到这）
            if (!PidAlive(t->pid)) {
                LogF(L"[%s] 启动后未登录就被关闭，自动原槽位重开。", t->name);
                t->state = ST_DEAD;
                LaunchNew(slot);
            } else {
                LogF(L"[%s] 还没找到游戏窗口（仍在启动/登录中？），稍后再点。", t->name);
            }
            return;
        }
        GetWindowThreadProcessId(t->hwnd, &t->pid);
    }
    // 旧号先不隐藏：等新号摆好后再藏（老代码在这里隐藏前台窗口，
    // 会让系统自动激活 z 序里的"下一个窗口"，落到谁身上不可控——
    // 这是快捷键切换时乱跳/容器异常的根源）
    int prev = g_active;
    BOOL hidePrev = prev >= 0 && prev != slot &&
                    g_inst[prev].hwnd && IsWindow(g_inst[prev].hwnd);
    if (hidePrev) g_lastTab = prev;
    if (!t->attached) {
        GetWindowRect(t->hwnd, &t->orig);
        t->attached = TRUE;
        t->state = ST_ALIVE;
        SetPropW(t->hwnd, OWN_PROP, (HANDLE)g_frame);   // 归属标记=收编者容器（跨实例防重复收编+快捷键路由）
        LogF(L"[%s] 已收编窗口 %p。", t->name, t->hwnd);
    }
    g_active = slot;
    FitFrame(!g_framePlaced);
    g_framePlaced = TRUE;
    POINT p = GameTarget();
    SetWindowPos(t->hwnd, NULL, p.x, p.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // 顺序关键：先置前新号，再藏旧号。
    // 1) 置前时旧号还活着且是前台，AttachThreadInput 挂的是健康线程；
    // 2) 隐藏旧号时它已不是前台——隐藏非前台窗口不会触发系统激活乱流
    //    （旧顺序"先藏前台窗口"会导致系统乱激活 + 与 AWT 纠缠 → 容器被最小化）。
    ShowWindow(t->hwnd, SW_SHOWNA);                    // 先可见
    SetWindowPos(g_frame, t->hwnd, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ForceForeground(t->hwnd);                          // 置前新号（内部自动选择直接/挂接路径）
    if (hidePrev) ShowWindow(g_inst[prev].hwnd, SW_HIDE);             // 再藏旧号
    ForegroundGuardStart(slot);                        // 异步抢前台的对手存在：80ms 内夺回
    LogF(L"[诊断] %s 切换后：新号fg=%d 新号最小化=%d 容器最小化=%d%s",
         t->name,
         GetForegroundWindow() == t->hwnd, IsIconic(t->hwnd), IsIconic(g_frame),
         GetForegroundWindow() == t->hwnd ? L"" : L"  ←置前失败！");
    InvalidateRect(g_frame, NULL, FALSE);
}
// 激活号死亡 → 自动切换
void AutoSwitchAfterClose(int deadSlot)
{
    if (g_lastTab >= 0 && g_lastTab != deadSlot &&
        g_inst[g_lastTab].attached && g_inst[g_lastTab].hwnd &&
        IsWindow(g_inst[g_lastTab].hwnd)) {
        LogF(L"[%s] 已关闭，自动切换到 [%s]。", g_inst[deadSlot].name, g_inst[g_lastTab].name);
        ActivateTab(g_lastTab);
        return;
    }
    for (int i = 0; i < MAX_INST; i++) {
        if (i == deadSlot) continue;
        if (g_inst[i].attached && g_inst[i].hwnd && IsWindow(g_inst[i].hwnd)) {
            LogF(L"[%s] 已关闭，自动切换到 [%s]。", g_inst[deadSlot].name, g_inst[i].name);
            ActivateTab(i);
            return;
        }
    }
    LogF(L"[%s] 已关闭，没有其他已开启的号。", g_inst[deadSlot].name);
}
static void PinLoop(void)
{
    // 监控：容器被最小化的时刻（无论谁干的、走哪条路），记录当时的前台窗口
    static BOOL s_prevIconic = FALSE;
    BOOL ic = IsIconic(g_frame);
    if (ic != s_prevIconic) {
        s_prevIconic = ic;
        if (ic) {
            HWND fg = GetForegroundWindow();
            WCHAR cls[64] = L"?";
            GetClassNameW(fg, cls, 64);
            LogF(L"[诊断] 容器进入最小化状态！当时前台=%p 类名=%s", (void*)fg, cls);
        }
    }
    // 巡检：卡在"启动中"的槽位，进程已死就转灰（颜色诚实）
    for (int i = 0; i < MAX_INST; i++) {
        Inst *t = &g_inst[i];
        if (t->state == ST_LAUNCH && !t->attached && !PidAlive(t->pid)) {
            LogF(L"[%s] 进程已退出（启动后未收编），页签转灰。", t->name);
            t->state = ST_DEAD;
            InvalidateRect(g_frame, NULL, FALSE);
        }
    }
    if (g_active < 0) return;
    Inst *t = &g_inst[g_active];
    if (!t->hwnd || !IsWindow(t->hwnd)) {
        LogF(L"[%s] 游戏窗口已关闭。页签释放（变灰保留，点击重开）。", t->name);
        if (t->hwnd) RemovePropW(t->hwnd, OWN_PROP);
        t->state = ST_DEAD;
        t->hwnd = NULL;
        t->attached = FALSE;
        int dead = g_active;
        g_active = -1;
        AutoSwitchAfterClose(dead);
        InvalidateRect(g_frame, NULL, FALSE);
        return;
    }
    POINT p = GameTarget();
    RECT r;
    GetWindowRect(t->hwnd, &r);
    if (r.left != p.x || r.top != p.y)
        SetWindowPos(t->hwnd, NULL, p.x, p.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (GetWindow(g_frame, GW_HWNDPREV) != t->hwnd)
        SetWindowPos(g_frame, t->hwnd, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- 托盘
static void TrayAdd(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_frame;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, APP);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayRemove(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// ---------------------------------------------------------------- 诊断日志窗
static WNDPROC g_oldLogProc = NULL;
static LRESULT CALLBACK LogProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

static void CreateLogWindow(void)
{
    g_logWin = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_READONLY,
        100, 620, 760, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
    g_logEdit = g_logWin;
    // 拦截 WM_CLOSE：点 × 只隐藏，窗口保活，随时可再呼出
    g_oldLogProc = (WNDPROC)(LONG_PTR)GetWindowLongPtrW(g_logWin, GWLP_WNDPROC);
    SetWindowLongPtrW(g_logWin, GWLP_WNDPROC, (LONG_PTR)(void *)LogProc);
}
static void ShowHelp(void)
{
    // 关键：owner 传 NULL！弹窗若归属容器，钉扎循环把容器压到游戏下面时
    // 弹窗会被连带压下去（owned window 永远跟着 owner 的层级走）。
    // 脱离归属 + MB_TOPMOST，让弹窗像日志窗一样独立于钉扎体系。
    MessageBoxW(NULL,
        L"鼠标操作：\n"
        "  左键页签 = 切换 / 收编该号\n"
        "  中键页签 = 收起该号（放回原位）\n"
        "  悬停页签点 × = 同收起\n"
        "  双击页签 = 改名\n"
        "  双击页签条空白 = 开新号\n"
        "  点击灰色掉线页签 = 原槽位重开\n\n"
        "快捷键：\n"
        "  Ctrl+1..6 = 直切对应号\n"
        "  Ctrl+T = 开新号\n"
        "  Ctrl+W = 收起当前号\n"
        "  Ctrl+Shift+P = 诊断日志\n"
        "  多实例：快捷键作用于\"当前活动窗口所属\"的那个多开器\n\n"
        "窗口：\n"
        "  拖页签条空白 = 移动窗口\n"
        "  — = 最小化   × = 最小化到托盘（真退出在 ☰ 菜单）",
        L"帮助 — 操作说明", MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
}

static void PickGameDir(void)
{
    // 与诊断日志同款置前方案：TOPMOST 弹跳 + 前台，不用暂停钉扎
    SetForegroundWindow(g_frame);
    SetWindowPos(g_frame, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_frame, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(g_frame);
    BROWSEINFOW bi;
    WCHAR path[MAX_PATH];
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = NULL;   // 脱离容器归属：不跟容器层级走，钉扎压不住它
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

// 诊断日志窗：点 × 只隐藏不销毁（否则关了就再也打不开）
static LRESULT CALLBACK LogProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }
    return CallWindowProcW(g_oldLogProc, h, m, wp, lp);
}

// 诊断日志：没开就开，开着（被盖住）就提到最前
static void ToggleLog(void)
{
    if (!g_logWin || !IsWindow(g_logWin)) CreateLogWindow();
    if (!IsWindowVisible(g_logWin)) ShowWindow(g_logWin, SW_SHOW);
    if (IsIconic(g_logWin)) ShowWindow(g_logWin, SW_RESTORE);
    SetWindowPos(g_logWin, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_logWin, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(g_logWin);
}

// ---------------------------------------------------------------- 页签条自绘
static void TabRects(RECT *out, int *plusX)
{
    RECT rc;
    GetClientRect(g_frame, &rc);
    int x = 6;
    for (int i = 0; i < MAX_INST; i++) {
        out[i].left = x; out[i].top = 4;
        out[i].right = x + 96; out[i].bottom = STRIP_H - 4;
        x += 102;
    }
    if (plusX) { *plusX = x + 2; }
}

static void PaintStrip(HDC dc)
{
    RECT rc;
    GetClientRect(g_frame, &rc);
    RECT strip = { 0, 0, rc.right, STRIP_H };
    FillRect(dc, &strip, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(dc, g_font);
    RECT tabs[MAX_INST];
    int plusX;
    TabRects(tabs, &plusX);
    for (int i = 0; i < MAX_INST; i++) {
        Inst *t = &g_inst[i];
        RECT r = tabs[i];
        if (t->state == ST_EMPTY) {
            // 空槽：画虚线框占位，提示可点
            HPEN dp = CreatePen(PS_DOT, 1, RGB(120,118,150));
            HPEN oldp2 = (HPEN)SelectObject(dc, dp);
            HBRUSH ob = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldb2 = (HBRUSH)SelectObject(dc, ob);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, oldp2); SelectObject(dc, oldb2);
            DeleteObject(dp);
            continue;
        }
        BOOL active = (i == g_active);
        // 整页签按状态着色（用户要求：不与容器底色混同，肉眼可辨）
        //   绿=在线  黄=启动中  红=掉线；激活页签用亮色版，非激活用暗色版
        COLORREF bg  = t->state == ST_ALIVE  ? (active ? RGB(29,158,117)  : RGB(8,80,86))
                     : t->state == ST_LAUNCH ? (active ? RGB(239,159,39)  : RGB(133,79,11))
                     :                           (active ? RGB(226,75,74)   : RGB(121,31,31));
        COLORREF txt = t->state == ST_ALIVE  ? (active ? RGB(255,255,255) : RGB(159,225,203))
                     : t->state == ST_LAUNCH ? (active ? RGB(60,40,2)     : RGB(250,199,117))
                     :                           (active ? RGB(255,255,255) : RGB(240,149,149));
        HBRUSH bgb = CreateSolidBrush(bg);
        FillRect(dc, &r, bgb);
        DeleteObject(bgb);
        // 标题
        SetTextColor(dc, txt);
        WCHAR nm[40];
        lstrcpynW(nm, t->name[0] ? t->name : L"N号", 40);
        if (t->state == ST_DEAD) wcscat(nm, L"(掉)");
        RECT tr = r; tr.left += 8; tr.right -= (i == g_hoverTab ? 20 : 6);
        DrawTextW(dc, nm, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        // 悬停 ×
        if (i == g_hoverTab) {
            SetTextColor(dc, txt);
            RECT xr = { r.right - 18, r.top, r.right, r.bottom };
            DrawTextW(dc, L"×", -1, &xr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
    }
    // + 按钮
    SetTextColor(dc, RGB(175,169,236));
    RECT pr = { plusX, 4, plusX + 26, STRIP_H - 4 };
    DrawTextW(dc, L"＋", -1, &pr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    // 右上角 — ☰ ×
    SetTextColor(dc, RGB(205,203,246));
    RECT br = { rc.right - 84, 0, rc.right - 56, STRIP_H };
    DrawTextW(dc, L"—", -1, &br, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    br.left = rc.right - 56; br.right = rc.right - 28;
    DrawTextW(dc, L"☰", -1, &br, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    br.left = rc.right - 28; br.right = rc.right;
    DrawTextW(dc, L"×", -1, &br, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    SelectObject(dc, old);
}

static int HitTab(int mx, int my)
{
    if (my >= STRIP_H) return -1;
    RECT tabs[MAX_INST];
    TabRects(tabs, NULL);
    for (int i = 0; i < MAX_INST; i++)
        if (mx >= tabs[i].left && mx < tabs[i].right &&
            my >= tabs[i].top && my < tabs[i].bottom) return i;
    return -1;
}

// ---------------------------------------------------------------- 容器过程
#define IDM_LOG   200
#define IDM_HELP  201
#define IDM_EXIT  202
#define IDM_DIR   203

// 快捷键分发：1..6=切号 7=开新号 8=收起 9=日志（本实例执行）
static void DispatchHotkey(HWND hwnd, WPARAM id)
{
    switch (id) {
    case 1: case 2: case 3: case 4: case 5: case 6:
        if ((int)id <= MAX_INST) {
            // 快捷键只切换"已收编在线"的号；空槽/掉线槽不响应（避免误开新游戏）
            Inst *t = &g_inst[(int)id - 1];
            if (t->state == ST_ALIVE && t->attached)
                ActivateTab((int)id - 1);
            else
                LogF(L"[快捷键] %d 号页签没有在线的游戏，仅切换已开启的号（开新号请用 + 或 Ctrl+T）。", (int)id);
        }
        break;
    case 7: LaunchNew(-1); break;
    case 8: if (g_active >= 0) { LogF(L"[%s] 已收起。", g_inst[g_active].name); ReleaseSlot(g_active, TRUE); } break;
    case 9: ToggleLog(); break;
    }
}

// 置前辅助：本进程没有"设置前台"权限时（如键盘钩子里响应快捷键）。
// 关键：目标窗口（新号）和当前前台（旧号）是两个不同的 Java 进程线程——
// 只挂前台线程不够，必须把【目标线程】也挂进同一输入队列，
// SetForegroundWindow 才会接受（test6p 的 fg=0 失败即此原因）。
static void ForceForeground(HWND h)
{
    if (SetForegroundWindow(h)) return;          // 本进程已是前台：直接成功
    // Alt 键脉冲：解除前台锁的经典手法（注入键带 LLKHF_INJECTED，被本钩子忽略）
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    if (SetForegroundWindow(h)) return;
    // 仍失败：双线程挂接（前台线程 + 目标线程，跨 Java 进程必须）
    HWND fg = GetForegroundWindow();
    DWORD ftFg = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD ftTg = GetWindowThreadProcessId(h, NULL);
    DWORD mt = GetCurrentThreadId();
    BOOL a1 = ftFg && ftFg != mt && AttachThreadInput(mt, ftFg, TRUE);
    BOOL a2 = ftTg && ftTg != mt && ftTg != ftFg && AttachThreadInput(mt, ftTg, TRUE);
    SetForegroundWindow(h);
    SetFocus(h);
    if (a2) AttachThreadInput(mt, ftTg, FALSE);
    if (a1) AttachThreadInput(mt, ftFg, FALSE);
}

// 低级键盘钩子：每个实例独立监听 Ctrl 快捷键，不做实例间转发。
// 规则：前台窗口归属本实例（容器或本实例收编的游戏）→ 执行并吞掉按键；
//       前台不属于本实例（其他实例的窗口/桌面/无关程序）→ 放行不处理。
static LRESULT CALLBACK KbdProc(int code, WPARAM wp, LPARAM lp)
{
    static int s_eatVK = 0;         // 已吞 keydown 的键，其 keyup 也吞（游戏不收半截按键）
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        if (!down && s_eatVK && k->vkCode == (DWORD)s_eatVK) {
            s_eatVK = 0;
            return 1;               // 配对 keyup 一并吞掉
        }
        if (down) {
            BOOL ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
            BOOL shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
            int id = 0;
            if (ctrl && !(k->flags & LLKHF_INJECTED)) {
                if (k->vkCode >= '1' && k->vkCode <= '6') id = (int)(k->vkCode - '0');
                else if (k->vkCode == 'T' && !shift) id = 7;
                else if (k->vkCode == 'W' && !shift) id = 8;
                else if (k->vkCode == 'P' && shift)  id = 9;
            }
            if (id) {
                HWND fg = GetForegroundWindow();
                BOOL mine = FALSE;
                if (fg == g_frame) {
                    mine = TRUE;                                  // 前台是本实例容器
                } else if (fg) {
                    WCHAR cls[64];
                    if (GetClassNameW(fg, cls, 64) && wcscmp(cls, L"YunHaiTabFrame") == 0)
                        mine = FALSE;                             // 前台是其他实例的容器
                    else
                        mine = (HANDLE)GetPropW(fg, OWN_PROP) == (HANDLE)g_frame;  // 前台是本实例收编的游戏
                }
                if (mine) {
                    s_eatVK = (int)k->vkCode;
                    PostMessageW(g_frame, WM_APP_HOTKEY, id, 0);
                    return 1;   // 吞掉按键，游戏/系统收不到（与 RegisterHotKey 行为一致）
                }
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        SetTimer(hwnd, 1, 200, NULL);
        // 快捷键：低级键盘钩子独立监听（不用 RegisterHotKey——它全局唯一，
        // 多实例必须有一家当中继转发；钩子方案每个实例平等，谁的窗口在前台谁切换）。
        // id 1..6 = Ctrl+1..6 切号；7 = Ctrl+T；8 = Ctrl+W；9 = Ctrl+Shift+P
        g_hKbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbdProc, GetModuleHandleW(NULL), 0);
        if (!g_hKbdHook)
            Log(L"[提示] 键盘钩子安装失败，快捷键不可用（鼠标操作不受影响）。");
        // 读回页签名
        WCHAR val[32], key[8];
        for (int i = 0; i < MAX_INST; i++) {
            swprintf(key, 8, L"slot%d", i);
            GetPrivateProfileStringW(L"names", key, L"", val, 32, g_cfgIni);
            if (val[0]) lstrcpynW(g_inst[i].name, val, 32);
        }
        CreateLogWindow();
        TrayAdd();
        return 0;
    }
    case WM_TIMER:
        if (wp == 2) { ForegroundGuardTick(); return 0; }
        PinLoop();
        return 0;
    case WM_APP_HOTKEY:                 // 键盘钩子投递的快捷键（仅前台归属本实例时）
        LogF(L"[快捷键] Ctrl+%d → 本实例处理。", (int)wp);
        DispatchHotkey(hwnd, wp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintStrip(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (my >= STRIP_H) break;
        int i = HitTab(mx, my);
        if (i >= 0) { ActivateTab(i); return 0; }
        RECT tabs[MAX_INST]; int plusX;
        TabRects(tabs, &plusX);
        if (mx >= plusX && mx <= plusX + 26) { LaunchNew(-1); return 0; }
        // 右上角按钮：— = 最小化；× = 托盘；☰ = 菜单
        RECT rc; GetClientRect(hwnd, &rc);
        if (mx >= rc.right - 84) {
            if (mx < rc.right - 56) {
                ShowWindow(hwnd, SW_MINIMIZE);  // — 普通最小化
            } else if (mx >= rc.right - 28) {
                ShowWindow(hwnd, SW_HIDE);      // × 进托盘
                if (!g_balloonShown) {
                    g_nid.uFlags = NIF_INFO;
                    lstrcpynW(g_nid.szInfo, L"已最小化到托盘，点托盘图标还原。", 256);
                    lstrcpynW(g_nid.szInfoTitle, APP, 64);
                    g_nid.dwInfoFlags = NIIF_INFO;
                    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                    g_balloonShown = TRUE;
                    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                }
            } else {
                POINT pt = { rc.right - 42, STRIP_H };
                ClientToScreen(hwnd, &pt);
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IDM_DIR,  L"选游戏目录");
                AppendMenuW(m, MF_STRING, IDM_LOG,  L"诊断日志\tCtrl+Shift+P");
                AppendMenuW(m, MF_STRING, IDM_HELP, L"帮助");
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
                AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
                int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(m);
                if (id == IDM_LOG) ToggleLog();
                else if (id == IDM_HELP) ShowHelp();
                else if (id == IDM_DIR) PickGameDir();
                else if (id == IDM_EXIT) DestroyWindow(hwnd);
            }
            return 0;
        }
        // 拖动标题栏
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    case WM_MBUTTONDOWN: {
        int i = HitTab(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i >= 0 && g_inst[i].attached) {
            LogF(L"[%s] 已收起。", g_inst[i].name);
            if (g_active == i) { g_active = -1; ReleaseSlot(i, TRUE); }
            else ReleaseSlot(i, TRUE);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int i = HitTab(mx, my);
        if (i >= 0 && g_inst[i].state != ST_EMPTY) {
            // 行内改名
            RECT tabs[MAX_INST];
            TabRects(tabs, NULL);
            g_renameSlot = i;
            g_renameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_inst[i].name,
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                tabs[i].left + 20, tabs[i].top, tabs[i].right - tabs[i].left - 20,
                tabs[i].bottom - tabs[i].top, hwnd, (HMENU)300, NULL, NULL);
            SendMessageW(g_renameEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
            SetFocus(g_renameEdit);
            SendMessageW(g_renameEdit, EM_SETSEL, 0, -1);
        } else if (i < 0 && my < STRIP_H) {
            RECT tabs[MAX_INST]; int plusX;
            TabRects(tabs, &plusX);
            if (mx < plusX) LaunchNew(-1);      // 双击空白 = 开新号
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int i = HitTab(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i != g_hoverTab) {
            g_hoverTab = i;
            if (i >= 0) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverTab = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_COMMAND:
        if (wp == 300 || HIWORD(wp) == EN_KILLFOCUS) {
            // 改名提交（Enter 或失焦）
            if (g_renameEdit && g_renameSlot >= 0) {
                WCHAR nm[32];
                GetWindowTextW(g_renameEdit, nm, 32);
                if (nm[0]) lstrcpynW(g_inst[g_renameSlot].name, nm, 32);
                DestroyWindow(g_renameEdit);
                g_renameEdit = NULL; g_renameSlot = -1;
                SaveConfig();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (lp == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            for (int i = 0; i < MAX_INST; i++) {
                if (g_inst[i].state == ST_EMPTY) continue;
                WCHAR it[48];
                swprintf(it, 48, L"切到 %s", g_inst[i].name);
                AppendMenuW(m, MF_STRING, 400 + i, it);
            }
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_DIR,  L"选游戏目录");
            AppendMenuW(m, MF_STRING, IDM_LOG,  L"诊断日志");
            AppendMenuW(m, MF_STRING, IDM_HELP, L"帮助");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
            SetForegroundWindow(hwnd);
            int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (id >= 400 && id < 400 + MAX_INST) ActivateTab(id - 400);
            else if (id == IDM_LOG) ToggleLog();
            else if (id == IDM_HELP) ShowHelp();
            else if (id == IDM_DIR) PickGameDir();
            else if (id == IDM_EXIT) DestroyWindow(hwnd);
        }
        return 0;
    case WM_SYSCOMMAND:
        // 取证+防御：正常流程里没人应该最小化容器（"—"按钮走 ShowWindow 不经过这里）。
        // 拦下并记录，说明有外部/系统行为想最小化容器。
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            LogF(L"[诊断] 拦截到容器最小化请求（SC_MINIMIZE, lp=%p），已忽略。", (void*)lp);
            return 0;
        }
        break;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DESTROY: {
        if (g_hKbdHook) { UnhookWindowsHookEx(g_hKbdHook); g_hKbdHook = NULL; }
        TrayRemove();
        SaveConfig();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 认领一个空闲实例编号（锁文件独占，进程退出自动释放）
static BOOL ClaimInstanceIdx(void)
{
    WCHAR base[MAX_PATH], lock[MAX_PATH];
    GetModuleFileNameW(NULL, base, MAX_PATH);
    WCHAR *p = wcsrchr(base, L'\\');
    if (p) *(p + 1) = 0;
    for (int i = 1; i <= 8; i++) {
        if (i == 1) swprintf(lock, MAX_PATH, L"%s页签多开器.lock", base);
        else        swprintf(lock, MAX_PATH, L"%s页签多开器%d.lock", base, i);
        HANDLE h = CreateFileW(lock, GENERIC_WRITE, 0 /*独占*/, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { g_lock = h; g_instIdx = i; return TRUE; }
    }
    return FALSE;
}

int APIENTRY wWinMain(HINSTANCE hi, HINSTANCE prev, LPWSTR cmd, int show)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *pDpi)(DPI_AWARENESS_CONTEXT) =
        (BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT))
        (void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (pDpi) pDpi((DPI_AWARENESS_CONTEXT)-4);

    // 多实例：每个实例认领一个编号（独立 ini/页签名），最多 8 个
    if (!ClaimInstanceIdx()) {
        MessageBoxW(NULL, L"多开器实例已达上限（8 个），请先关闭一些。", APP, MB_OK | MB_ICONWARNING);
        return 0;
    }
    LoadConfig();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = L"YunHaiTabFrame";
    RegisterClassW(&wc);

    WCHAR title[64];
    swprintf(title, 64, L"%s #%d", APP, g_instIdx);
    g_frame = CreateWindowW(L"YunHaiTabFrame", title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        60 + (g_instIdx - 1) * 30, 60 + (g_instIdx - 1) * 24, 900, 560,
        NULL, NULL, hi, NULL);
    LogF(L"[就绪] 实例 #%d（游戏主目录 = %s）", g_instIdx, g_gameDir);
    Log(L"[流程] + 开新号 → 登录 → 点页签收编 → 页签切换；游戏内退出自动切号。");
    Log(L"[提示] ☰ 菜单有诊断日志和帮助；× 是最小化到托盘。可再开一个多开器实例分摊页签。");
    (void)show;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_renameEdit && msg.hwnd == g_renameEdit && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(g_frame, WM_COMMAND, 300, 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                HWND e = g_renameEdit;
                g_renameEdit = NULL; g_renameSlot = -1;
                DestroyWindow(e);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_lock) CloseHandle(g_lock);
    CoUninitialize();
    return 0;
}
