#include "windowmark/core/PreviewStack.h"

#include <algorithm>

namespace windowmark {
namespace {

[[nodiscard]] bool IsSide(Placement placement) {
    return placement == Placement::Left || placement == Placement::Right;
}

// 交叉轴往哪边长：+1 朝坐标增大的方向，-1 朝坐标减小的方向。
[[nodiscard]] float GrowthSign(Placement placement) {
    switch (placement) {
    case Placement::Top:
    case Placement::Left:
        return 1.0F;
    default:
        return -1.0F;   // Bottom、Right、Auto
    }
}

struct Interval {
    float start{};
    float end{};
};

[[nodiscard]] Interval MainOf(const RectF& r, Placement placement) {
    return IsSide(placement) ? Interval{r.top, r.bottom} : Interval{r.left, r.right};
}

// 书签从根边伸出去多深。
[[nodiscard]] float DepthOf(const RectF& r, Placement placement, float rootEdge) {
    switch (placement) {
    case Placement::Top: return r.bottom - rootEdge;
    case Placement::Left: return r.right - rootEdge;
    case Placement::Right: return rootEdge - r.left;
    default: return rootEdge - r.top;
    }
}

[[nodiscard]] float MaxDepthUnder(const PreviewStackInput& in, float mainStart, float mainEnd) {
    float deepest = 0.0F;
    for (const RectF& tab : in.tabs) {
        const Interval span = MainOf(tab, in.placement);
        if (span.end <= mainStart || span.start >= mainEnd) continue;
        deepest = std::max(deepest, DepthOf(tab, in.placement, in.rootEdge));
    }
    return deepest;
}

// 以 anchor 为中心放一段长 size 的主轴区间，夹进 [lo, hi]。比工作区还长就从 lo 开始。
[[nodiscard]] float CenteredStart(float anchor, float size, float lo, float hi) {
    const float desired = anchor - size * 0.5F;
    if (size >= hi - lo) return lo;
    return std::clamp(desired, lo, hi - size);
}

// 由主轴区间和「距根边的深度区间」拼出屏幕矩形。
[[nodiscard]] RectF Compose(const PreviewStackInput& in, float mainStart, float mainSize,
                            float depthStart, float depthSize) {
    const float sign = GrowthSign(in.placement);
    const float a = in.rootEdge + sign * depthStart;
    const float b = in.rootEdge + sign * (depthStart + depthSize);
    const float crossLo = std::min(a, b);
    const float crossHi = std::max(a, b);
    if (IsSide(in.placement)) return RectF{crossLo, mainStart, crossHi, mainStart + mainSize};
    return RectF{mainStart, crossLo, mainStart + mainSize, crossHi};
}

} // namespace

bool RectsOverlap(const RectF& a, const RectF& b) noexcept {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

PreviewStackLayout LayoutPreviewStack(const PreviewStackInput& in) {
    PreviewStackLayout out;
    const bool side = IsSide(in.placement);
    const float mainLo = side ? in.workArea.top : in.workArea.left;
    const float mainHi = side ? in.workArea.bottom : in.workArea.right;

    // --- 标题 ---
    const float titleMain = std::min(in.titleMain, mainHi - mainLo);
    const float titleStart = CenteredStart(in.anchorMain, titleMain, mainLo, mainHi);
    const float titleDepth = MaxDepthUnder(in, titleStart, titleStart + titleMain) + in.titleGap;
    out.title = Compose(in, titleStart, titleMain, titleDepth, in.titleCross);

    // --- 缩略图 ---
    if (in.thumbnailMain <= 0.0F || in.thumbnailCross <= 0.0F) return out;

    float thumbMain = std::min(in.thumbnailMain, mainHi - mainLo);
    float thumbCross = in.thumbnailCross * (thumbMain / in.thumbnailMain);
    float thumbStart = CenteredStart(in.anchorMain, thumbMain, mainLo, mainHi);
    const float thumbDepth = std::max(
        titleDepth + in.titleCross + in.thumbnailGap,
        MaxDepthUnder(in, thumbStart, thumbStart + thumbMain) + in.titleGap);

    // 交叉轴上从根边到工作区边界还剩多少
    float room = 0.0F;
    switch (in.placement) {
    case Placement::Top: room = in.workArea.bottom - in.rootEdge; break;
    case Placement::Left: room = in.workArea.right - in.rootEdge; break;
    case Placement::Right: room = in.rootEdge - in.workArea.left; break;
    default: room = in.rootEdge - in.workArea.top; break;
    }
    const float available = room - thumbDepth;
    if (thumbCross > available) {
        // 放不下就按比例缩小。缩小只会让主轴跨度变窄，跨度内的最大深度只可能变小，所以上面算好的
        // 深度仍然足够，不会压到书签。
        const float scale = available > 0.0F ? available / thumbCross : 0.0F;
        thumbMain *= scale;
        thumbCross *= scale;
        thumbStart = CenteredStart(in.anchorMain, thumbMain, mainLo, mainHi);
    }
    if (thumbMain < in.minThumbnailMain || thumbCross < in.minThumbnailCross ||
        thumbMain <= 0.0F || thumbCross <= 0.0F) {
        return out;
    }
    out.thumbnail = Compose(in, thumbStart, thumbMain, thumbDepth, thumbCross);
    out.thumbnailVisible = true;
    return out;
}

} // namespace windowmark
