#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace windowmark {

using WindowId = std::uint64_t;

struct Rect {
    int left{};
    int top{};
    int right{};
    int bottom{};

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
};

struct Color {
    float r{};
    float g{};
    float b{};
    float a{1.0F};
};

enum class Placement {
    Auto,
    Left,
    Right,
    Top,
    Bottom,
};

enum class WindowEventKind {
    StructureChanged,
    GeometryChanged,
    ActiveChanged,
    TitleChanged,
    VisibilityChanged,
};

struct WindowEvent {
    WindowEventKind kind{WindowEventKind::StructureChanged};
    WindowId windowId{};
};

struct WindowInfo {
    WindowId id{};
    std::string groupKey;
    std::string appName;
    std::string title;
    Rect frame;
    Rect workArea;
    bool visible{true};
    bool minimized{false};
    bool maximized{false};
    bool active{false};
};

// Window titles are whatever the app feels like putting there, and some of it occupies no
// space at all: zero-width joiners, bidi controls, byte-order marks, the invisible maths
// operators. A Chrome page was measured carrying fifty of them before its first real
// glyph. A collapsed tab shows the first few *characters* of the title, so a title like
// that produces a tab that is correctly rendered and completely blank. Dropping the
// characters that draw nothing leaves every visible one untouched.
[[nodiscard]] std::string SanitizeTitle(std::string_view title);

struct BookmarkItemModel {
    WindowId targetWindowId{};
    std::string label;
    Color color;
    bool isSelf{false};
    bool isActive{false};
};

struct OverlayModel {
    WindowId hostWindowId{};
    Placement placement{Placement::Left};
    Rect screenBounds;
    Rect hostFrame;
    Rect workArea;
    bool visible{true};
    std::vector<BookmarkItemModel> items;
};

// One per tracked top-level window, independent of bookmark grouping.
struct BorderModel {
    WindowId windowId{};
    Rect frame;
    bool active{false};
    bool visible{true};
};

struct PreviewRequest {
    WindowId hostWindowId{};
    WindowId sourceWindowId{};
    Placement placement{Placement::Left};
    Rect anchorScreenRect;
    Rect hostFrame;
    Rect workArea;
};

struct OverlayCallbacks {
    std::function<void(WindowId)> onActivate;
    std::function<void(const PreviewRequest&)> onPreview;
    std::function<void()> onPreviewHide;
    // Raised from a bookmark's context menu. The platform layer owns the input and
    // settings UI; Core only receives the result through Coordinator.
    std::function<void(WindowId)> onRename;
    std::function<void()> onOpenSettings;
};

// Platform-neutral selection model used by settings UIs on Windows/macOS.
struct WindowSelectionModel {
    WindowId windowId{};
    std::string title;
    bool enabled{true};
};

struct AppSelectionModel {
    std::string groupKey;
    std::string appName;
    bool enabled{true};
    std::vector<WindowSelectionModel> windows;
};

} // namespace windowmark
