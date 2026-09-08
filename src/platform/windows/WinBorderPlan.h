#pragma once

#include "WinDesktopSnapshot.h"

#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <vector>

namespace windowmark::win {

// 一段要画的线。
//
// rect 是这一段的范围——遮挡裁剪的结果。直角边框到此为止：把 rect 填成 color 就完了。
//
// 圆角边框还需要后面几个字段：曲线是连续的，没法按段独立画，只能画整个环再裁到
// rect，所以每一段都带着它所属的那个环的信息。roundWidth == 0 就是直角，渲染器据此
// 走填像素的快路径——不能拿 radius 判，圆角内移得多时半径会算到 0，那仍然是圆角
// 模式下的一个直角矩形路径，和填像素完全是两回事。
struct BorderStroke {
    Rect rect;
    unsigned color{};

    Rect ringOuter;      // 环的外矩形（这一段所属的完整边框）
    int strokeWidth{};   // 线宽
    float radius{};      // 路径处的圆角半径
    float roundWidth{};  // 圆角线宽；0 = 直角，走像素填充
    float roundInset{};  // 圆角路径从 ringOuter 往里收多少

    // 用来判断「这一帧和上一帧画的是同一批线」。兜底轮询每 500ms 算一次，绝大多数
    // 时候结果一模一样，比出来相同就直接跳过渲染。
    [[nodiscard]] bool operator==(const BorderStroke& o) const {
        return rect.left == o.rect.left && rect.top == o.rect.top &&
               rect.right == o.rect.right && rect.bottom == o.rect.bottom &&
               color == o.color && ringOuter.left == o.ringOuter.left &&
               ringOuter.top == o.ringOuter.top &&
               ringOuter.right == o.ringOuter.right &&
               ringOuter.bottom == o.ringOuter.bottom &&
               strokeWidth == o.strokeWidth && radius == o.radius &&
               roundWidth == o.roundWidth && roundInset == o.roundInset;
    }
};

// 把「谁该有边框」和「桌面此刻长什么样」合成一串待画线段。
//
// 规则：
//   - 按 z 序从上往下扫，边扫边累积遮挡区域
//   - 非激活窗口：边框环减去**排在它上面**的所有窗口，剩下的才画
//   - 激活窗口：完整边框、不裁剪，放在返回列表的**末尾**（调用方按顺序画即可）
//   - 最大化窗口不画（置顶的除外）——它贴着工作区边缘，外面没有画边框的那几像素，
//     夹回屏幕内之后边框和窗口边界完全重合，一个像素都露不出来
//
// 这个函数不碰任何窗口的 z 序，只读快照。「边框排第几」这个要和整个系统抢一条全局
// 链表的时序问题，在这里变成了「哪几段该画」的纯几何问题。
[[nodiscard]] std::vector<BorderStroke> PlanBorders(const DesktopSnapshot& snapshot,
                                                    const std::vector<BorderModel>& models,
                                                    const Settings& settings);

} // namespace windowmark::win
