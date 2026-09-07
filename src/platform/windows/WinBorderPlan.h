#pragma once

#include "WinDesktopSnapshot.h"

#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <vector>

namespace windowmark::win {

// 一段要画的线：一个矩形加一个 0xAARRGGBB 颜色。
struct BorderStroke {
    Rect rect;
    unsigned color{};
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
