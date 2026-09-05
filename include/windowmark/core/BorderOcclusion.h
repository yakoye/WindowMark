#pragma once

#include "windowmark/core/Types.h"

#include <vector>

namespace windowmark {

// 从 from 里挖掉 hole，返回剩下的部分（最多四块，互不重叠）。
//
// 用矩形分解而不是 GDI 的 Region：边框是四条细带，减法产生的碎片很少，而 Region
// 每次都要跨进 GDI 分配内核对象。这里全是纯算术，可以脱离 Windows 测试。
[[nodiscard]] std::vector<Rect> SubtractRect(const Rect& from, const Rect& hole);

// 把边框环拆成四条互不重叠的边：上、下、左、右。
//
// outer 是边框的外矩形，inner 是它围住的那个洞（也就是窗口自己的边界）。左右两条边
// 只占中间那段高度，不和上下两条在四个角上重叠——重叠会让同一块像素被画两次，半透明
// 色下四个角会明显更深。
[[nodiscard]] std::vector<Rect> BorderRingSegments(const Rect& outer, const Rect& inner);

// 边框环减去所有遮挡物，返回还看得见的段。
//
// 这是整个 Overlay 方案的核心：非激活窗口的边框不再靠 z 序去「排到正确的位置」，而是
// 直接把被上方窗口盖住的那几段**不画**。于是「边框排第几」这个要和整个系统抢一条全局
// 链表的时序问题，变成了「哪几段该画」的纯几何问题——后者不会失败，也没有竞争。
//
// 激活窗口不走这个函数：它的边框完整画、最后画。
[[nodiscard]] std::vector<Rect> VisibleBorderSegments(const Rect& outer, const Rect& inner,
                                                      const std::vector<Rect>& occluders);

} // namespace windowmark
