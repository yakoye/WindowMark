# WindowMark v0.4.5

WindowMark is a lightweight Windows utility for **multi-window bookmarks, per-window
borders, and temporary always-on-top pinning**.

If three independent VS Code windows are open, all three windows receive the same three bookmarks. Clicking any bookmark immediately activates the corresponding VS Code window. Chrome, Explorer, SiYuan, terminals, and other ordinary top-level applications use the same mechanism without app-specific plugins.

## v0.4.3 at a glance

This Windows release combines three independent tools in one ordinary user process:

- **Window bookmarks** for identifying, previewing, renaming, and switching between
  multiple windows of the same application.
- **Window borders** for outlining every eligible top-level window with separate active
  and inactive colors. Borders are optional and disabled by default.
- **Window pinning** through the target window's system menu, a crosshair picker, or an
  optional global hotkey. Pinned windows always receive a visible highlight.

v0.4.3 fixes **开机启动** reporting and registration. The tray menu now checks both the
actual Run command and Windows' separate `StartupApproved` veto, enabling writes the Run
command before clearing that veto, and failures are reported instead of silently ignored.
Logon launches carry `--autostart` and append their startup phases to
`%LOCALAPPDATA%\WindowMark\startup.log`, so a failed launch can be distinguished from
Windows never attempting it.

v0.4.2 added per-application border exclusions and fixed the shared selection panel's
checkbox round trip. v0.4.1 added window pinning. See [CHANGELOG.md](CHANGELOG.md) for the
full history.

## Quick start

1. Download `WindowMark-v0.4.5-win64.zip` from the
   [v0.4.5 release](https://github.com/yakoye/WindowMark/releases/tag/v0.4.5).
2. Extract it and run `WindowMarkSetup.exe`. To use it without installing, run
   `WindowMark.exe` directly from the extracted directory.
3. Open at least two normal windows from the same application to see bookmarks.
4. Use the tray menu to configure **书签**, **窗口边框**, and **窗口置顶** independently.

The functional release targets Windows 10/11. The macOS directory remains an architecture
scaffold and does not provide a working macOS application in v0.4.5.

## Interaction

Normal windows prefer the **left outside edge**. Each bookmark has its own stable pastel/rainbow color during the process lifetime.

```text
        compact                         hover B

     [ A… ]                          [ A… ]
     [ B… ]     VS Code              [         PCIe Tool         ]  VS Code
     [ C… ]                          [ C… ]
```

Only the hovered bookmark expands. The others remain compact. `Self` (the window that owns this bookmark strip) and `Active` (the current foreground window) are separate states.

When a host window is maximized, automatic layout moves the bookmarks to a compact row at the **bottom edge**. Hovering a non-self bookmark waits for the configured preview delay and then shows one DWM thumbnail preview; only one preview exists at a time.

## Window borders

WindowMark also draws a coloured outline around windows, with separate colours for the
active and inactive window. The corner radius follows the system per window, so it matches
Windows 11's rounding and stays square on Windows 10.

This is **independent of bookmarks**: its own switch, its own settings window, its own
tray submenu, and it covers every top-level window — including single-window apps that
never get a bookmark strip. Turn it on under **窗口边框 → 启用窗口边框**, or set
`border.enabled=true`. It is off by default.

The idea comes from [tacky-borders](https://github.com/lukeyou05/tacky-borders); the
implementation here is native rather than a bundled copy of it. That project is Rust and
this one is C++, so bundling would have meant a second process with its own window hooks
and tray icon, doubling the event handling for something this app's window tracking and
layered rendering already do.

## 窗口置顶

把任意窗口钉在最上层，并给它画一条高亮边框作为「已置顶」的提示。

**三种触发方式**，都不需要往别的程序里注入任何东西：

1. **标题栏右击 →「❏置于顶层」**。最顺手的一种。WindowMark 用 `GetSystemMenu` +
   `InsertMenuItemW` 往对方的系统菜单里插一项，点击通过 `EVENT_OBJECT_INVOKED` 收到——
   全程只用公开 API，不加载 DLL，不改对方的窗口过程。
2. **托盘 →「窗口置顶」→「⊕抓取窗口置顶...」**。屏幕上出现一个准星手柄，**按住它拖到目标
   窗口上松开**即可，跟 Spy++ 找窗口是同一个手势。拖动过程中被指到的窗口会实时高亮，所以
   松手前就能确认钉的是哪个。Esc、在手柄上右键、15 秒无操作都可取消。
3. **全局快捷键**（默认不设）。在「置顶设置」里按下想要的组合键即可，退格清除。

置顶状态**只存在于本次会话**。程序退出时会把每个窗口恢复成置顶之前的样子——包括本来就
是置顶的那些，它们不会被误取消。

### 已知限制

UWP / WinUI 窗口（「设置」、部分商店应用）**加不上菜单项**，这是平台限制不是缺陷：

- `Windows.UI.Core.CoreWindow`：`GetSystemMenu` 直接返回 0
- `ApplicationFrameWindow`：返回一个非空句柄，但 `GetMenuItemCount` 是 -1、
  `InsertMenuItemW` 报 `1401 ERROR_INVALID_MENU_HANDLE`——那个菜单归 `ApplicationFrameHost`
  所有，跨进程写不了

PowerToys 也进不去。这类窗口请用准星或快捷键。

### 颜色怎么选

边框和置顶的颜色都是一排色卡：**6 个预设 + 1 个自定义**。

| 位置 | 内容 |
|---|---|
| 第 1 格 | 该项原来的默认值——边框活动 `#6274E7`、边框非活动 `#7080AA`、置顶「跟随系统强调色」 |
| 第 2–6 格 | 红 `#E81123`、橙 `#F7630C`、黄 `#FFB900`、绿 `#16C60C`、紫 `#8E4EC6` |
| 第 7 格「⋯」 | 打开系统取色器，任意颜色 |

白色不在预设里：白边框在浅色桌面上等于没画。

色卡右边写着当前色值。置顶那一项写的是「跟随系统 #0078D4」——**后面那个值是实时从注册表读的
真实强调色**，不是占位符。换了系统主题，这里和边框一起跟着变。

键盘：Tab 进焦点，← → 切换，空格/回车确认，Home/End 跳到两端。

配置文件里仍然是纯文本，可以手改：`pin.color=accent` 或 `pin.color=#E81123`。

### 快捷键

默认**不占用**任何组合键。`RegisterHotKey` 是先到先得且**输的一方不会收到任何通知**，
所以除非你明说，这个程序不会去抢。

- 设置位置：托盘 →「窗口置顶」→「置顶设置...」→「快捷键」
- 直接按下想要的组合键，框里会显示规范写法；**退格键清除**
- 必须带至少一个修饰键。否则等于把一个光秃秃的按键从全系统抢走
- 组合已被别的程序占用时，会**明确弹框告诉你**，而不是留一个按了没反应的快捷键
- 手改配置文件也行：`pin.hotkey=Ctrl+Alt+T`。大小写随意，`Win`/`Windows`/`Meta` 等价，
  支持 `F1`–`F24`、字母数字和 `Space`/`Enter`/`Home`/`PageUp` 这类具名键。写错等于不设，
  不会导致启动失败

快捷键作用于**当前前台窗口**。托盘菜单做不到这一点——菜单一打开，前台就变成 WindowMark
自己了，没有「当前窗口」可读；快捷键不夺取前台，所以可以。

### 排除不想要边框的应用

托盘 →「窗口边框」→「排除应用...」，**勾上的应用不画边框**。
应用层的选择长期保存，单个窗口的选择只在本次运行内有效。选中一行时对应窗口会在屏幕上高亮。

**为什么按应用而不是按类名**：本机实测 6 个不同的 exe 共用 `Chrome_WidgetWin_1` 这一个类名
（chrome、Typora、Claude、ChatGPT、Feishu、墨鱼阅读）。按类名排会一次干掉六个；
按 exe 路径排就能只排掉其中一个。标题也不行——它随页面变，而且经常塞满不可见字符。

被排除的应用一旦被置顶仍然会画边框，那是置顶生效的唯一提示。

## Settings, renaming and the context menu

Right-click a bookmark for **重命名** / **设置**. The tray menu groups everything into
**书签** and **窗口边框** submenus, each with its own settings window and its own
**启用** check mark — the two features are switched and configured separately, and both
switches are the same setting the dialog's checkbox writes, so they survive a restart.

The settings dialog exposes every configurable value — appearance, the bottom row, hover
preview, behaviour and performance — and changes **apply immediately** and are written to
`settings.conf`. There is a 恢复默认值 button, and out-of-range numbers are reported rather
than silently clamped.

The tray menu itself carries the program-wide switches, above 关于 and 退出:

```text
✓ 书签             >   顶层的对勾镜像各功能的启用状态，右击一次就看得到
✓ 窗口边框          >
  窗口置顶          >
──────────
  暂停所有             master switch; reads 启用所有 while paused
✓ 开机启动
  配置文件...          配置文件放在哪，见「配置文件位置」一节
  关于
──────────
  退出
```

顶层那三个对勾是**状态显示**，切换仍在各自子菜单的第一项（「启用书签」/「启用窗口边框」/
「启用窗口置顶」）。Win32 里带子菜单的项点击即展开子菜单，系统不给它命令 ID，因此没办法
兼作开关——这是菜单模型本身的限制，`MF_OWNERDRAW` 也绕不过去（它只让你自绘，不让你细分
点击区域）。

**暂停所有 / 启用所有** turns both features off, or both back on, without visiting either
submenu. The label names what the click will do, so there is no tick to interpret.

**开机启动** is off after a fresh install, and is *not* stored in `settings.conf`: Windows
lets the user turn a startup entry off from Task Manager and from 设置 - 应用 - 启动, so the
registry entry is the only record and the menu reads it live every time it opens.

Renaming overrides the window title on that one bookmark; clearing the field restores it.
Like per-window selection, a custom name lasts only for the current WindowMark run — see
the reasoning under "choose which apps/windows participate" below.

## New in rc2: choose which apps/windows participate

Right-click the tray icon and choose:

```text
选择需要书签的应用/窗口...
```

The native selection panel shows applications and their current top-level windows with checkboxes.

- **Application checkbox**: persistent. If an app is unchecked, WindowMark will not create bookmarks for that application on later launches either.
- **Window checkbox**: session-only. An unchecked window is removed both as a bookmark host and as a bookmark target for the rest of the current WindowMark run.
- A participating application still needs at least **two enabled windows** before bookmark overlays are created.

Per-window state is deliberately not persisted generically: an `HWND` on Windows (and similarly a generic window object on macOS) has no reliable cross-session identity. A future app-specific extension can provide stable workspace/project identities without corrupting Core with title-based guesses.

## Architecture

The project deliberately keeps platform APIs out of `core/`.

```text
WindowMark
|
+-- core/                         platform-neutral C++
|   +-- Coordinator              grouping + synchronization + selection policy
|   +-- LayoutEngine             left/right/top/bottom policy
|   +-- DrawerState              compact/hover expansion state machine
|   +-- Settings                 portable settings model
|   +-- AppSelectionModel        platform-neutral checkbox data
|   +-- IWindowBackend
|   +-- IOverlayBackend
|   +-- IPreviewBackend
|   +-- IBorderBackend           window outlines; independent of bookmarks
|
+-- platform/windows/
|   +-- WinWindowBackend         EnumWindows + SetWinEventHook
|   +-- WinOverlayBackend        Win32 + Direct2D/DirectWrite
|   +-- WinBorderBackend         one layered window per outline, shared bitmap
|   +-- WinPreviewBackend        DWM thumbnail preview
|   +-- WinControlWindow         tray icon and menu
|   +-- WinSelectionDialog       native app/window checkbox UI
|   +-- WinSettingsDialog        bookmark and border settings windows
|
+-- platform/macos/
|   +-- backend scaffold         Cocoa types stay here only
|
+-- tests/
    +-- core tests               no Win32 dependency
```

### macOS migration boundary

A future macOS backend should implement the same interfaces rather than changing Core. Expected platform mapping:

- window discovery/activation: macOS Accessibility + window APIs;
- overlay: `NSPanel` / `NSWindow`;
- preview: ScreenCaptureKit / CoreGraphics;
- native selection UI: AppKit frontend over the same `AppSelectionModel`;
- configuration, grouping, selection policy, drawer state, colors, and placement policy remain in Core.

No `HWND`, `RECT`, DWM, Direct2D, Cocoa, or CoreGraphics type is allowed in the platform-neutral public model.

## Safety design

WindowMark intentionally does **not** inject DLLs, modify Explorer, patch the taskbar, install a kernel driver, or install a Windows service. It runs as one ordinary user process.

Important safety properties:

- `SetWinEventHook` is out-of-context and skips the WindowMark process itself.
- Hook callbacks only coalesce/post lightweight events; they do not render previews or enumerate windows synchronously.
- location-change events are throttled (default 33 ms) so dragging a window does not cause an unbounded redraw storm.
- no polling loop repeatedly calls `EnumWindows`.
- overlay and border windows sit at their target's own depth, never at the top of the desktop. A border only becomes topmost when its target is, and drops back when the target does.
- host minimize/hide/close causes its overlay to hide or be destroyed.
- DWM preview count is globally limited to one.
- all WinEvent hooks, DWM thumbnails, HWNDs and COM graphics resources are released on normal shutdown; process-owned windows/resources also disappear if the process is force-terminated.
- **no global hotkey.** `RegisterHotKey` claims a combination process-wide for the session — whoever asks first wins and everyone else silently loses it. Both features are switched from the tray menu instead.

## 配置文件位置

默认位置是 `%LOCALAPPDATA%\WindowMark\settings.conf`。托盘菜单的「配置文件...」可以改，
三个选项对应三层查找顺序：

```text
1. exe 同目录的 settings.conf              存在即用（便携，跟着程序走）
2. HKCU\Software\WindowMark\ConfigPath     「自定义」写在这里
3. %LOCALAPPDATA%\WindowMark\settings.conf 默认
```

**便携排在自定义之前**，因为注册表跟着机器走，而 exe 旁边的 conf 跟着程序走——U 盘插到
别人电脑上，不该去读那台机器上指定的本地路径。这可能反直觉，所以对话框顶部始终显示
当前实际生效的是哪一份。

切换位置时现有配置会被搬过去。从便携切走时，exe 旁那份会改名为 `settings.conf.disabled`
而不是删掉，否则下次启动第 1 层又会把它抢回去。

### 如果 C 盘装了还原工具

**只把配置文件挪个位置解决不了这个问题。** WindowMark 默认装在
`C:\Users\<用户>\AppData\Local\Programs\WindowMark\`，跟配置一样在 C 盘，会被一起还原。
注册表里的自定义路径同理——`HKCU` 也在 C 盘。

唯一可靠的做法是**把整个 WindowMark 目录拷到非 C 盘**（D 盘或 U 盘），从那里运行，
配置放在 exe 同目录。三点提醒：

- 是「搬走」不是「多放一份」。C 盘那份如果还在跑，第二个实例会因单例互斥量冲突直接退出。
- 便携模式下开机自启动写的是当时的绝对路径，**U 盘盘符一变就失效**。
- `border.excluded_apps` 和 `selection.disabled_apps` 记的是**被排除的那些应用**的 exe
  绝对路径（Chrome、墨鱼阅读等）。换一台机器路径不同，这两项会**静默失效**——不报错，
  只是匹配不上。这是按 exe 路径标识应用的固有代价，也正是它能把墨鱼阅读和 Chrome 分开
  的原因：两者类名同为 `Chrome_WidgetWin_1`。

配置文件里没有任何指向 WindowMark 自身的路径，所以没有可以改成相对路径的东西。

## Settings

On first Windows run WindowMark creates:

```text
%LOCALAPPDATA%\WindowMark\settings.conf
```

Defaults:

```ini
drawer.enabled=true
placement=bottom
drawer.collapsed_extent=30
drawer.expanded_extent=180
drawer.thickness=34
drawer.gap=6
drawer.corner_radius=10
drawer.animation_ms=90
drawer.short_name_chars=4
drawer.top_offset=72
drawer.attach_overlap=6
drawer.active_window_only=true
drawer.active_extra_extent=10
drawer.transparency=0

drawer.bottom_collapsed_extent=44
drawer.bottom_expanded_extent=120
drawer.bottom_collapsed_thickness=0
drawer.bottom_active_thickness=23

border.enabled=false
# 不画边框的应用，按可执行文件路径。用「边框设置 -> 排除应用」勾选。
# 和 selection.disabled_apps 分开：不想要边框和不想要书签是两回事。
border.excluded_apps=
border.width=4
border.offset=-1
border.corners=auto
border.corner_radius=8
border.active_color=#6274E7
border.inactive_color=#7080AA

preview.enabled=true
preview.delay_ms=450
preview.width=480
preview.height=300
preview.corner_radius=12

pin.enabled=true
pin.color=accent
pin.width=10
pin.show_in_system_menu=true
# 空 = 不占用任何快捷键。见「窗口置顶」一节。
pin.hotkey=

performance.geometry_throttle_ms=33

# Extra window classes to ignore completely - no bookmark, no border. Adds to the
# built-in list. Run WindowMarkInspect.exe to find a class name.
#
# 注意：类名往往不足以区分。Chrome、Claude、ChatGPT 和它们的悬停浮窗都是
# Chrome_WidgetWin_1，按类名排除会把正常窗口一起干掉。浮窗是靠
# WS_EX_NOACTIVATE / WS_EX_TRANSPARENT 排除的，不在这个列表里。
tracking.exclude_classes=

# 自绘阴影内缩，格式 类名:左,上,右,下，多个用 | 分隔。
# GTK 这类应用把投影画在自己的窗口矩形里，那圈边距透明且没有任何 Windows 接口能报出来，
# 于是边框看起来离窗口很远。最大化时自动忽略。
# 双击 measure_shadow_inset.bat 可以量出该填什么。
tracking.shadow_insets=

# Filled automatically when apps are unchecked in the selection panel.
selection.disabled_apps=
```

`drawer.animation_ms=0` gives an immediate no-animation expansion. `preview.width` and `preview.height` control preview size. `drawer.collapsed_extent` is how far a compact tab sticks out; `drawer.short_name_chars` is how many characters that compact tab shows (no ellipsis — the full label appears on hover).

An existing `settings.conf` is never rewritten by an upgrade, so changed defaults only apply to new installs. Edit the file and restart WindowMark to pick them up, or run
`rebuild_and_install.bat -Fresh`, which deletes it.

### 这些数值是定过的，不要顺手改回去

Each of these was chosen deliberately after looking at the result on screen. Anything that
would change one of them needs to be raised first, not decided in passing.

| 配置项 | 值 | 为什么是这个值 |
|---|---|---|
| `drawer.bottom_active_thickness` | **23** | 激活标签的高度。此前写死等于 `drawer.thickness`（34），只能靠改 `thickness` 来调，会连非激活项一起缩掉——所以给了它独立设置 |
| `drawer.bottom_expanded_extent` | **120** | 悬停展开后的标签宽度 |
| `drawer.bottom_collapsed_extent` | **44** | 平时的标签宽度 |
| `drawer.short_name_chars` | **4** | 折叠标签显示几个字 |
| `border.width` + `border.offset` | **4 / -1** | `Reach = 4 + (-1) = 3`：窗口外 3px，再压住窗口自身边缘 1px。`offset=0` 会让 Windows 自己那条 1px 边框露出来变成灰缝（实测 `#4F5255`/`#646765`） |
| `placement` | **bottom** | `auto` 会随窗口移动在左右之间跳，找不着 |
| `drawer.active_window_only` | **true** | 只有前台窗口显示书签条 |
| `pin.width` | **10** | 置顶高亮的线宽。6 看着和普通边框没区别；PowerToys 用 15，偏重了 |
| `pin.color` | **accent** | 跟随系统强调色，置顶窗口看起来像属于这个桌面 |
| `pin.hotkey` | **空** | 全局快捷键先到先得，不主动从别的程序手里抢 |

Same rule for the settings dialog's layout numbers in
`src/platform/windows/WinSettingsDialog.cpp` — the label/hint column widths were measured
against the longest string each one has to hold, and shrinking one clips text rather than
wrapping it.

## Build on Windows

Requirements:

- Windows 10/11
- CMake
- Visual Studio with the Desktop C++ workload

Double-click, or from PowerShell / CMD:

| 双击这个 | 做什么 |
|---|---|
| `build.bat` | 只编译 |
| **`rebuild_and_install.bat`** | 编译 → 单元测试 → 卸载旧版 → 装新版并启动，最后打印生效的配置。改完直接看效果就用它 |
| `rebuild_and_install.bat -Fresh` | 同上，**并删除 `settings.conf`**。改的是代码里的默认值时必须用这个，否则旧配置会盖掉新默认值 |
| `rebuild_and_install.bat -NoBuild` | 跳过编译，只重装（约 5 秒）|
| **`check_border.bat`** | 边框诊断：倒数 5 秒让你切到目标窗口，然后报边框的位置、层级、四条边逐点取色，以及每个不对的点被哪个窗口盖着 |
| **`build\Release\WindowMarkInspect.exe`** | 看到不该有边框的东西时跑它。盯 20 秒，然后给屏幕上每个边框贴黄色数字牌并打印编号表，表里就有类名，还会告诉你填到哪。`--watch 40` 加长，`--now` 只看此刻 |

`.bat` 里全是 ASCII，不是疏忽：`cmd.exe` 按系统 ANSI 代码页读 `.bat`，写 UTF-8 中文会变乱码并打断命令解析。中文输出由对应的 `.ps1` 负责。

## Install / uninstall

The build produces four user-facing executables in `build\Release\`:

```text
WindowMark.exe             the app
WindowMarkSetup.exe        installer
WindowMarkUninstall.exe    uninstaller
WindowMarkInspect.exe      diagnostic: which window has an outline, and why
```

Double-click `WindowMarkSetup.exe`. It shows one dialog with the install location and an
**开机时自动启动 WindowMark** checkbox — unchecked on a fresh install, and pre-set to
whatever is already configured on an upgrade — then installs to
`%LOCALAPPDATA%\Programs\WindowMark`, creates a Start menu shortcut, registers an entry
under **设置 - 应用**, and starts the app. The same switch is available from the tray menu.

The startup switch reflects Windows' effective state, not merely the presence of a Run
value. If Task Manager or Settings disables WindowMark, the tray item becomes unchecked.
Enabling it writes a quoted command ending in `--autostart` and clears the corresponding
`StartupApproved` veto. After the next sign-in, `%LOCALAPPDATA%\WindowMark\startup.log`
records `attempt` followed by `running`, or the startup phase that failed.

Uninstall from **设置 - 应用**, from the Start menu, or by running `WindowMarkUninstall.exe`
in the install directory. Its dialog has a **同时删除我的设置和数据** checkbox; leaving it
unchecked keeps `%LOCALAPPDATA%\WindowMark`.

**A running WindowMark is never an error.** Both tools ask the live instance to close
through its own message loop, wait for it, and only then continue — installing over a
running copy just restarts it. Launching `WindowMark.exe` a second time is not an error
either: it hands off to the instance that is already running, which shows a tray balloon,
and exits quietly.

Both accept switches for scripted use:

```text
WindowMarkSetup.exe      /S  /StartWithWindows  /NoStartWithWindows
WindowMarkUninstall.exe  /S  /Purge
```

`/Purge` additionally removes `%LOCALAPPDATA%\WindowMark` and the reserved roaming data
directory. No shell extension, service, driver, Explorer patch, COM registration, or
system-wide registry state is ever installed — everything lives under the current user.

## Validation status

rc3 is the first revision **built and run on Windows**. See [VALIDATION.md](VALIDATION.md) for what was
exercised: MSVC Release/Debug builds, core and registry-integration tests in both
configurations, overlay rendering on a 125% display, startup-command simulation, and full
install/reinstall-over-running/uninstall cycles.

## 图标

`res/wmiicon.ico` — 托盘图标和两个设置窗口的标题栏图标都用它。换图标只需替换这个文件，
代码不用动：CMake 会把绝对路径填进生成的 `.rc`。

必须是**真正的多尺寸 .ico**，至少含 16/20/24/32/48/256，32bpp 带 alpha，背景透明。
16 那一档最关键——托盘和标题栏用的就是它，而且代码用 `LoadImage` 按小图标尺寸取，
不会拿 256 缩下来发虚。留白色底板的话，深色任务栏上会显示成一个白方块。

## Version policy

**v0.4.5** is the current release. It includes window bookmarks, per-window borders,
per-application border exclusions, window pinning, `WindowMarkInspect.exe`, reliable
start-with-Windows state handling with a login-attempt audit log, borders clamped to the
window's own monitor, and a configurable config-file location (portable or custom path).

The earlier public repository release is tag `2.0`, corresponding to application version
v0.2.0. All intermediate versions are retained in the changelog so the progression to
v0.4.5 remains auditable.

Fixes go to `v0.4.x`; larger new features go to the next minor line. See
[VALIDATION.md](VALIDATION.md) for what is verified and what still is not, and
[ROADMAP.md](ROADMAP.md) for what is deferred.
