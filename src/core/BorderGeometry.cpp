#include "windowmark/core/BorderGeometry.h"

#include <algorithm>

namespace windowmark {

Rect ClampBorderToMonitor(const Rect& frame, const Rect& outer, const Rect& monitor,
                          int reach) {
    Rect result = outer;
    if (reach <= 0) return result;

    // 窗口这条边到监视器对应边的间隙。贴合时为 0，窗口跑到监视器外面时为负。
    const auto touches = [reach](int gap) { return gap >= 0 && gap <= reach; };

    if (touches(frame.left - monitor.left)) {
        result.left = std::max(result.left, monitor.left);
    }
    if (touches(frame.top - monitor.top)) {
        result.top = std::max(result.top, monitor.top);
    }
    if (touches(monitor.right - frame.right)) {
        result.right = std::min(result.right, monitor.right);
    }
    if (touches(monitor.bottom - frame.bottom)) {
        result.bottom = std::min(result.bottom, monitor.bottom);
    }
    return result;
}

} // namespace windowmark
