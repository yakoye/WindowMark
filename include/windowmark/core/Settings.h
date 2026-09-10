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
    // Applications that never get an outline, keyed the same way the bookmark selection
    // panel keys its own list: the normalized executable path. That key is what separates
    // 墨鱼阅读 from Chrome even though both put up Chrome_WidgetWin_1 windows - measured,
    // six different executables share that one class on this machine.
    //
    // Deliberately not the class name and deliberately not the title. The class is too
    // coarse for exactly that reason, and a title is whatever the app feels like: the
    // Chrome tab measured here carried fifty invisible characters before its first real
    // glyph, and it changes every time the user switches page.
    //
    // Separate from selection.disabledAppKeys on purpose: that one is the bookmark list,
    // and wanting no outline around an app is not the same as wanting no bookmark for it.
    std::vector<std::string> excludedAppKeys;
    bool enabled{true};
    // 3 with an offset of -1, so the outline reaches 2px past the window and covers the
    // last pixel of it. At offset 0 the outline stops one pixel short and the 1px frame
    // Windows draws for itself shows through as a grey seam between the outline and the
    // window - measured #646765 on Explorer, #4F5255 on Chrome. Overlapping by one pixel
    // hides it.
    int width{3};
    // Distance from the window's own edge. Negative shrinks the outline inwards (over the
    // window), positive pushes it outwards - same convention as tacky-borders.
    int offset{-1};
    // Custom rather than Auto by default. Auto asks DWM what shape the window is and
    // gets 8 DIP back, and 8 DIP is the radius of the window's own corner - but the
    // outline sits outside that corner, on a wider arc, so following it exactly leaves
    // the outline visibly squarer than the window it wraps.
    BorderCorners corners{BorderCorners::Custom};
    // Only consulted when corners == Custom. Physical pixels, not DIP: this is the knob
    // for "make it look right on my screen", and 12 is what that turned out to be at
    // 125%. On a different scale factor it wants adjusting - that is the price of a knob
    // that means exactly what it says.
    int cornerRadius{12};
    // 画圆角时在 width 上多加这么宽，多出来的部分全长在窗口内侧，外沿不动。
    //
    // 圆角模式下整圈——四条边加四个角——都是同一条 D2D 弧矩形，所以这个增量作用于
    // 整圈。要它的理由是抗锯齿：弧在两侧各留约 1px 渐变，画出来的实心部分比名义线宽
    // 窄（实测 width=4 时名义 4 只剩 2px 实心，名义 5 剩 4px，名义 7 剩 7px）。
    //
    // 默认 3 是在屏幕上逐档试出来的，不是算出来的：抗锯齿吃掉多少取决于弧的曲率和
    // 它落在像素格的哪个位置，没有一个能一次算准的公式。设 0 就是不补，负数更细。
    int cornerWidthExtra{3};
    // 整圈弧往窗口中心挪多少。正=向内，负=向外。默认 0：外沿和直角模式齐平。
    int cornerInset{0};
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

    // 按窗口类名强制画边框，跳过所有「这算不算一个窗口」的资格判据。
    //
    // 那套判据是为了挡掉工具窗口、悬浮小卡片、对话框这些不该有边框的东西，可总会有
    // 应用长得不像话——判据一多，误伤就一定存在，而误伤了没有别的办法绕过。这个名单
    // 就是那个办法：写进来的类名，只要它还是个可见的顶级窗口，就给它画。
    //
    // 只放宽资格，不放宽别的：排除名单仍然优先，被排除的不会因为写在这里就冒出来。
    std::vector<std::string> forceIncludeClasses;

    // 按窗口类名「视为置顶」：别人的边框一律给它让路。
    //
    // 前台窗口的边框只被两类东西裁——真正的 topmost 窗口，和它自己的 owned 对话框。
    // 可有些窗口既不是 topmost 也不属于前台，却实实在在浮在上面（各家自绘的浮动面板、
    // 输入法候选框、悬浮工具条），边框就会从它们身上穿过去。写进这个名单，它们就享受
    // 和 topmost 一样的待遇。
    std::vector<std::string> treatAsTopmostClasses;
};

// 按住修饰键拖动窗口。默认关闭：它接管全局鼠标事件，不该在用户没要求时就生效。
struct DragSettings {
    // 不参与拖动的应用，键与 border.excludedAppKeys 同样是规范化的可执行文件路径。
    // 独立成一项而不是共用边框那份：有些程序自己就用 Alt+拖动（Photoshop 等），
    // 不想要拖动和不想要边框是两件事。
    std::vector<std::string> excludedAppKeys;

    // 触发键，"RAlt" / "RAlt|LWin"。任一按下即触发，不是组合键。
    //
    // 存字符串而不是解析后的键码：配置文件是手写的，原样保留用户写的东西——包括六个
    // 预设之外的键名——比存一份解析结果更不容易丢信息。
    std::string modifiers{"RAlt"};

    bool enabled{false};
};

struct Settings {
    DrawerSettings drawer;
    BorderSettings border;
    DragSettings drag;
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
