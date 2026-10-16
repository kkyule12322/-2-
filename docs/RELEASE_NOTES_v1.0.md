# v1.0 Release notes

首个正式版。40KB 单文件 Win32 C，零第三方依赖，纯窗口管理（不注入、不修改游戏进程）。

## 产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `duokai_tab.exe` | ~40KB | 预编译可直接运行（Win10/11，依赖系统 DLL） |
| 源码 | — | `src/duokai_tab.c`（1077 行单文件）+ `experiments/`（2613 行） |

## 编译器信息

- **WinLibs GCC 16.2.0**（MinGW-w64）
- 命令：
  ```bash
  gcc -O2 -s -municode -mwindows -finput-charset=UTF-8 \
      src/duokai_tab.c -o duokai_tab.exe -lgdi32 -luser32 -lshell32 -lole32
  ```
- `-finput-charset=UTF-8` 必加：源码含中文宽字符串字面量，缺了会乱码

## 功能

- 自绘 32px 页签条兼标题栏，整页签按状态着色（🟢在线 / 🟡启动中 / ⚪掉线）
- 左键切换/收编 · 中键收起 · 悬停 × · 双击改名（ini 持久化）
- 开新号（目录隔离：exe 副本 + junction 共享资源，绕过单实例锁）
- 掉线保留页签，点原槽位重开；关号自动切到上一个号
- 托盘常驻，右键直切各号
- 快捷键：`Ctrl+1..6` / `Ctrl+T` 开新号 / `Ctrl+W` 收起 / `Ctrl+Shift+P` 诊断日志
- 多实例：最多 8 个，每个管 6 个号，快捷键按前台窗口归属自动路由

## 已知限制

- **不提供"杀号"功能**。`ShellExecuteEx` 返回的 pid 是 Jar2Exe 启动器的，真实 JVM 是脱钩子进程，`taskkill /T` 实测连续失败。请手动关闭游戏，窗口句柄失效时会自动释放页签。
- exe 未签名，首次运行可能触发 SmartScreen/杀软提示。
- 窗口识别用的是标题关键字匹配（`wcsstr(txt, L"大话西游")`），只适配类 AWT/Java 主窗口的游戏。

## 这个 Release 的重点其实不是 exe

仓库真正值钱的部分是 README 第一节的 8 条跨进程窗口管理踩坑——

`SetParent` 单行道 · AWT 坐标缓存 · 隐藏前台窗口的激活乱流 · 跨进程置前三层权限 · 异步抢前台守护 · `RegisterHotKey` 全局唯一 · `SetPropW` 归属标记 · owned window 层级

这些结论与游戏无关，适用于任何"把别的进程窗口收编到自己框架里"或"跨进程操作窗口"的场景。如果你就是来看工具的，下载即走；如果你是来解决问题的，建议直接看那 8 条。

## 使用

1. 把 `duokai_tab.exe` 放进游戏主目录
2. 运行 → 点 `+` 开新号 → 登录游戏 → 点对应页签收编

免责：本项目仅做窗口层面的管理，不注入、不修改游戏进程、不提供加速或内存修改功能。请遵守你所在的服务条款。