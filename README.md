# WindowMark v0.3.7

WindowMark is a lightweight **same-application multi-window bookmark layer**.

If three independent VS Code windows are open, all three windows receive the same three bookmarks. Clicking any bookmark immediately activates the corresponding VS Code window. Chrome, Explorer, SiYuan, terminals, and other ordinary top-level applications use the same mechanism without app-specific plugins.

## v0.3.7 at a glance

This Windows release combines two independent tools in one ordinary user process:

- **Window bookmarks** for identifying, previewing, renaming, and switching between
  multiple windows of the same application.
- **Window borders** for outlining every eligible top-level window with separate active
  and inactive colors. Borders are optional and disabled by default.

v0.3.7 reworks the tray menu — a **暂停所有 / 启用所有** master switch, **开机启动** (off
after a fresh install), shorter labels and a left-growing popup — and makes the settings
dialog's height derive from its field list instead of a hand-tuned constant.

v0.3.6 fixed the major drag-latency problem caused by assigning cross-process owners to
bookmark windows, strengthened bookmark/border z-order recovery, created bookmark windows
only when they are visible, and added build timestamps plus opt-in diagnostic counters.
See [CHANGELOG.md](CHANGELOG.md) for the measurements and full history.

## Quick start

1. Download `WindowMark-v0.3.7-Release.zip` from the
   [v0.3.7 release](https://github.com/yakoye/WindowMark/releases/tag/v0.3.7).
2. Extract it and run `WindowMarkSetup.exe`. To use it without installing, run
   `WindowMark.exe` directly from the extracted directory.
3. Open at least two normal windows from the same application to see bookmarks.
4. Use the tray menu to enable/configure **书签** and the optional **窗口边框** separately.

The functional release targets Windows 10/11. The macOS directory remains an architecture
scaffold and does not provide a working macOS application in v0.3.7.

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
WindowMark          greyed header, so 关于/退出 need not repeat the name
书签              >
窗口边框           >
──────────
暂停所有            master switch; reads 启用所有 while paused
开机启动
关于
──────────
退出
```

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

performance.geometry_throttle_ms=33

# Extra window classes to ignore completely - no bookmark, no border. Adds to the
# built-in list. Run WindowMarkInspect.exe to find a class name.
tracking.exclude_classes=

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
exercised: MSVC Release/Debug builds, core tests in both configurations, overlay rendering
on a 125% display, and full install/reinstall-over-running/uninstall cycles.

## 图标

`res/wmiicon.ico` — 托盘图标和两个设置窗口的标题栏图标都用它。换图标只需替换这个文件，
代码不用动：CMake 会把绝对路径填进生成的 `.rc`。

必须是**真正的多尺寸 .ico**，至少含 16/20/24/32/48/256，32bpp 带 alpha，背景透明。
16 那一档最关键——托盘和标题栏用的就是它，而且代码用 `LoadImage` 按小图标尺寸取，
不会拿 256 缩下来发虚。留白色底板的话，深色任务栏上会显示成一个白方块。

## Version policy

**v0.3.7** is the current release. It includes the window-border feature developed in the
v0.3.0 line, configurable exclusions and `WindowMarkInspect.exe` from v0.3.2, the movement
path improvements from v0.3.5, the cross-process-owner and z-order fixes from v0.3.6, and
the start-with-Windows switch from v0.3.7.

The earlier public repository release is tag `2.0`, corresponding to application version
v0.2.0. Development revisions v0.3.0, v0.3.2, v0.3.5 and v0.3.6 are retained in the
changelog so the progression to v0.3.7 remains auditable.

Fixes go to `v0.3.x`; new features to `v0.4.0`. See [VALIDATION.md](VALIDATION.md) for what
is verified and what still is not, and [ROADMAP.md](ROADMAP.md) for what is deferred.
