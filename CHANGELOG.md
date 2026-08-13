# Changelog

## v0.2.0

### Bookmarks always show text

`drawer.short_name_chars` was treated as a literal glyph count and the font size was
fixed at 13px, but a tab is only as tall as the drawer settings make it. Measured with
DirectWrite, 13px needs **17.3px of line height** — more than the 16px a default collapsed
row tab provides. Text that overflows its layout rect is not trimmed by
`D2D1_DRAW_TEXT_OPTIONS_CLIP`, it disappears completely, so a bookmark would render as a
blank coloured block while its neighbours read fine.

- The font size is now derived from each tab's own height, and tabs differ (the active
  one is at full thickness, the rest rest at part of it), so this is decided per tab.
- Labels are still trimmed to the measured width, with `short_name_chars` as an upper
  bound rather than a target.
- Verified exhaustively against real DirectWrite metrics: **550 combinations** of 11 tab
  heights (10–80px, covering the smallest a `thickness` of 20 can produce), 5 collapsed
  widths (24–100px) and 10 labels spanning Latin, CJK, Japanese, Cyrillic, emoji and
  single characters — 0 cases where text would be empty, too wide or too tall.

### Defaults and settings UI

- **`placement` now defaults to `bottom`.** `auto` chooses a side from whichever has more
  free space, so the strip jumps between left and right as a window is dragged and you
  have to hunt for it. A fixed edge stays put.
- **Fixed the settings window opening off-screen.** It was centred on `owner`, which is
  the hidden tray control window — a 0x0 window parked at (0,0) — so the maths produced
  negative coordinates. It now centres on the work area of the monitor under the pointer,
  clamped so the title bar always stays reachable, and sizes itself with that monitor's
  DPI rather than the tray window's.
- **Fixed controls overlapping the buttons.** The right column ran to ~464px while the
  button row sat at 428px. Groups are rebalanced across the columns and the window is
  taller; a guard now reports the overflow instead of silently drawing on top.
- The settings window is brought to the foreground on open — its owner is hidden, so
  nothing did that for it and the dialog could appear behind another window.

### About

- Tray menu gains **关于 WindowMark...**, showing version, executable and config paths,
  and what the app deliberately does not touch. The settings window footer shows the
  version too.

## v0.1.0-rc4

Adds the settings UI, bookmark renaming and configurable transparency, and fixes two
installer defects found while verifying them.

### Settings

- **Settings dialog**, reachable from the tray menu and from a bookmark's right-click menu. It exposes every configurable value — appearance, bottom row, hover preview, behaviour, performance — grouped in two columns, with a 恢复默认值 button. Out-of-range numbers are reported rather than silently clamped, so a typo cannot quietly become a different setting.
- Changes **apply immediately and are written to `settings.conf`** — no restart. `IOverlayBackend`/`IPreviewBackend` gained `UpdateSettings`, and `Coordinator::UpdateSettings` pushes the edit through and re-applies the models.
- New `drawer.transparency` (percent, 0 = fully opaque, the default). It applies to every tab equally; the active one is still told apart by geometry, not opacity.

### Bookmarks

- **Fixed collapsed tabs rendering blank for some labels.** `drawer.short_name_chars` was applied as a literal character count, but three CJK glyphs are roughly twice as wide as three Latin ones: at the default 44px bottom tab only ~35px is usable, and a three-CJK label measures 39px. Overflowing text combined with `D2D1_DRAW_TEXT_OPTIONS_CLIP` disappeared entirely rather than degrading, so one tab looked empty while its neighbours read fine. The label is now measured with DirectWrite and trimmed to what actually fits; `short_name_chars` is an upper bound rather than a target, and labels that already fit are untouched.
- **Right-click a bookmark** for 重命名 / 设置.
- **Renaming** overrides the window title on that bookmark. It is deliberately session-only, for the same reason per-window selection is: a generic OS window has no reliable cross-session identity, so a persisted name would eventually attach itself to the wrong window. Clearing the field restores the title.
- Bottom/top rows now anchor **inside** the host's own edge whether or not it is maximized. Previously a restored window's row hung below the window while a maximized one sat inside it, which flipped the root edge and drew those tabs upside down.

### Installer fixes

- **Silent install could hang forever.** `ShellExecuteExW` was called without `SEE_MASK_FLAG_NO_UI`, so when a launch failed it popped its own modal error box and waited for a click that a `/S` install will never get. All three call sites now suppress that UI and report failures through return values instead.
- **Install-over-running intermittently left nothing running.** Waiting on the old process handle was not enough — if `OpenProcess` failed the wait was skipped entirely, and a predecessor still mid-exit kept the single-instance mutex. The new instance then saw that mutex, concluded it was a second launch, and exited silently, which looked exactly like the installer not starting the app. The installer now waits for the mutex to actually be released, retries the launch, and verifies the process is still alive afterwards; `/S` returns a non-zero code if it is not.
- Fixed a dangling pointer in the uninstall dialog: `pszMainInstruction` pointed at a temporary `std::wstring`'s `c_str()`, so the title rendered as garbage.

## v0.1.0-rc3

First revision actually built and run on Windows. Fixes what that run exposed.

### Build

- Fixed MSVC `std::max` ambiguity in `WinSelectionDialog.cpp`: `RECT` members are `LONG`, so `std::max(0, ...)` could not deduce a single `_Ty`. Same class of bug rc2 fixed in `WinPreviewBackend.cpp`.

### Layering and legibility

- **Bookmarks now appear only on the foreground window** (`drawer.active_window_only`, on by default). Overlays are owned popups of windows belonging to *other* processes, and Windows does not keep cross-process owner/owned z-order in sync, so strips belonging to background windows could float above whatever was actually in front. Restricting to the active host makes the owner always the foreground window, which is the position the overlay should occupy anyway. Set the key to `false` for the old every-window behaviour.
- Every bookmark is squared off at its **root edge** — the edge it grows out of — so it meets the window seamlessly there, like a bookmark slipped between pages. Which edge that is depends on placement: side tabs reach outward from the window, so the root is the edge nearest it; row tabs grow inward from the window's own boundary, so the root is that outer edge. This holds for the active tab too: it cannot overhang its root, and rounding it there made it look like it was floating off the edge.
- The active bookmark is instead set apart by **geometry alone** — it is longer (`drawer.active_extra_extent`) and it sits over the window edge where the others stop at it, so it reads as lying *on top of* the window while the rest read as slotted in *behind* it. No outline, no shadow, no opacity difference: every tab is filled identically, which keeps the strip quiet and the draw path to a single rounded-rect fill per tab.
- Bottom/top rows size independently from the sides (`drawer.bottom_collapsed_extent`, `drawer.bottom_expanded_extent`): there a tab's extent is its width, so the side numbers never transferred. Row tabs also rest at half thickness against the window edge and grow toward the host on hover (`drawer.bottom_collapsed_thickness`, 0 = half of `drawer.thickness`).

### Rendering

- Fixed bookmark tabs rendering as black blocks offset from their own window. The Direct2D render target inherited the system DPI (120 on a 125% display) while every rectangle from `LayoutEngine` is in physical pixels, so all drawing was scaled 1.25x and pushed off the right edge; the exposed area showed the cleared background, which an `ALPHA_MODE_IGNORE` HWND render target renders as opaque black. The render target is now pinned to 96 DPI.
- Overlays are now layered windows presented with `UpdateLayeredWindow` from a premultiplied-alpha DIB. Tabs have real per-pixel transparency and antialiased corners; the `SetWindowRgn` approximation is gone.
- Compact tabs center their label and drop the ellipsis, which at 30px was consuming a glyph that the tab needed.
- The tray selection panel now gets Common Controls v6 visuals.

### Performance

Idle cost is 0.02% CPU. The work that mattered was window dragging, where a geometry
event fires roughly 30 times a second after throttling and each one previously rebuilt
everything. Measured on a 7-window group with synthetic drag plus cursor traffic, that
path went from ~20% of one core to ~15%.

- `WinEventProc` now rejects on `objectId`/`childId` before anything else. `EVENT_OBJECT_LOCATIONCHANGE` fires for every mouse move (`OBJID_CURSOR`) and every child object on the desktop, and each of those rejects was paying for a `GetWindowThreadProcessId` cross-process call first. The call was also redundant — the hooks use `WINEVENT_SKIPOWNPROCESS`.
- Executable paths are cached per `HWND` (invalidated when the pid behind it changes, pruned on full enumeration). Dragging no longer performs an `OpenProcess`/`QueryFullProcessImageName` round trip, a UTF-8 conversion and a lowercase pass ~30 times a second per window.
- `QueryProcessPath` uses an inline `MAX_PATH` buffer and only falls back to a heap buffer on `ERROR_INSUFFICIENT_BUFFER`, instead of allocating 64 KB per call.
- One shared `ID2D1DCRenderTarget` and text format for all overlays, replacing one HWND render target plus one text format per bookmarked window.
- Item rectangles are computed into a reused buffer, and labels are converted to UTF-16 once per model change instead of once per frame. Neither the animation tick nor mouse tracking allocates any more.
- A geometry event that only moves a host window skips re-rendering and re-uploading the layered bitmap, and overlays whose position and visibility are already correct skip `SetWindowPos`/`ShowWindow` entirely. Dragging one window no longer disturbs its siblings.

### Install / uninstall

- Replaced `scripts\install.ps1` and `scripts\uninstall.ps1` with `WindowMarkSetup.exe` and `WindowMarkUninstall.exe`, both native GUI executables that also accept `/S` for scripted use.
- **A running WindowMark is no longer an error anywhere.** Installer and uninstaller post a registered quit message straight to the tray control window, wait for the process to exit, and only force-terminate as a fallback. Installing over a running copy restarts it.
- Launching `WindowMark.exe` a second time no longer shows an error dialog; it notifies the live instance, which shows a tray balloon, and exits 0. `--purge` on a running instance now asks it to close instead of refusing.
- The installer registers a **设置 - 应用** entry, a Start menu shortcut, and an optional startup entry; the uninstaller removes all of them and can also purge user data. The uninstaller runs itself from `%TEMP%` so it can delete its own install directory, and cleans that copy up afterwards.

### Defaults

- `drawer.collapsed_extent` 52 -> 30, `drawer.short_name_chars` 4 -> 3.
- New keys: `drawer.active_window_only`, `drawer.active_extra_extent`, `drawer.bottom_collapsed_extent`, `drawer.bottom_expanded_extent`, `drawer.bottom_collapsed_thickness`. An existing `settings.conf` is never rewritten on upgrade, so add them by hand or delete the file to regenerate it.

### Known gaps, deferred to the next revision

- No settings UI yet: everything is edited in `settings.conf` and picked up on restart.
- No right-click menu on a bookmark.
- Tab opacity is fixed; the inactive hold-back is not configurable.
- Hover preview exists (`preview.enabled`) but has not been verified on Windows, and its
  window uses the same cross-process owner arrangement that the overlays just moved away
  from, so it may sit behind the host window.

### Tests

- `tests/core_tests.cpp` used `assert()` for calls with side effects. Under `NDEBUG` those calls vanished, so the Release test binary skipped `Coordinator::Start()`, `Settings::Save()` and `DrawerState::Tick()` entirely and then crashed on an empty `std::function`. Replaced with a `CHECK` macro that always evaluates and always verifies, in every configuration.

## v0.1.0-rc2

Bug-fix and selection-control revision based on the first Windows build feedback.

- Fixed MSVC `std::max` ambiguity in `WinPreviewBackend.cpp` caused by mixing Win32 `LONG` values with `int` literals.
- Added a native tray entry: **选择需要书签的应用/窗口...**.
- Added a platform-neutral selection model in Core; the Windows UI is only a frontend for that model, so macOS can provide its own UI later.
- Application-level enable/disable is persisted in `settings.conf` using the backend-provided application identity.
- Individual-window enable/disable is supported for the current process session; it is intentionally not persisted because generic OS windows do not have a reliable cross-session identity.
- Disabled windows are removed both as bookmark hosts and bookmark targets; a group must still have at least two enabled windows before any overlay is created.
- Added selection filtering and settings round-trip tests.
- Kept the same `v0.1.0` target version; this is `rc2`, not a feature-version bump.

## v0.1.0-rc1

First runnable architecture revision.

- Platform-neutral Core with `IWindowBackend`, `IOverlayBackend`, and `IPreviewBackend`.
- Windows event-driven window discovery/tracking with `SetWinEventHook`.
- Same-app grouping by executable path.
- One bookmark overlay per independent window; every overlay contains the same bookmark set for that app group.
- Rounded pastel-rainbow bookmark tabs.
- Platform-neutral DrawerState: compact by default, hovered item expands independently, click switches immediately.
- `Self` and `Active` visual states are separate.
- Auto layout: normal windows prefer left-side bookmarks; maximized windows use bottom bookmarks.
- Hover preview via DWM thumbnail, one preview at a time; width/height configurable.
- Geometry-event throttling/coalescing to reduce drag/resize overhead.
- Tray control and `Ctrl+Alt+W` quick hide/show.
- Portable-style install/uninstall scripts with optional full purge.
- macOS backend boundary/scaffold included from the first revision.
