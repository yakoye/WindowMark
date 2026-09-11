#include "windowmark/core/BorderGeometry.h"

#include <algorithm>
#include <cmath>

namespace windowmark {
namespace {

// 窗口这条边到某个边界的间隙。贴合时为 0，窗口跑到边界外面时为负。
// [0, reach] 之内才算「贴上去了」——下界不能省，见头文件。
[[nodiscard]] bool Touches(int gap, int reach) { return gap >= 0 && gap <= reach; }

} // namespace

Rect ClampBorderToScreen(const Rect& frame, const Rect& outer, const Rect& monitor,
                         const Rect& workArea, int reach) {
    Rect result = outer;
    if (reach <= 0) return result;

    // 每条边分别看：先问贴不贴工作区，再问贴不贴监视器。工作区优先，因为最大化和半屏
    // 吸附贴的都是它——底边尤其关键，工作区底比监视器底高出整条任务栏。
    if (Touches(frame.left - workArea.left, reach)) {
        result.left = std::max(result.left, workArea.left);
    } else if (Touches(frame.left - monitor.left, reach)) {
        result.left = std::max(result.left, monitor.left);
    }

    if (Touches(frame.top - workArea.top, reach)) {
        result.top = std::max(result.top, workArea.top);
    } else if (Touches(frame.top - monitor.top, reach)) {
        result.top = std::max(result.top, monitor.top);
    }

    if (Touches(workArea.right - frame.right, reach)) {
        result.right = std::min(result.right, workArea.right);
    } else if (Touches(monitor.right - frame.right, reach)) {
        result.right = std::min(result.right, monitor.right);
    }

    if (Touches(workArea.bottom - frame.bottom, reach)) {
        result.bottom = std::min(result.bottom, workArea.bottom);
    } else if (Touches(monitor.bottom - frame.bottom, reach)) {
        result.bottom = std::min(result.bottom, monitor.bottom);
    }

    return result;
}

RoundedRing RoundedRingOf(int stroke, int widthExtra, int cornerInset) {
    RoundedRing ring;
    ring.width = static_cast<float>(std::max(1, stroke + widthExtra));
    // 内沿钉在窗口内 stroke - reach（= -offset）像素处，加宽全长在外侧：
    // 中心线 = 内沿 - width/2，换算成「相对外矩形往里」就是下面这个。
    ring.inset = static_cast<float>(stroke) - ring.width * 0.5F +
                 static_cast<float>(cornerInset);
    // 环的外沿相对外矩形往外多远。cornerInset 为负时整圈还要再往外挪同样多。
    const float outward = ring.width - static_cast<float>(stroke) -
                          static_cast<float>(cornerInset);
    ring.grow = outward > 0.0F ? static_cast<int>(std::ceil(outward)) : 0;
    return ring;
}

float RingInnerEdge(const RoundedRing& ring, int reach) {
    return ring.inset + ring.width * 0.5F - static_cast<float>(reach);
}

float RingOuterEdge(const RoundedRing& ring, int reach) {
    return ring.inset - ring.width * 0.5F - static_cast<float>(reach);
}

} // namespace windowmark
