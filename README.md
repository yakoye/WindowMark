# WindowMark v0.2.0

WindowMark is a lightweight **same-application multi-window bookmark layer**.

It solves a simple but common problem: when one application has many independent top-level
windows, titles and taskbar previews are often too slow to scan. WindowMark gives every
participating window a compact, colored bookmark strip so you can identify and activate a
sibling window directly.

If three independent VS Code windows are open, all three windows receive the same three bookmarks. Clicking any bookmark immediately activates the corresponding VS Code window. Chrome, Explorer, SiYuan, terminals, and other ordinary top-level applications use the same mechanism without app-specific plugins.

## Version scope: v0.2.0

> **Window borders are not part of v0.2.0.** This release does not strengthen window
> borders and does not add border glow or active-window border highlighting. It does not
> include functionality equivalent to
> [tacky-borders](https://github.com/lukeyou05/tacky-borders). Border enhancement is
> planned for **v0.3.0**.

The colored elements in this release are bookmark tabs attached to participating windows;
they are not a replacement window border.

## Main features

- Groups ordinary top-level windows by application and creates bookmarks when a group has
  at least two enabled windows.
- Activates a sibling window with one click and distinguishes the current and active window.
- Supports bottom, top, left, right, and automatic placement, with compact-to-expanded hover
  animation.
- Shows a delayed DWM thumbnail preview for a hovered non-current window.
- Provides per-session bookmark renaming and app/window participation controls.
- Applies appearance, preview, behavior, and performance settings immediately.
- Includes a tray menu, `Ctrl+Alt+W` emergency hide/show, installer, and uninstaller.

## Quick start

The functional v0.2.0 release targets **Windows 10/11**. The macOS directory is currently an
architecture scaffold, not a working macOS release.

1. Download `WindowMark-v0.2.0-Release.zip` from the
   [2.0 release](https://github.com/yakoye/WindowMark/releases/tag/2.0).
2. Extract the archive and run `WindowMarkSetup.exe` (recommended), or run
   `WindowMark.exe` directly for a portable session.
3. Open two or more normal windows of the same application. Bookmarks appear on the active
   participating window; click one to switch to that window.
4. Right-click a bookmark to rename it or open settings. Use the tray menu to configure
   participating applications/windows or to exit.

## Interaction

The default placement is a compact row at the **bottom edge**. If placement is changed to
`auto`, a normal window uses the side with more free space and a maximized window uses the
bottom edge. Each bookmark has its own stable pastel/rainbow color during the process
lifetime. A left-edge layout looks like this:

```text
        compact                         hover B

     [ A… ]                          [ A… ]
     [ B… ]     VS Code              [         PCIe Tool         ]  VS Code
     [ C… ]                          [ C… ]
```

Only the hovered bookmark expands. The others remain compact. `Self` (the window that owns this bookmark strip) and `Active` (the current foreground window) are separate states.

Hovering a non-self bookmark waits for the configured preview delay and then shows one DWM
thumbnail preview; only one preview exists at a time.

## Settings, renaming and the context menu

Right-click a bookmark for **重命名** / **设置**; the tray menu has **设置...** too.

The settings dialog exposes every configurable value — appearance, the bottom row, hover
preview, behaviour and performance — and changes **apply immediately** and are written to
`settings.conf`. There is a 恢复默认值 button, and out-of-range numbers are reported rather
than silently clamped.

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
|
+-- platform/windows/
|   +-- WinWindowBackend         EnumWindows + SetWinEventHook
|   +-- WinOverlayBackend        Win32 + Direct2D/DirectWrite
|   +-- WinPreviewBackend        DWM thumbnail preview
|   +-- WinControlWindow         tray + emergency hide/show
|   +-- WinSelectionDialog       native app/window checkbox UI
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
- overlay windows are ordinary non-topmost owned popup windows; no `HWND_TOPMOST` is used.
- host minimize/hide/close causes its overlay to hide or be destroyed.
- DWM preview count is globally limited to one.
- all WinEvent hooks, DWM thumbnails, HWNDs and COM graphics resources are released on normal shutdown; process-owned windows/resources also disappear if the process is force-terminated.
- `Ctrl+Alt+W` immediately hides/shows all bookmark overlays without stopping window tracking.

## Settings

On first Windows run WindowMark creates:

```text
%LOCALAPPDATA%\WindowMark\settings.conf
```

Defaults:

```ini
placement=bottom
drawer.collapsed_extent=30
drawer.expanded_extent=180
drawer.thickness=34
drawer.gap=6
drawer.corner_radius=10
drawer.animation_ms=90
drawer.short_name_chars=3
drawer.top_offset=72
drawer.attach_overlap=6
drawer.active_window_only=true
drawer.active_extra_extent=10
drawer.transparency=0

drawer.bottom_collapsed_extent=44
drawer.bottom_expanded_extent=150
drawer.bottom_collapsed_thickness=0

preview.enabled=true
preview.delay_ms=450
preview.width=480
preview.height=300
preview.corner_radius=12

performance.geometry_throttle_ms=33

# Filled automatically when apps are unchecked in the selection panel.
selection.disabled_apps=
```

`drawer.animation_ms=0` gives an immediate no-animation expansion. `preview.width` and `preview.height` control preview size. `drawer.collapsed_extent` is how far a compact tab sticks out; `drawer.short_name_chars` is how many characters that compact tab shows (no ellipsis — the full label appears on hover).

An existing `settings.conf` is never rewritten by an upgrade, so changed defaults only apply to new installs. Edit the file and restart WindowMark to pick them up.

## Build on Windows

Requirements:

- Windows 10/11
- CMake
- Visual Studio with the Desktop C++ workload

From PowerShell or CMD:

```powershell
.\build.bat
```

or:

```powershell
.\build.ps1
```

The scripts configure a CMake build tree, compile the Release configuration, and run the
platform-neutral core tests. The equivalent commands are:

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### Development layout

```text
include/windowmark/core/  Platform-neutral public interfaces and models
src/core/                 Grouping, layout, drawer state, and settings logic
src/platform/windows/     Win32 discovery, overlays, previews, tray, and dialogs
src/platform/macos/       macOS backend scaffold
src/platform/stub/        Unsupported-platform entry point
src/installer/            Per-user Windows installer and uninstaller
src/shared/               Product identity shared by Windows executables
tests/                    Platform-neutral core tests
build/                    Generated build tree (ignored by Git)
```

## Install / uninstall

Both are ordinary executables. Build produces three files in `build\Release\`:

```text
WindowMark.exe             the app
WindowMarkSetup.exe        installer
WindowMarkUninstall.exe    uninstaller
```

Double-click `WindowMarkSetup.exe`. It shows one dialog with the install location and an
**开机时自动启动 WindowMark** checkbox, then installs to `%LOCALAPPDATA%\Programs\WindowMark`,
creates a Start menu shortcut, registers an entry under **设置 - 应用**, and starts the app.

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

## Version policy

**v0.2.0** is the first released version. It supersedes the `v0.1.0-rc1`..`rc5` series, which
never shipped: rc1/rc2 were only ever built on Linux, and rc3 onward were the Windows
bring-up. Beyond what v0.1.0 aimed at, this release adds the settings window, bookmark
renaming, the bookmark context menu and the about dialog.

Fixes go to `v0.2.x`; new features to `v0.3.0`. See [VALIDATION.md](VALIDATION.md) for what
is verified and what still is not.

## Roadmap

- **v0.2.x:** stability, compatibility, and bug fixes without expanding the feature scope.
- **v0.3.0:** add optional window-border enhancement, glow, and active-window highlighting
  in the spirit of [tacky-borders](https://github.com/lukeyou05/tacky-borders), while
  keeping it separate from the existing bookmark behavior and preserving the current
  non-injection safety model.
- **Later:** complete a native macOS backend behind the existing platform-neutral core
  interfaces.
