#pragma once

#include "windowmark/core/MagneticDock.h"
#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <cstddef>
#include <vector>

namespace windowmark {

// Side placements measure a tab's reach horizontally, row placements measure it
// vertically, so the two cannot share one pair of numbers.
struct DrawerMetrics {
    int collapsedExtent{};
    // Thickness at rest. Row tabs sit at part of their full thickness; side tabs always
    // use the full thickness.
    int restThickness{};
    // What the active tab stands at. Separate from fullThickness so a row's active tab
    // can be sized without dragging the resting height and the side layout along with it.
    int activeThickness{};
    int fullThickness{};
};

// 磁性书签栏的 base 尺寸和磁场参数。只由方向、设置、标签个数和哪个是激活标签决定，和鼠标
// 无关——这就是 base：只用来算距离，任何动画都不改它。
struct DockSpec {
    std::vector<DockItemBase> items;
    DockParams params;
};

// 书签条窗口放在屏幕哪里，以及 base 排布在窗口里从哪儿开始。
struct DockBounds {
    Rect bounds;
    // 第一个标签的 base 区间在窗口内主轴上的起点。窗口比 base 长：两侧各留出书签被挤开时
    // 最多能长出去的量，所以 base 不从 0 开始。
    float baseOrigin{};
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

    // activeIndex 是激活标签的下标，没有激活标签时为 -1。
    //
    // 主轴 / 交叉轴的分工：
    //   横排  main = 标签宽度，cross = 标签伸进窗口的高度
    //   侧边  main = 标签高度，cross = 标签伸进窗口的深度
    // 激活标签的 base 比别人大（横排更宽更高，侧边伸得更深），峰值却和别人一样：峰值是绝对值。
    [[nodiscard]] static DockSpec DockSpecFor(
        Placement placement,
        std::size_t count,
        int activeIndex,
        const DrawerSettings& settings);

    // 四个方向都贴在宿主窗口**内侧**的那条边上，标签从边往窗口里长：
    //   交叉轴厚度 = 峰值深度（没有标签会长得比它更深）
    //   主轴长度   = base 总长 + 两侧各 maxGrowth
    // base 在主轴上横排居中于宿主、侧边从 drawer.top_offset 开始。整块窗口夹进工作区时 base
    // 跟着一起挪，所以鼠标下的点仍然是不动点，不需要事后再把 visual 平移回来。
    //
    // maxGrowth 就是 DockMaxGrowth(spec.items, spec.params)，由调用方算好传进来：它要把鼠标
    // 位置扫一遍，而宿主每挪一下都会走到这里，调用方按「方向 × 个数 × 激活下标」缓存它。
    [[nodiscard]] static DockBounds ComputeOverlayBounds(
        const WindowInfo& host,
        const DockSpec& spec,
        float maxGrowth,
        Placement placement,
        const DrawerSettings& settings);
};

} // namespace windowmark
