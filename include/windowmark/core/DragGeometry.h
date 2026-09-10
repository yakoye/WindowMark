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

// 按下到松开几乎没动 = 单击，不是拖动。slop 是允许的抖动，单位像素，两个方向分别判断。
//
// 只看位移不看时间：按住修饰键瞄准、犹豫两秒再松手，仍然是一次单击。加时限只会让慢的人
// 得不到反应，而他们恰恰是最需要这个手势的人。
[[nodiscard]] bool IsTap(int dx, int dy, int slop);

// 把窗口摆到工作区正中，尺寸不变。
//
// 纵向夹到 work.top：窗口比工作区高时，居中会把标题栏顶出屏幕外，那正好毁掉这个手势要
// 解决的问题——够不着标题栏。横向不夹，比工作区宽的窗口左右均等溢出，两边都还够得着。
[[nodiscard]] Rect CenterInWorkArea(const Rect& frame, const Rect& work);

} // namespace windowmark
