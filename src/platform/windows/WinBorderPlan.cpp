#include "WinBorderPlan.h"

#include "WinUtil.h"

#include "windowmark/core/BorderOcclusion.h"

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

// 边框往窗口外伸多远。置顶的线更粗，所以伸得也更远。
[[nodiscard]] int ReachOf(const BorderModel& model, const Settings& settings) {
    const int stroke =
        std::max(1, model.pinned ? settings.pin.width : settings.border.width);
    return std::max(0, stroke + settings.border.offset);
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
    const BorderModel* activeModel = nullptr;
    Rect activeFrame{};

    // 从上往下扫：轮到某个窗口时，occluders 里正好是**排在它上面**的所有窗口。
    // 一趟就够，不需要两两比较。
    for (const auto& entry : snapshot.windows) {
        const bool paintable = !entry.cloaked && !entry.minimized;

        if (paintable) {
            if (const auto it = wanted.find(entry.hwnd); it != wanted.end()) {
                const BorderModel& model = *it->second;
                // 最大化窗口不画边框，置顶的除外——置顶是显式操作，边框是它生效的
                // 唯一视觉反馈。
                if (!entry.maximized || model.pinned) {
                    const Rect frame = ToCore(entry.frame);
                    const int reach = ReachOf(model, settings);
                    const Rect outer{frame.left - reach, frame.top - reach,
                                     frame.right + reach, frame.bottom + reach};
                    if (entry.hwnd == snapshot.foreground) {
                        // 激活窗口留到最后画，而且不裁剪。
                        activeModel = &model;
                        activeFrame = frame;
                    } else {
                        const unsigned color = ColorOf(model, settings, accent);
                        for (const Rect& seg :
                             VisibleBorderSegments(outer, frame, occluders)) {
                            strokes.push_back(BorderStroke{seg, color});
                        }
                    }
                }
            }
        }

        // 不管这个窗口有没有边框，它都会挡住排在它下面的窗口。
        if (paintable) occluders.push_back(ToCore(entry.frame));
    }

    if (activeModel != nullptr) {
        const int reach = ReachOf(*activeModel, settings);
        const Rect outer{activeFrame.left - reach, activeFrame.top - reach,
                         activeFrame.right + reach, activeFrame.bottom + reach};
        const unsigned color = ColorOf(*activeModel, settings, accent);
        for (const Rect& seg : BorderRingSegments(outer, activeFrame)) {
            strokes.push_back(BorderStroke{seg, color});
        }
    }
    return strokes;
}

} // namespace windowmark::win
