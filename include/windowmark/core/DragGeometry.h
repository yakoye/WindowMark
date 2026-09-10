#pragma once

#include "windowmark/core/Types.h"

namespace windowmark {

// 拖动改变窗口矩形的哪几条边。四条全为真 = 移动（整体平移）。
struct DragEdges {
    bool left{};
    bool top{};
    bool right{};
    bool bottom{};

    [[nodiscard]] bool IsMove() const { return left && top && right && bottom; }
    [[nodiscard]] bool None() const { return !left && !top && !right && !bottom; }
};

// 鼠标按下的位置落在窗口的哪一格。九宫格 1:1:1 等分，正中间返回「移动」——否则在窗口
// 中间按下右键将无事可做。
//
// 格线归属：左闭右开。窗口宽 300 时，x=100 属于中间列而不是左列。这个约定必须稳定，
// off-by-one 会让某一列的行为随窗口尺寸漂移，而那种 bug 靠肉眼几乎发现不了。
[[nodiscard]] DragEdges HitZone(const Rect& frame, int x, int y);

// 起始矩形 + 光标位移 -> 新矩形。
//
// 缩放受 minWidth / minHeight 限制：到达下限后继续拖不再改变尺寸，被拖的那条边停住，
// 对面那条边始终不动。没有这个夹取，窗口会被拖成一条线甚至左右反转。
// 移动不受限制——把窗口拖出屏幕是用户的自由，Windows 自己也允许。
[[nodiscard]] Rect ApplyDrag(const Rect& start, const DragEdges& edges, int dx, int dy,
                             int minWidth, int minHeight);

} // namespace windowmark
