#pragma once

#include "windowmark/core/Types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace windowmark {

// 点到圆角矩形轮廓的有符号距离：轮廓上为 0，里面为负，外面为正。
//
// px、py 是相对矩形中心的坐标。把点折进第一象限，减掉「直边的半长」，剩下的负数说明
// 还落在某条直边上，正数才走到角的那段弧上——两个方向都为正时才需要开方。
//
// 边框的每个像素都要过一遍这个函数（一个窗口三万像素），所以写在头文件里让它内联。
[[nodiscard]] inline float RoundedRectDistance(float px, float py, float halfWidth,
                                               float halfHeight, float radius) {
    const float r =
        std::max(0.0F, std::min(radius, std::min(halfWidth, halfHeight)));
    const float dx = std::fabs(px) - (halfWidth - r);
    const float dy = std::fabs(py) - (halfHeight - r);
    const float ax = std::max(dx, 0.0F);
    const float ay = std::max(dy, 0.0F);
    // 两个短路分支都必须把另一个方向的值带上。只写 ax == 0 就返回 0 会让整条上下边的
    // 距离算成 -radius，覆盖率为负，边框直接消失。
    const float outside = ax == 0.0F ? ay
                        : ay == 0.0F ? ax
                                     : std::sqrt(ax * ax + ay * ay);
    return outside + std::min(std::max(dx, dy), 0.0F) - r;
}

// 环里面那块「肯定没有笔迹」的矩形，从路径矩形的边界往里收多少。
//
// 逐像素算距离场之前先把它挖掉，循环就从整个窗口缩到环那一圈带子——一个 1223x724 的
// 窗口从 88 万像素降到 3 万。挖掉的这块必须整个落在环内侧。
//
// 按「半个线宽」往里收是不够的：那对直边成立，对角不成立。环的内沿在角上是一段半径
// 为 radius - halfStroke 的圆弧，而挖掉的是**直角**矩形，它的角会顶进环带里，把那一
// 段笔迹连着削掉——屏幕上就是圆角内侧缺一块、紧挨着的那一行相对凸出一个小直角。
//
// 直角的角要缩到内沿圆弧的 45 度点上才不会顶出去，那个点比弧的极点近 1 - cos45。
[[nodiscard]] inline float RingHoleInset(float radius, float halfStroke) {
    constexpr float kCornerPull = 0.29289322F;   // 1 - cos(45°)
    const float innerRadius = std::max(0.0F, radius - halfStroke);
    // 末尾那 1 像素是留给抗锯齿过渡带的余量。
    return halfStroke + innerRadius * kCornerPull + 1.0F;
}

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

// 一组矩形减去所有遮挡物，返回还看得见的部分。
//
// 传进来的是「边框可能出现在哪些矩形里」。直角边框传四条边（互不重叠，画的时候直接
// 填满）；圆角边框传整个外矩形一块——角上的弧跨越相邻两条边，而边的直角范围装不下
// 它向内弯的那一段，按四条边裁会把角削平。
[[nodiscard]] std::vector<Rect> ClipSegments(const std::vector<Rect>& segments,
                                             const std::vector<Rect>& occluders);

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
