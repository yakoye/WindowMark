#include "windowmark/core/BorderOcclusion.h"

#include <algorithm>

namespace windowmark {
namespace {

[[nodiscard]] bool IsEmpty(const Rect& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

[[nodiscard]] bool Intersects(const Rect& a, const Rect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

void Push(std::vector<Rect>& out, const Rect& r) {
    if (!IsEmpty(r)) out.push_back(r);
}

} // namespace

std::vector<Rect> SubtractRect(const Rect& from, const Rect& hole) {
    std::vector<Rect> out;
    if (IsEmpty(from)) return out;
    if (!Intersects(from, hole)) {
        out.push_back(from);
        return out;
    }

    // 四刀切法：先横着切掉洞上方和下方的整条，再在洞所在的那段高度里切掉左右两块。
    // 这样切出来的块天然互不重叠，不需要再去重。
    Push(out, Rect{from.left, from.top, from.right, std::min(from.bottom, hole.top)});
    Push(out, Rect{from.left, std::max(from.top, hole.bottom), from.right, from.bottom});

    const int midTop = std::max(from.top, hole.top);
    const int midBottom = std::min(from.bottom, hole.bottom);
    if (midTop < midBottom) {
        Push(out, Rect{from.left, midTop, std::min(from.right, hole.left), midBottom});
        Push(out, Rect{std::max(from.left, hole.right), midTop, from.right, midBottom});
    }
    return out;
}

std::vector<Rect> BorderRingSegments(const Rect& outer, const Rect& inner) {
    std::vector<Rect> ring;
    Push(ring, Rect{outer.left, outer.top, outer.right, inner.top});        // 上：整宽
    Push(ring, Rect{outer.left, inner.bottom, outer.right, outer.bottom});  // 下：整宽
    Push(ring, Rect{outer.left, inner.top, inner.left, inner.bottom});      // 左：中段高
    Push(ring, Rect{inner.right, inner.top, outer.right, inner.bottom});    // 右：中段高
    return ring;
}

std::vector<Rect> VisibleBorderSegments(const Rect& outer, const Rect& inner,
                                        const std::vector<Rect>& occluders) {
    std::vector<Rect> segments = BorderRingSegments(outer, inner);
    for (const Rect& occluder : occluders) {
        if (segments.empty()) break;   // 全被盖住了，后面的遮挡物不用再算
        std::vector<Rect> next;
        next.reserve(segments.size());
        for (const Rect& seg : segments) {
            for (const Rect& part : SubtractRect(seg, occluder)) next.push_back(part);
        }
        segments.swap(next);
    }
    return segments;
}

} // namespace windowmark
