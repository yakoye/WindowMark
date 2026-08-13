#include "windowmark/core/LayoutEngine.h"

#include <algorithm>

namespace windowmark {
namespace {

int ClampOrigin(int desired, int extent, int minValue, int maxValue) {
    if (extent >= (maxValue - minValue)) {
        return minValue;
    }
    return std::clamp(desired, minValue, maxValue - extent);
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
        metrics.expandedExtent = std::max(metrics.collapsedExtent, settings.expandedExtent);
        metrics.restThickness = metrics.fullThickness;
        return metrics;
    }

    metrics.collapsedExtent = std::max(1, settings.bottomCollapsedExtent);
    metrics.expandedExtent = std::max(metrics.collapsedExtent, settings.bottomExpandedExtent);
    metrics.restThickness = settings.bottomCollapsedThickness > 0
        ? std::min(settings.bottomCollapsedThickness, metrics.fullThickness)
        : std::max(1, metrics.fullThickness / 2);
    return metrics;
}

Placement LayoutEngine::ResolvePlacement(const WindowInfo& host, const DrawerSettings& settings) {
    if (settings.placement != Placement::Auto) {
        return settings.placement;
    }

    if (host.maximized) {
        return Placement::Bottom;
    }

    const int leftSpace = host.frame.left - host.workArea.left;
    const int rightSpace = host.workArea.right - host.frame.right;
    const int minimumOutside = std::max(28, settings.expandedExtent - settings.attachOverlap);

    if (leftSpace >= minimumOutside) {
        return Placement::Left;
    }
    if (rightSpace >= minimumOutside) {
        return Placement::Right;
    }
    return Placement::Bottom;
}

Rect LayoutEngine::ComputeOverlayBounds(
    const WindowInfo& host,
    std::size_t itemCount,
    Placement placement,
    const DrawerSettings& settings) {

    const int count = static_cast<int>(std::max<std::size_t>(1, itemCount));
    const DrawerMetrics metrics = MetricsFor(placement, settings);
    const int stackExtent = count * settings.thickness + (count - 1) * settings.gap;

    if (placement == Placement::Left || placement == Placement::Right) {
        // Room for the active tab's extra reach, so it is not clipped by its own window.
        const int width = metrics.expandedExtent + settings.activeExtraExtent;
        const int height = std::min(stackExtent, std::max(settings.thickness, host.workArea.height()));
        const int desiredY = host.frame.top + settings.topOffset;
        const int y = ClampOrigin(desiredY, height, host.workArea.top, host.workArea.bottom);
        const int x = placement == Placement::Left
            ? host.frame.left - width + settings.attachOverlap
            : host.frame.right - settings.attachOverlap;
        return Rect{x, y, x + width, y + height};
    }

    const int maxRowWidth = metrics.expandedExtent + settings.activeExtraExtent
        + (count - 1) * metrics.collapsedExtent
        + (count - 1) * settings.gap;
    const int width = std::min(maxRowWidth, std::max(metrics.collapsedExtent, host.workArea.width()));
    // Full thickness even though row tabs rest at part of it: the spare space above is
    // what they expand into on hover.
    const int height = metrics.fullThickness;
    const int centeredX = host.frame.left + (host.frame.width() - width) / 2;
    const int x = ClampOrigin(centeredX, width, host.workArea.left, host.workArea.right);

    // A row strip sits just inside the host's own edge and its tabs grow inward from
    // there. Anchoring it inside regardless of whether the host is maximized keeps the
    // root edge - the one drawn square - on the same side in both cases; hanging it
    // outside for restored windows would flip the tabs upside down.
    const int y = placement == Placement::Top
        ? ClampOrigin(host.frame.top, height, host.workArea.top, host.workArea.bottom)
        : ClampOrigin(host.frame.bottom - height, height, host.workArea.top, host.workArea.bottom);
    return Rect{x, y, x + width, y + height};
}

} // namespace windowmark
