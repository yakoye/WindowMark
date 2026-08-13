#pragma once

#include "windowmark/core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace windowmark {

struct DrawerSettings {
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
    int bottomExpandedExtent{150};
    int bottomCollapsedThickness{0};
    int gap{6};
    int cornerRadius{10};
    int animationMs{90};
    int shortNameChars{3};
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

struct Settings {
    DrawerSettings drawer;
    PreviewSettings preview;
    PerformanceSettings performance;
    SelectionSettings selection;

    static Settings LoadOrCreate(const std::filesystem::path& filePath);
    static bool Save(const std::filesystem::path& filePath, const Settings& settings);
};

[[nodiscard]] std::string ToString(Placement placement);
[[nodiscard]] Placement PlacementFromString(const std::string& value);

} // namespace windowmark
