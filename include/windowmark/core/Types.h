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

// 动画中的几何是连续变化的，取整只在交给窗口系统的最后一步做。
struct RectF {
    float left{};
    float top{};
    float right{};
    float bottom{};

    [[nodiscard]] float width() const noexcept { return right - left; }
    [[nodiscard]] float height() const noexcept { return bottom - top; }
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
    // 第一个标签的 base 区间在书签条窗口里、沿书签栏方向的起点。窗口两侧留着标签被磁性
    // 挤开时的余量，所以它不是 0。见 LayoutEngine::ComputeOverlayBounds。
    float dockOrigin{};
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
    // Pinned takes priority over active when the outline picks its colour and width:
    // "this window is stuck in front of everything" is the more surprising state and
    // the one the user needs to be able to spot.
    bool pinned{false};
    bool visible{true};
};

// One pinned window. `wasTopmostBefore` is what makes unpinning safe: a window may
// already have been always-on-top before we touched it - Task Manager has its own
// option, so do many media players - and clearing the style unconditionally would
// turn off something the user set themselves.
struct PinRecord {
    WindowId windowId{};
    bool wasTopmostBefore{false};
};

// 预览栈里的一层：一个书签的标题和缩略图。主标签换人时新旧两层同时存在，一个淡出一个淡入。
struct PreviewLayer {
    WindowId sourceWindowId{};
    std::string title;
    // false：书签指向宿主窗口自己，只有标题，没有缩略图
    bool thumbnail{true};
    float opacity{};   // 0..1
};

// 书签条每一帧交给预览端的东西：书签 → 浮动标题 → 缩略图三段怎么排，所需的全部输入。
// 三段的具体位置由预览端用 LayoutPreviewStack 算——它量得出标题文字有多长。
struct PreviewRequest {
    WindowId hostWindowId{};
    Placement placement{Placement::Left};
    Rect workArea;
    // 书签条贴着的那条窗口边，在交叉轴上的屏幕坐标
    float rootEdge{};
    // 这一帧所有书签画出来的矩形，屏幕坐标。标题和缩略图要躲开其中每一个
    std::vector<RectF> tabs;
    // 标题和缩略图在主轴上以它为中心：各层所属书签中心按不透明度加权，切换时连续滑过去
    float anchorMain{};
    // 整个预览栈的不透明度，跟着磁场强度进出
    float opacity{};
    // 缩略图第一次出现要等 preview.delay_ms；之前只有标题
    bool thumbnailArmed{false};
    // 最新的一层在最后
    std::vector<PreviewLayer> layers;
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
