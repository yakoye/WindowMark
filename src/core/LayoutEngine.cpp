#include "windowmark/core/LayoutEngine.h"

#include <algorithm>
#include <cmath>

namespace windowmark {
namespace {

int ClampOrigin(int desired, int extent, int minValue, int maxValue) {
    if (extent >= (maxValue - minValue)) {
        return minValue;
    }
    return std::clamp(desired, minValue, maxValue - extent);
}

// 书签栏的几何只分两种：主轴是 X（Top / Bottom）或 Y（Left / Right）。Auto 在这里不会出现
// ——ResolvePlacement 总会给出具体方向——万一出现按 Bottom 处理，和 PreviewStack 的约定一致。
[[nodiscard]] bool IsSide(Placement placement) {
    return placement == Placement::Left || placement == Placement::Right;
}

} // namespace

bool LayoutEngine::IsRowPlacement(Placement placement) noexcept {
    return placement == Placement::Top || placement == Placement::Bottom;
}

DrawerMetrics LayoutEngine::MetricsFor(Placement placement, const DrawerSettings& settings) {
    DrawerMetrics metrics;
    metrics.fullThickness = std::max(1, settings.thickness);

    if (!IsRowPlacement(placement)) {
        metrics.collapsedExtent = std::max(1, settings.collapsedExtent);
        metrics.restThickness = metrics.fullThickness;
        // Side tabs are all the same height; the active one is told apart by reaching
        // further in, so shrinking it here would only make it look broken.
        metrics.activeThickness = metrics.fullThickness;
        return metrics;
    }

    metrics.collapsedExtent = std::max(1, settings.bottomCollapsedExtent);
    metrics.restThickness = settings.bottomCollapsedThickness > 0
        ? std::min(settings.bottomCollapsedThickness, metrics.fullThickness)
        : std::max(1, metrics.fullThickness / 2);
    // Never below the resting height (the active tab would sink under its neighbours)
    // and never above the full thickness.
    metrics.activeThickness = settings.bottomActiveThickness > 0
        ? std::clamp(settings.bottomActiveThickness, metrics.restThickness, metrics.fullThickness)
        : metrics.fullThickness;
    return metrics;
}

Placement LayoutEngine::ResolvePlacement(const WindowInfo& host, const DrawerSettings& settings) {
    if (settings.placement != Placement::Auto) {
        return settings.placement;
    }
    // 书签条以前挂在窗口外面，Auto 要看左右哪边窗口外还有地方，窗口一挪就在左右之间跳。现在
    // 四个方向都贴在窗口内侧，不再需要窗口外的空间：最大化用底部横排，其余一律左侧。
    return host.maximized ? Placement::Bottom : Placement::Left;
}

DockSpec LayoutEngine::DockSpecFor(
    Placement placement,
    std::size_t count,
    int activeIndex,
    const DrawerSettings& settings) {

    const bool side = IsSide(placement);
    const DrawerMetrics metrics = MetricsFor(side ? placement : Placement::Bottom, settings);
    const float extra = static_cast<float>(std::max(0, settings.activeExtraExtent));

    DockSpec spec;
    spec.params.gap = static_cast<float>(std::max(0, settings.gap));
    spec.params.radius = static_cast<float>(std::max(1, settings.magnetRadius));
    spec.params.peakMain = static_cast<float>(side ? settings.magnetMaxThickness
                                                   : settings.bottomMagnetMaxExtent);
    spec.params.peakCross = static_cast<float>(side ? settings.magnetMaxExtent
                                                    : settings.bottomMagnetMaxThickness);

    spec.items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool active = static_cast<int>(i) == activeIndex;
        DockItemBase item;
        if (side) {
            item.main = static_cast<float>(metrics.fullThickness);
            item.cross = static_cast<float>(metrics.collapsedExtent) + (active ? extra : 0.0F);
        } else {
            item.main = static_cast<float>(metrics.collapsedExtent) + (active ? extra : 0.0F);
            item.cross = static_cast<float>(active ? metrics.activeThickness
                                                   : metrics.restThickness);
        }
        spec.items.push_back(item);
    }
    return spec;
}

DockBounds LayoutEngine::ComputeOverlayBounds(
    const WindowInfo& host,
    const DockSpec& spec,
    float maxGrowth,
    Placement placement,
    const DrawerSettings& settings) {

    const bool side = IsSide(placement);

    float baseTotal = 0.0F;
    float depth = 1.0F;
    for (const DockItemBase& item : spec.items) {
        baseTotal += item.main;
        depth = std::max({depth, item.cross, spec.params.peakCross});
    }
    if (spec.items.size() > 1) {
        baseTotal += spec.params.gap * static_cast<float>(spec.items.size() - 1);
    }

    // 余量取整：base 的尺寸和间距都是整数，起点也落在整数上，静止时标签边缘才是锐利的。
    const int margin = static_cast<int>(std::ceil(std::max(0.0F, maxGrowth)));
    const int base = static_cast<int>(std::ceil(baseTotal));
    const int cross = static_cast<int>(std::ceil(depth));

    const int workLo = side ? host.workArea.top : host.workArea.left;
    const int workHi = side ? host.workArea.bottom : host.workArea.right;
    const int wanted = base + 2 * margin;
    const int length = std::max(1, std::min(wanted, workHi - workLo));

    const int desiredBase = side ? host.frame.top + settings.topOffset
                                 : host.frame.left + (host.frame.width() - base) / 2;
    const int mainStart = ClampOrigin(desiredBase - margin, length, workLo, workHi);

    DockBounds out;
    // 工作区放不下整条（标签多到这种程度很少见）时窗口就是整个工作区，base 在里面居中。
    out.baseOrigin = wanted <= length ? static_cast<float>(margin)
                                      : static_cast<float>((length - base) / 2);

    int crossStart = 0;
    switch (placement) {
    case Placement::Top:
        crossStart = ClampOrigin(host.frame.top, cross, host.workArea.top, host.workArea.bottom);
        break;
    case Placement::Left:
        crossStart = ClampOrigin(host.frame.left, cross, host.workArea.left, host.workArea.right);
        break;
    case Placement::Right:
        crossStart = ClampOrigin(host.frame.right - cross, cross,
                                 host.workArea.left, host.workArea.right);
        break;
    default:
        crossStart = ClampOrigin(host.frame.bottom - cross, cross,
                                 host.workArea.top, host.workArea.bottom);
        break;
    }

    out.bounds = side ? Rect{crossStart, mainStart, crossStart + cross, mainStart + length}
                      : Rect{mainStart, crossStart, mainStart + length, crossStart + cross};
    return out;
}

} // namespace windowmark
