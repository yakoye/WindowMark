#include "WinBorderPlan.h"

#include "WinUtil.h"

#include "windowmark/core/BorderOcclusion.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace windowmark::win {
namespace {

[[nodiscard]] Rect ToCore(const RECT& r) {
    return Rect{r.left, r.top, r.right, r.bottom};
}

[[nodiscard]] HWND HwndOf(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

// 线有多粗。置顶的更粗——「这个窗口被钉在最前面」值得一眼认出来。
[[nodiscard]] int StrokeOf(const BorderModel& model, const Settings& settings) {
    return std::max(1, model.pinned ? settings.pin.width : settings.border.width);
}

// 边框往窗口外伸多远。
[[nodiscard]] int ReachOf(const BorderModel& model, const Settings& settings) {
    return std::max(0, StrokeOf(model, settings) + settings.border.offset);
}

// 边框环：外边界在窗口外 reach，内边界往窗口里进 stroke-reach。
//
// 内侧那一两像素必须盖在窗口自己身上，不能只画在窗口外面。offset 默认 -1 就是为了
// 这个：Windows 会给窗口画一条 1px 的边（实测 Explorer #646765、Chrome #4F5255），
// 边框如果停在窗口外沿，那条灰边就从缝里透出来，看着像边框和窗口之间有 1~2px 的
// 空隙——Windows Terminal 上尤其明显，它那条边颜色更浅。
[[nodiscard]] Rect RingInner(const Rect& frame, int stroke, int reach) {
    const int inset = stroke - reach;   // 等于 -offset
    Rect inner{frame.left + inset, frame.top + inset, frame.right - inset,
               frame.bottom - inset};
    // 窗口小到内边界翻转时退回窗口边界，宁可少盖一像素也不要画出一个空环。
    if (inner.right <= inner.left || inner.bottom <= inner.top) return frame;
    return inner;
}

// Windows 11 给普通窗口用的圆角半径，以及紧凑窗口用的那档。
constexpr float kRoundRadius = 8.0F;
constexpr float kRoundSmallRadius = 4.0F;

// 这个窗口自己想要多圆。Windows 10 没有圆角，那边这个查询直接失败。
[[nodiscard]] float SystemCornerRadius(HWND hwnd) {
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif
    enum : int { kDefault = 0, kDoNotRound = 1, kRound = 2, kRoundSmall = 3 };

    int preference = kDefault;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                                     sizeof(preference)))) {
        return 0.0F;   // Windows 10，或者这个窗口整个不参与该 API
    }
    switch (preference) {
    case kDoNotRound: return 0.0F;
    case kRoundSmall: return kRoundSmallRadius;
    case kRound:
    case kDefault:
    default:          return kRoundRadius;
    }
}

// 边框该画多圆。Auto 跟着窗口自己走——Windows 11 的窗口本来就是圆的，边框跟着圆才
// 贴合；用户想要直角就显式选 Square。
[[nodiscard]] float CornerRadiusOf(HWND hwnd, const Settings& settings) {
    switch (settings.border.corners) {
    case BorderCorners::Square:     return 0.0F;
    case BorderCorners::Round:      return kRoundRadius;
    case BorderCorners::RoundSmall: return kRoundSmallRadius;
    case BorderCorners::Custom:     return static_cast<float>(
                                        std::max(0, settings.border.cornerRadius));
    case BorderCorners::Auto:
    default:                        return SystemCornerRadius(hwnd);
    }
}

// 置顶压过活动状态：「这个窗口被钉在最前面」是更少见、也更值得一眼认出来的状态。
[[nodiscard]] unsigned ColorOf(const BorderModel& model, const Settings& settings,
                               unsigned accent) {
    if (model.pinned) {
        return settings.pin.color == PinSettings::kAccentColor ? accent : settings.pin.color;
    }
    return model.active ? settings.border.activeColor : settings.border.inactiveColor;
}

} // namespace

std::vector<BorderStroke> PlanBorders(const DesktopSnapshot& snapshot,
                                      const std::vector<BorderModel>& models,
                                      const Settings& settings) {
    std::vector<BorderStroke> strokes;
    if (models.empty() || snapshot.windows.empty()) return strokes;

    // 系统主题色只在真有置顶边框时才需要，整帧读一次就够。
    unsigned accent = 0;
    bool accentNeeded = false;
    for (const auto& model : models) {
        if (model.pinned && settings.pin.color == PinSettings::kAccentColor) {
            accentNeeded = true;
            break;
        }
    }
    if (accentNeeded) accent = SystemAccentColor();

    std::unordered_map<HWND, const BorderModel*> wanted;
    wanted.reserve(models.size());
    for (const auto& model : models) wanted.emplace(HwndOf(model.windowId), &model);

    std::vector<Rect> occluders;
    occluders.reserve(snapshot.windows.size());

    // 从上往下扫：轮到某个窗口时，occluders 里正好是**排在它上面**的所有窗口。
    // 一趟就够，不需要两两比较。
    //
    // 激活窗口不再有「不裁剪」的特权。它本来就在最前，上面没有普通窗口，裁出来自然
    // 是完整的一圈；而当它自己弹出对话框时（Word 的查找替换、Notepad++ 的查找都是
    // 这种 owned window，z 序上位于主窗口之上），那段边框就该让开——开后门的结果是
    // 边框横穿对话框把它切成两半。
    for (const auto& entry : snapshot.windows) {
        const bool paintable = !entry.cloaked && !entry.minimized;

        if (paintable) {
            if (const auto it = wanted.find(entry.hwnd); it != wanted.end()) {
                const BorderModel& model = *it->second;
                // 最大化窗口不画边框，置顶的除外——置顶是显式操作，边框是它生效的
                // 唯一视觉反馈。
                if (!entry.maximized || model.pinned) {
                    const Rect frame = ToCore(entry.frame);
                    const int stroke = StrokeOf(model, settings);
                    const int reach = ReachOf(model, settings);
                    const Rect outer{frame.left - reach, frame.top - reach,
                                     frame.right + reach, frame.bottom + reach};
                    const Rect inner = RingInner(frame, stroke, reach);
                    const unsigned color = ColorOf(model, settings, accent);
                    // 曲线沿路径居中，而路径在环外沿往里 stroke/2 处；把半径加上这段
                    // 偏移，画出来的弧才和窗口自己的圆角同心，不会在角上被掐细。
                    float radius = CornerRadiusOf(entry.hwnd, settings);
                    if (radius > 0.0F) {
                        radius += static_cast<float>(reach) - static_cast<float>(stroke) * 0.5F;
                        radius = std::max(0.0F, radius);
                    }
                    // 裁剪单元：直角用四条边（互不重叠，填满即可）；圆角用整个
                    // 外矩形一块——角上的弧跨越相邻两条边，按四条边裁会把角削平，
                    // 内沿在角上会突然被切成直线。
                    const std::vector<Rect> units =
                        radius > 0.0F ? std::vector<Rect>{outer}
                                      : BorderRingSegments(outer, inner);
                    for (const Rect& seg : ClipSegments(units, occluders)) {
                        strokes.push_back(BorderStroke{seg, color, outer, stroke, radius});
                    }
                }
            }
        }

        // 不管这个窗口有没有边框，它都会挡住排在它下面的窗口。
        if (paintable) occluders.push_back(ToCore(entry.frame));
    }
    return strokes;
}

} // namespace windowmark::win
