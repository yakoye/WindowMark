# ClipKeeper 集成 — 设计

日期：2026-09-03
来源：把 `C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev` 并进 WindowMark，解决 ToDesk 场景下剪贴板失效

---

## 背景

ClipKeeper 是一个已经写好的独立小工具（782 行），针对的问题是：ToDesk 远程会话中截图后
`Ctrl+V` 失效——`Win+V` 历史里能看到图片，但各处粘贴都变灰。它的做法是抢占剪贴板监听链
的链头，在内容刚写入时先保存一份，随后若被清空则自动恢复。

目标是让它跟 WindowMark 一起安装、由 WindowMark 的托盘菜单开关，同时**不牺牲它现有的
救援能力**。

---

## 关键决定：保持独立进程，不合并进 WindowMark.exe

有两条硬理由，都不是风格偏好。

### 一、Viewer Chain 依赖启动顺序，而 WindowMark 开机自启

ClipKeeper 用的是 `SetClipboardViewer`（`main.cpp:313`），老的 Clipboard Viewer Chain：

- `SetClipboardViewer` 把自己插到**链头**，返回原链头
- 剪贴板变化时 Windows **只通知链头**，由链头 `SendMessage` 往下传
- 所以**谁最后注册，谁排在最前面**

ClipKeeper 的 README 让用户「在建立 ToDesk 连接后启动」正是这个原因：只有先于 ToDesk
看到内容才来得及保存。排在 ToDesk 后面，轮到自己时剪贴板已经被清空，没有东西可存。

**WindowMark 是开机自启的常驻程序，必然比 ToDesk 早。** 把代码搬进 WindowMark.exe，
这个功能会直接变弱。

### 二、链式传递会阻塞主消息循环

`main.cpp:328` 每次剪贴板变化都会往下游传：

```cpp
::SendMessageTimeoutW(g_nextViewer, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
```

在独立进程里这没问题。放进 WindowMark 的主消息循环，则下游任一监听器卡住时，**边框刷新
会跟着卡最多 250ms**——与之前 Excel 卡顿属于同类问题。

### 结论

保持 `ClipKeeper.exe` 为独立进程，与 WindowMark 一起安装、由托盘菜单启停。崩溃域隔离，
且用户可以在连上 ToDesk **之后**再启动它，天然避开顺序问题。

---

## 不开机自启

ClipKeeper **不随 Windows 启动**，也不随 WindowMark 启动。用户连上 ToDesk 后从托盘菜单
点一下开启。这是唯一能保证它排在 ToDesk 前面的做法。

它自己的「开机启动」勾选框**保留**——那是既有功能，不删。但 README 要写清楚：开了就回到
「先于 ToDesk 启动」的状态，救援能力会下降。

因为选了手动启动，**不做「自动检测 ToDesk 进程并重抢链头」**那套机制。

---

## 仓库与构建

源码复制到 WindowMark 仓库的 `src/clipkeeper/`：

```
src/clipkeeper/main.cpp
src/clipkeeper/clipboard.cpp
src/clipkeeper/clipboard.h
src/clipkeeper/resource.h
src/clipkeeper/ClipKeeper.rc
```

`CMakeLists.txt` 新增 `add_executable(ClipKeeper WIN32 ...)`，编译口径与其他目标一致：
`/W4 /WX /permissive- /utf-8 /EHsc`，链接 `user32 gdi32 shell32 advapi32 comctl32`。

**已实测**：ClipKeeper 源码在 C++20 + `/W4 /WX` 下**零代码警告**（原项目是 C++17 + `/W4`
无 `/WX`）。不需要为它放宽编译口径。

**版本号独立**：ClipKeeper 保持自己的 `0.1.0`，不跟 WindowMark 的 `0.4.x`。它是独立 exe，
有自己的演进节奏，硬拉到 0.4.6 只会让人困惑。它有完整的静态 `.rc`，不接 WindowMark 那套
`configure_file` 版本注入。

原 `ClipKeeper-dev` 目录不动，由用户自行决定归档还是删除。

---

## 两处必须改的代码

### 单例互斥量（当前缺失）

ClipKeeper 现在**没有任何单例保护**。点两次菜单会起两个实例，两个都 `SetClipboardViewer`
插进链里——链结构混乱，两份缓存互相抢救援。

加 `Local\ClipKeeper.Singleton.v1`。第二个实例的行为照搬 WindowMark 的既有模式
（`kSecondInstanceMessage`）：向已有窗口发一个 `ClipKeeper.SecondInstance.v1` 注册消息让它
把面板显示出来，然后自己退出。

**不用 `SetForegroundWindow` 抢前台**——本机实测它不可靠（前台锁），已在
`docs/Windows开发避坑规则.md` 记录。让已有实例自己 `ShowWindow` 即可。

### 两个外部消息（当前一个都没有）

`WM_CLOSE` 现在是收起到托盘（`main.cpp:393`），**这个行为正好是菜单要的「关闭面板」，不改**。
但还缺两个入口，照搬 WindowMark 已有的 `RegisterWindowMessageW` 模式：

- **`ClipKeeper.ShowPanel.v1`** — 把收起的面板叫出来。菜单项在「进程在跑但面板收起」时发它，
  第二个实例启动时也发它（见上）。
- **`ClipKeeper.RequestQuit.v1`** — 真正退出。**只有卸载器用**：运行中的 exe 删不掉。
  托盘菜单不用它，停止守护是在 ClipKeeper 面板里做的事。

退出路径本来就会 `ChangeClipboardChain`（`main.cpp:399`）干净地退出监听链，不需要改。

消息名与窗口类名 `ClipKeeperMainWindow` 一起，放进 `src/shared/ClipKeeperIdentity.h`——
`src/shared/` 是 WindowMark 已有的共享目录，安装器和主程序都从那里取标识字符串。
新开一个头文件而不是塞进 `AppIdentity.h`：那个文件是 WindowMark 自己的身份，混进另一个
产品的字符串会让两者的生命周期纠缠在一起。

需要它的三方各自 include：ClipKeeper（注册窗口类、收消息）、WindowMark（查状态、发退出
消息）、卸载器（卸载前停止它）。字符串在三处各写一份必然漂移，这正是 `AppIdentity.h`
当初存在的理由。

---

## 托盘菜单

```text
WindowMark
书签           >
窗口边框        >
窗口置顶        >
窗口拖动        >     （下一个项目，本次不加）
剪贴板守护            ← 无子菜单，带对勾，点击开启/关闭
──────────
暂停所有
开机启动
配置文件...
关于
──────────
退出
```

这个菜单项是 **ClipKeeper 面板的开关**，不是进程的开关。WindowMark 只负责「把它叫出来」，
自动救援、历史、退出这些都在 ClipKeeper 自己的面板里操作。

**对勾 = 面板当前可见**，与点击行为一一对应：

| 当前状态 | 对勾 | 点击后 |
|---|---|---|
| 进程没跑 | 无 | `CreateProcess` 启动（它启动时自带面板） |
| 面板开着 | 有 | 发 `WM_CLOSE`——它现有行为就是收起到自己的托盘，进程继续守护 |
| 在托盘里（面板已收起） | 无 | 发显示面板消息，把它叫出来 |

**状态实时读，不缓存**：每次打开菜单用 `FindWindowW(L"ClipKeeperMainWindow", nullptr)` 加
`IsWindowVisible` 查一次。用户可能从 ClipKeeper 自己的托盘收起或退出它，缓存会说谎——与
「开机启动」每次读注册表是同一个理由。

**已知的可发现性代价**：进程在后台守护但面板收起时，这里没有对勾，孤立地看会以为守护没开。
接受这个代价，因为 ClipKeeper 自己的托盘图标始终在，那才是「是否在运行」的真实指示；让对勾
去表示进程状态、点击却切换面板，两者不对应反而更糟。

**「暂停所有」不包含剪贴板守护。** 它管的是书签/边框/置顶（以后加窗口拖动），那些都是视觉
干扰，暂停通常是为了截图或演示——而截图恰恰是最需要剪贴板保护的时刻，跟着一起停是帮倒忙。
另外它是独立进程，「暂停」一个外部进程语义也不清楚。

**菜单会变宽**：「剪贴板守护」5 个字成为新的最宽顶层标签，Win32 菜单宽度由最宽标签决定。
这是明知的代价。若实际观感太宽，可缩短为「剪贴板」。

---

## 安装器

- `WindowMarkSetup.exe` 一并复制 `ClipKeeper.exe` 到安装目录
- `WindowMarkUninstall.exe` **先发退出消息停掉它，再删文件**——运行中的 exe 删不掉
- 卸载时清理 ClipKeeper 自己的 `HKCU\...\Run\ClipKeeper` 键值（用户可能开过它的自启动开关）

---

## 明确不做的

- **不把 ClipKeeper 的 UI 并进 WindowMark 的设置对话框**。它有自己的窗口和托盘图标，是完整
  独立的工具，WindowMark 只负责开关它。
- **不做自动检测 ToDesk 并重抢链头**。选了手动启动，这条路不需要。
- **不统一版本号**。理由见上。
- **不给「窗口拖动」加占位菜单项**。灰着的空项比没有更让人困惑，等做那个功能时一并加。

---

## 验证方式

| 检查 | 期望 |
|---|---|
| 全新构建 | 两个 exe 都产出，零警告 |
| 菜单项状态 | 未运行时无对勾；启动后面板可见，再开菜单有对勾 |
| 从菜单启动 | ClipKeeper 进程出现，面板和它自己的托盘图标都出现 |
| 面板开着时再点菜单 | 面板收起，**进程仍在**（托盘图标还在），对勾消失 |
| 面板收起后再点菜单 | 面板重新出现，对勾恢复 |
| 从 ClipKeeper 自己的托盘退出 | 再开 WindowMark 菜单，对勾消失且点击能重新启动（状态没有被缓存） |
| 手动双击 exe 两次 | 只有一个进程，第二次把已有面板叫出来（单例生效） |
| 安装器 | 安装目录里有 ClipKeeper.exe |
| 卸载（ClipKeeper 运行中） | 先停止再删除，目录清空无残留 |
| 剪贴板救援本身 | 连上 ToDesk 后启动它，截图后 Ctrl+V 正常 |

最后一项依赖 ToDesk 环境，由用户实机确认。

---

## 已知限制

1. **必须在 ToDesk 连接建立之后启动**才能可靠地排在它前面。这是 Clipboard Viewer Chain 的
   固有性质，不是实现缺陷。
2. **ToDesk 若在会话中重新初始化自己的剪贴板钩子**，ClipKeeper 会退到它后面。此时需要手动
   关闭再开启一次（托盘菜单点两下），重新回到链头。
3. ClipKeeper 的「自动救援」开关状态**不持久化**，每次启动都是默认开启。属既有行为，不改。
