#pragma once

#include "windowmark/core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace windowmark {

struct DrawerSettings {
    // Same switch the tray menu flips, and the same shape as BorderSettings::enabled, so
    // the two features are turned on and off the same way and both survive a restart.
    bool enabled{true};
    // Bottom by default rather than Auto: Auto picks a side from whichever has more room
    // outside the window, so the strip jumps between left and right as the window moves
    // and you have to hunt for it. A fixed edge is easier to find.
    Placement placement{Placement::Bottom};
    // Side placements (left/right): the extent is how far a tab reaches out
    // horizontally, the thickness is its height.
    int collapsedExtent{30};
    int expandedExtent{180};
    int thickness{34};

    // Row placements (bottom/top, used for maximized hosts) need their own numbers:
    // there the extent is a tab's width, so the side values do not transfer. A row tab
    // also sits half-height against the window edge and grows upward on hover, which is
    // what bottomCollapsedThickness controls (0 means half of thickness).
    int bottomCollapsedExtent{44};
    int bottomExpandedExtent{120};
    int bottomCollapsedThickness{0};
    // How tall the active row tab stands, and what a hovered one grows to. Its own
    // setting on purpose: it used to be hard-wired to `thickness`, so the only way to
    // change it was to change `thickness`, which also shrank the resting tabs and the
    // strip. 0 means fall back to `thickness`.
    int bottomActiveThickness{23};
    int gap{6};
    int cornerRadius{10};
    int animationMs{90};
    int shortNameChars{4};
    int topOffset{72};
    int attachOverlap{6};

    // Show bookmarks only on the foreground window. Overlays are owned popups of a
    // window in another process, and Windows does not keep cross-process owner/owned
    // z-order in sync, so background strips could float above unrelated foreground
    // windows. Restricting to the active host makes the owner always the foreground
    // window, which is the position the overlay should occupy anyway.
    bool activeWindowOnly{true};
    // Extra length for the bookmark pointing at the window you are looking at, so the
    // current one is identifiable at a glance in a row of same-sized tabs.
    int activeExtraExtent{10};
    // Percent. 0 means fully opaque, which is the default look; raising it lets the
    // window show through the tabs. Applies to every tab equally - the active one is
    // told apart by geometry, not by opacity.
    int transparency{0};
};

// Corner rounding, modelled on tacky-borders' border_radius:
//   Auto        - ask DWM what shape this particular window is
//   Square      - no rounding
//   Round       - the standard Windows 11 radius
//   RoundSmall  - the smaller radius Windows uses for compact windows
//   Custom      - use BorderSettings::cornerRadius verbatim
// Auto is the only one that varies per window; Windows 10 has no rounding and reports
// square regardless.
enum class BorderCorners {
    Auto,
    Square,
    Round,
    RoundSmall,
    Custom,
};

// Window borders are independent of bookmarks: they apply to every top-level window,
// including single-window apps that never get a bookmark strip.
struct BorderSettings {
    bool enabled{false};
    // 4 with an offset of -1, so the outline reaches 3px past the window and covers the
    // last pixel of it. At offset 0 the outline stops one pixel short and the 1px frame
    // Windows draws for itself shows through as a grey seam between the outline and the
    // window - measured #646765 on Explorer, #4F5255 on Chrome. Overlapping by one pixel
    // hides it, and the extra width buys legibility against busy backgrounds.
    int width{4};
    // Distance from the window's own edge. Negative shrinks the outline inwards (over the
    // window), positive pushes it outwards - same convention as tacky-borders.
    int offset{-1};
    BorderCorners corners{BorderCorners::Auto};
    // Only consulted when corners == Custom.
    int cornerRadius{8};
    // 0xAARRGGBB. Alpha lives in the colour itself, as in tacky-borders, so there is one
    // place to change rather than a colour plus a separate opacity knob.
    unsigned activeColor{0xFF6274E7};
    unsigned inactiveColor{0xFF7080AA};
};

struct PinSettings {
    // Independent of border.enabled on purpose. A pinned window always gets its
    // highlight, because the highlight *is* the feedback that the pin worked - tying
    // it to a feature that ships turned off would mean pressing the shortcut does
    // nothing visible.
    bool enabled{true};
    // kAccentColor means "whatever the system accent colour is right now". Chosen as the
    // default over a fixed colour so a pinned window looks like it belongs to the desktop
    // it is sitting on, and so it tracks the user's own theme - which is what PowerToys
    // does and what this was measured against.
    static constexpr unsigned kAccentColor = 0;
    unsigned color{kAccentColor};
    // Wide enough to read as a deliberate highlight rather than a slightly heavier border.
    // 6 was tried first and looked like the ordinary outline; PowerToys uses 15, which the
    // user found heavier than they wanted. 10 is the value they settled on.
    int width{10};
    bool showInSystemMenu{true};
    // Empty by default. RegisterHotKey claims a combination process-wide and the
    // loser fails silently, so this app does not take one unless asked.
    std::string hotkey;
};

struct PreviewSettings {
    bool enabled{true};
    int delayMs{450};
    int width{480};
    int height{300};
    int cornerRadius{12};
};

struct PerformanceSettings {
    int geometryThrottleMs{33};
};

// Application-level selection is persistent and platform-neutral. The keys are
// opaque identities supplied by IWindowBackend (on Windows this is the
// normalized executable path). Per-window enable/disable is intentionally kept
// as runtime state because a generic HWND/NSWindow has no stable cross-session
// identity without an app-specific plugin.
struct SelectionSettings {
    std::vector<std::string> disabledAppKeys;
};

// Window classes the platform layer must never report at all - not as a bookmark, not as
// a border. The backend has a built-in list for the shell UI it already knows about, and
// this adds to it.
//
// User-editable on purpose: the built-in list was measured on one Windows build with one
// set of IMEs, and neither travels. A different Windows version renames its shell classes
// and a different IME brings its own candidate window, so on someone else's machine the
// built-in list will miss things. Finding the class name is what mark_borders.bat is for;
// adding it here is what stops needing a rebuild to act on the answer.
struct TrackingSettings {
    std::vector<std::string> excludeClasses;
    // "类名:左,上,右,下" - how far inside its own window rect an application paints its
    // visible edge. Needed for client-side-decorated toolkits: GTK draws its drop shadow
    // *inside* the window rect, and nothing in Win32 or DWM reports where the opaque part
    // starts - GetWindowRect, DWMWA_EXTENDED_FRAME_BOUNDS and GetClientRect all return the
    // same rect, hit-testing covers the shadow too, and PrintWindow's alpha is unusable on
    // most windows. So the number has to be supplied rather than discovered.
    // Run WindowMarkInspect.exe to measure one.
    std::vector<std::string> shadowInsets;
};

struct Settings {
    DrawerSettings drawer;
    BorderSettings border;
    PinSettings pin;
    PreviewSettings preview;
    PerformanceSettings performance;
    SelectionSettings selection;
    TrackingSettings tracking;

    static Settings LoadOrCreate(const std::filesystem::path& filePath);
    static bool Save(const std::filesystem::path& filePath, const Settings& settings);
};

[[nodiscard]] std::string ToString(Placement placement);
[[nodiscard]] Placement PlacementFromString(const std::string& value);
[[nodiscard]] std::string ToString(BorderCorners corners);
[[nodiscard]] BorderCorners BorderCornersFromString(const std::string& value);

} // namespace windowmark
