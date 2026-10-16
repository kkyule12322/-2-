# 跨进程窗口管理实战：给 Java/AWT 游戏客户端做页签式多开器

![lang](https://img.shields.io/badge/lang-Win32%20C-C%20red) ![size](https://img.shields.io/badge/exe-40KB-green) ![deps](https://img.shields.io/badge/deps-0-blue) ![arch](https://img.shields.io/badge/arch-zero%20SetParent-8A2BE2) ![license](https://img.shields.io/badge/license-MIT-blue)

> **这个仓库的主角不是那个 exe，而是 8 条关于「如何管理别人进程的窗口」的结论。**
> 它们和游戏无关，和 AWT/Java 无关框架无关，是 Win32 跨进程窗口控制的通用坑。
> 做游戏多开、远程桌面容器、窗口自动化、任何"把别的进程窗口收编到自己框架里"的人，都会撞上同一批。

**载体**：3690 行纯 Win32 C（正式版 1077 行 + 5 个实验验证器 2613 行），零第三方依赖，产物 40KB 单文件。
**形态**：把多个游戏客户端窗口"贴靠"进一个带页签条的容器里，像浏览器标签页一样切换、改名、收起、掉线重开。**不注入、不修改游戏进程，纯窗口管理。**

---

## 一、先看这里：8 条踩坑（每一条都花了真金白银的调试时间）

这 8 条是本仓库最值钱的部分，与游戏无关。

| # | 结论 | 一句话解释 |
|---|---|---|
| 1 | **`SetParent` 是单行道** | 对 AWT 窗口用 `SetParent`，任何脱父子事件都会让 AWT 重建原生窗口 → 你拿到白屏空壳 |
| 2 | **AWT 会"跑位"** | AWT 缓存了窗口坐标，隐藏的窗口被它挪回原位时机不可控 → 容器只首次定位，永远"游戏迁就容器" |
| 3 | **隐藏前台窗口 = 激活乱流** | 先隐藏当前前台窗口，系统会乱激活 z 序里"下一个" → **必须先置前新号、再隐藏旧号** |
| 4 | **跨进程置前要三层权限** | `SetForegroundWindow` 被静默拒绝时：直接试探 → Alt 键脉冲解锁 → 双线程 `AttachThreadInput` |
| 5 | **抢前台是异步的** | 对手（游戏 AWT 线程）会延迟抢回前台 → 切换后要 80ms 守护定时器夺回（0.4s 上限） |
| 6 | **`RegisterHotKey` 全局唯一** | 同一快捷键全系统只能一个进程注册，多实例必然要"转发中继" → 每个实例各装 `WH_KEYBOARD_LL` 钩子 |
| 7 | **收编要用窗口属性做归属标记** | `SetPropW(hwnd, L"Owner", hFrame)` 既防重复收编，又让键盘钩子查出"这窗口归哪个实例管" |
| 8 | **弹窗被挡的真凶是归属层级** | owned window 跟随 owner 的 z 序 → 弹窗 `hwndOwner` 设 NULL + `MB_TOPMOST` 才彻底脱离容器层级 |

**下面展开讲，附完整实验代码位置：**

### 1. `SetParent` 是单行道

把 Java AWT 窗口（类名 `SunAwtToolkit`）`SetParent` 进容器后，**任何脱父子事件**（恢复、关闭容器、甚至切换显示器）都会让 AWT 销毁旧 HWND 并重建——你拿到的是一个**白屏空壳**：标题栏正常、类名正确、可见=1，但 D3D 画面永远不回来。

> 实验路径：`experiments/embed_test.c`（最初路线）→ `embed_test3.c`（发现单行道，614 行诊断日志）→ `embed_test4.c`（零 `SetParent` 验证器）

**正确姿势：零 `SetParent`**。游戏永远是顶层窗口，只用 `SetWindowPos` 平移贴靠进容器客户区，容器在 z 序上压在游戏下方；页签切换用 `SW_HIDE/SW_SHOW`——对**顶层**窗口隐藏不会触发 AWT 重建。

### 2. AWT 会"跑位" 🏃

AWT 内部缓存了窗口屏幕坐标，`SetParent` 后按缓存值自我修正，时机不可控（实测时有时无，有时延迟数百 ms）。

**方案**：容器**只首次定位**（`g_framePlaced` 标志），永远"游戏迁就容器"，不要试图让容器跟着游戏跑。钉扎循环（200ms `WM_TIMER`）只作为兜底保险，且必须配 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`，否则坐标差被 DPI 缩放放大。

### 3. 隐藏前台窗口 = 激活乱流 💥

切换页签时如果**先隐藏旧号（它正是前台窗口）**，系统被迫自动激活 z 序里"下一个"可见窗口——落到谁身上不可控，表现为**容器被最小化、活动窗口跳到别的程序/别的实例**。叠加 `AttachThreadInput` 在"濒死线程"上挂接，与 AWT 纠缠出各种怪状态。

> 真实排查记录：从 test6m 到 test6p，9 个版本才定位到这一个顺序错误

**正确顺序：先置前新号（旧号还健康）→ 再隐藏旧号**（非前台窗口的隐藏不触发任何激活行为）。

### 4 + 5. 跨进程置前的三层权限 + 异步抢前台 🎯

快捷键切换时你的进程**没有前台权限**（输入被发给了游戏进程），`SetForegroundWindow` 会被静默拒绝。三层递进：

1. **直接试探**：本进程已是前台时直接调，鼠标路径零开销
2. **Alt 键脉冲**：`keybd_event(VK_MENU, down/up)` 解锁前台锁（注入键被 `LLKHF_INJECTED` 标记天然忽略，游戏只收一瞬间 Alt 无副作用）
3. **双线程 `AttachThreadInput`**：目标窗口与前台窗口是**两个不同的 Java 进程线程**，只挂前台线程不够，**必须把目标线程也挂进同一输入队列**

即使当场成功，对手（AWT 线程对失活/隐藏的延迟反应）会在**你的日志之后**才异步抢回前台。所以还需要**第二层：80ms 守护定时器**，被抢就夺回，最多 5 拍（0.4s 上限）。

> 关键实测结论："隐藏前台窗口让系统自动激活新号"这条路线**根本不成立**（test6o 实锤：自动激活从未成功过）

### 6. `RegisterHotKey` 是全局唯一的 🔑

`Ctrl+1` 全系统只能被**一个**进程注册。多实例方案要么做"转发中继"（本项目试过，被用户否决："不应该有转发这个说法"），要么只有先启动的实例能用。

**正确姿势：每个实例装自己的 `WH_KEYBOARD_LL` 钩子**，各自判断前台窗口归属（`GetPropW` 查坑 7 的标记），归我就执行并**吞键**（`return 1`），不归我放行。**零转发、零实例间通信。**

坑：吞键必须 **keydown + keyup 配对**，否则游戏会收到孤立 keyup。

### 7. 收编要用窗口属性做归属标记 🏷️

```c
SetPropW(hwnd, L"YunHaiOwned", (HANDLE)hFrame);   // 值 = 收编者容器句柄
```

一处解决三个问题：防多实例重复收编同一个游戏 / 键盘钩子查出归属 / 快捷键三级路由判定。

### 8. 弹窗被游戏挡住的真凶：归属层级 🪟

症状：`MessageBox` 被反复压到游戏窗口下面，日志窗（无 owner）却正常。

**根因**：owned window 跟随 owner 的 z 序/层级。钉扎循环把容器压到游戏下方时，弹窗连带被压。

**方案**：弹窗 `hwndOwner = NULL` + `MB_TOPMOST | MB_SETFOREGROUND`，彻底脱离容器层级。目录选择对话框同理：`bi.hwndOwner = NULL`。

> 完整的问题 → 诊断 → 结论记录见 **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**（约 100+ 条日志级记录，含每个版本的实测结果与被否决的方案）

---

## 二、为什么是纯 Win32 C

| 方案 | 产物 | 代价 |
|---|---|---|
| **C + Win32** | **40KB 单文件** | 无运行时依赖，拷到任何 Win10/11 电脑直接跑 |
| Python + PyQt | 12.5MB | 目标机器要装 Python 或带打包器 |
| C# / .NET | 4MB+ | 要装 .NET 运行时 |

选型理由：要做的事全是 HWND 级操作（`SetWindowPos` / `AttachThreadInput` / `SetPropW` / `WH_KEYBOARD_LL`），Win32 API 是正路；起一个 JVM 或 .NET 运行时只为摆几个窗口，得不偿失。

**额外红利**：不引入运行时 = 不会被杀软误报为"游戏外挂"，也更容易分发。

---

## 三、工具本体：功能清单

- **页签条兼标题栏**：自绘 32px 深色页签条，整页签按状态着色（🟢 在线 / 🟡 启动中 / ⚪ 掉线）
- **页签操作**：左键切换/收编 · 中键收起 · 悬停显示 × · 双击改名（ini 持久化）
- **开新号**：`+` 或 `Ctrl+T`，目录隔离启动（exe 副本 + junction 共享资源，绕过 Java 单实例锁）
- **掉线处理**：掉线号页签变灰保留，点击原槽位重开；收编前关掉的号自动恢复
- **自动切换**：关闭某号自动切到最近使用过的号
- **托盘**：× 最小化到托盘，右键菜单各号直切
- **快捷键**：`Ctrl+1..6` 切换 · `Ctrl+T` 开新号 · `Ctrl+W` 收起当前号 · `Ctrl+Shift+P` 诊断日志
- **多实例**：最多 8 个实例，每个管理 6 个游戏；快捷键按前台窗口归属自动路由到对应实例
- **零动画**：切换 = 一次 `ShowWindow`

> ⚠️ 关于"杀号"：本项目**故意不提供**。`ShellExecuteEx` 返回的 pid 是 Jar2Exe 启动器的，真实 JVM 是脱钩子进程，`taskkill /T` 树快照抓不到（实测连续失败 3 次）。请手动关闭游戏——窗口句柄失效时自动释放已验证可靠。**判断进程死活要看窗口在不在，不是看 pid。**

### 架构：为什么是遮挡式而不是嵌入式

四步实验定责（`embed_test.c` 阶段，用户实测）：

| 步骤 | 操作 | 结果 |
|---|---|---|
| 1 | 仅 `SetParent`，零样式改动 | ✅ 一切正常 |
| 2 | 加页签条、整体平移 34px（**尺寸不变**） | ✅ 一切正常 |
| 3 | 去边框改 `WS_POPUP`（客户区变高 39px） | ❌ 鼠标上下不精准、画面多出一截 |
| 4 | 恢复原样 | ❌ AWT 销毁旧窗口重建 → 引出坑 1 |

**铁律：动 Java/AWT 客户端，只允许 `SetParent` + 平移，绝不能改尺寸或样式。**（正式版已进一步退化为零 `SetParent`。）

---

## 四、构建

> **📦 [Releases 页](https://github.com/kkyule12322/dahua2-multi-boxing-tool/releases) 提供了预编译的 `duokai_tab.exe`（41472 字节），不想装编译器可以直接下载。**
> 仓库里也提供全部源码，自行编译随时可复现。
>
> ```
> SHA256  97a0132ac083720726280874a0bcd60d287fd1978cdcbd964ad3d7ae9ebe16bd
> MD5     833ef9d40f6673bf8b52a8e4a21f667d
> ```
> 未签名 exe 被 SmartScreen/杀软拦是正常的，校验哈希一致即可放行。

```bash
gcc -O2 -s -municode -mwindows -finput-charset=UTF-8 \
    src/duokai_tab.c -o duokai_tab.exe -lgdi32 -luser32 -lshell32 -lole32
```

- 任何 MinGW-w64 均可（WinLibs GCC 16.2 测试通过）
- 无第三方依赖，产物约 40KB，单文件绿色便携

## 五、界面长什么样

没有截图，下面用文字描述界面结构（外层 = 容器窗口，内层 = 游戏原生窗口）：

```
┌────────────────────────────────────────────────────────┐
│ ▌1号▌ ▌2号▌ ▌3号▌ ▌4号▌▌5号▌▌6号▌   +    ─   □   ×    │ ← 自绘页签条 = 标题栏
│   ▓绿      ▓绿      ▓黄      ▓灰(掉线)                 │   整页签按状态着色
├────────────────────────────────────────────────────────┤
│ ┌────────────────────────────────────────────────────┐ │
│ │                                                    │ │
│ │              游戏窗口（原生，零样式改动）             │ │ ← 压在页签条下方
│ │         顶部 32px 被页签条遮住＝看起来像嵌进去了        │ │    鼠标精度不受影响
│ │                                                    │ │
│ └────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────┘
```

| 元素 | 行为 |
|---|---|
| 页签（整块着色） | 🟢 在线 · 🟡 启动中 · ⚪ 掉线（点它可原槽位重开） |
| `+` | 开新号：exe 副本 + junction 共享资源，绕过单实例锁 |
| `—` / `×` | 最小化到托盘 / 关闭（托盘图标右键可直切各号） |
| 双击页签 | 改名（写入 ini，重启保留） |
| 中键页签 / 悬停 `×` | 收起该号（放回原位） |
| `☰` 菜单 | 选游戏主目录 · 诊断日志 · 帮助 · 退出 |

**切换一个页签发生了什么**：先置前新号 → 容器压到它下方 → 最后隐藏旧号。整个过程对游戏进程零干预。

## 六、使用

1. 把 `duokai_tab.exe` 放进游戏主目录（与游戏主程序同目录）
2. 运行 → 点 `+` 开新号 → 登录游戏 → 点对应页签收编
3. 也可以手动多开游戏，点页签会自动"收编"游离窗口

**移植前提**：目标机器装了对应客户端即可，Win10/11 直接跑。exe 未签名，首次运行可能被 SmartScreen/杀软提示，放行即可。**没有静态路径写死**，ini 里的旧路径失效会自动回退到 exe 所在目录。

---

## 七、换成别的游戏？

收编式架构（标题匹配 + `SetPropW` 归属标记）**不挑游戏**。改造点只有两处，都在单文件源码里，改完重新编译即可：

| 要改什么 | 在哪（以 `src/duokai_tab.c` 为例） |
|---|---|
| 窗口识别规则：标题关键字、类名黑名单、尺寸下限、面积优先 | `AdoptCb()`（约 250 行），核心是 `wcsstr(txt, L"大话西游")` 那一行 |
| 单实例锁绕过方式（exe 副本 + junction 共享资源） | `PrepareInstance()` / `MakeJunction()`（约 148–200 行） |

游戏专属逻辑**没有散落在别处**——置前、页签、托盘、钩子、快捷键路由全部与具体游戏无关。**不提供"通用注入"或"通用游戏加速"等功能，也不建议用于破坏游戏规则。**

---

## 目录结构

```
src/duokai_tab.c        正式版源码（单文件，1077 行）
experiments/            实验代码：每个技术决策的最小复现（2613 行）
  embed_test.c          四步实验：真嵌入 vs 遮挡式定责
  embed_test3.c         恢复/快捷键实验 → 发现 SetParent 单行道
  embed_test4.c         零 SetParent 架构验证器
  embed_test5.c         工作流验证（开号/收编/切换/自动切换）
  embed_test6.c         UI 版（页签条/托盘/钩子/多实例的前身）
docs/DEVELOPMENT.md     开发全记录（问题→诊断→结论→实测结果）
```

---

## English summary

**Tabbed multi-instance window manager for Java/AWT game clients — and a field guide to managing other processes' windows.**

A 40KB single-file Win32 C program that docks multiple game client windows into one container with a browser-style tab bar. No injection, no game-process modification, pure window management.

The interesting part is not the tool, it's the **8 pitfalls** documented above — they apply to anyone embedding, reparenting, or stealing focus across process boundaries (AWT/Java apps, Electron apps, DirectX games, anything with an owned-window dialog):

1. `SetParent` is a one-way street for AWT windows
2. AWT caches window coordinates and "runs away"
3. Hiding the foreground window causes activation chaos — show the new one first
4. Cross-process foreground requires three escalating privilege tricks
5. Foreground stealing is **asynchronous** — you need a guard timer
6. `RegisterHotKey` is globally unique — use per-instance `WH_KEYBOARD_LL` hooks
7. Use `SetPropW` as an ownership marker for "adopted" windows
8. Owned windows inherit owner z-order — that's why your dialog hides behind the game

Zero `SetParent` in the final version: game windows stay top-level, the container positions below them in z-order, and tab switching is just `SW_HIDE`/`SW_SHOW`.

Build: `gcc -O2 -s -municode -mwindows src/duokai_tab.c -o duokai_tab.exe -lgdi32 -luser32 -lshell32 -lole32`

## License

MIT