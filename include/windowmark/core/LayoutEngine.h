#pragma once

#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <cstddef>

namespace windowmark {

// Side placements measure a tab's reach horizontally, row placements measure it
// vertically, so the two cannot share one pair of numbers.
struct DrawerMetrics {
    int collapsedExtent{};
    int expandedExtent{};
    // Thickness at rest. Row tabs sit at part of their full thickness and grow toward
    // the screen interior on hover; side tabs always use the full thickness.
    int restThickness{};
    int fullThickness{};
};

class LayoutEngine {
public:
    [[nodiscard]] static bool IsRowPlacement(Placement placement) noexcept;

    [[nodiscard]] static DrawerMetrics MetricsFor(
        Placement placement,
        const DrawerSettings& settings);

    [[nodiscard]] static Placement ResolvePlacement(
        const WindowInfo& host,
        const DrawerSettings& settings);

    [[nodiscard]] static Rect ComputeOverlayBounds(
        const WindowInfo& host,
        std::size_t itemCount,
        Placement placement,
        const DrawerSettings& settings);
};

} // namespace windowmark
