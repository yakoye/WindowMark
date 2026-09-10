#include "windowmark/core/DragGeometry.h"

#include <algorithm>

namespace windowmark {
namespace {

// 把一维坐标归到 0/1/2 三格之一。左闭右开，超出范围夹到两端。
//
// 用 extent/3 和 extent - extent/3 划界而不是先算格宽再乘：整数除法的余数落在中间格，
// 三格加起来正好是 extent，不会因为舍入丢掉或重复一两个像素。
[[nodiscard]] int Band(int value, int start, int extent) {
    if (extent <= 0) return 1;
    const int offset = value - start;
    if (offset < extent / 3) return 0;
    if (offset < extent - extent / 3) return 1;
    return 2;
}

} // namespace

DragEdges HitZone(const Rect& frame, int x, int y) {
    const int col = Band(x, frame.left, frame.width());
    const int row = Band(y, frame.top, frame.height());

    if (col == 1 && row == 1) return DragEdges{true, true, true, true};

    DragEdges edges;
    edges.left = col == 0;
    edges.right = col == 2;
    edges.top = row == 0;
    edges.bottom = row == 2;
    return edges;
}

Rect ApplyDrag(const Rect& start, const DragEdges& edges, int dx, int dy, int minWidth,
               int minHeight) {
    if (edges.IsMove()) {
        return Rect{start.left + dx, start.top + dy, start.right + dx, start.bottom + dy};
    }

    Rect out = start;
    if (edges.left) out.left = start.left + dx;
    if (edges.right) out.right = start.right + dx;
    if (edges.top) out.top = start.top + dy;
    if (edges.bottom) out.bottom = start.bottom + dy;

    // 夹到下限时，动的是被拖的那条边，对面那条始终保持不动。没有这一段，用力拖过头
    // 会把窗口拉成一条线甚至左右反转——而那只在拖得够猛时才出现，手工测试最容易漏。
    const int minW = std::max(1, minWidth);
    const int minH = std::max(1, minHeight);
    if (out.right - out.left < minW) {
        if (edges.left) {
            out.left = out.right - minW;
        } else {
            out.right = out.left + minW;
        }
    }
    if (out.bottom - out.top < minH) {
        if (edges.top) {
            out.top = out.bottom - minH;
        } else {
            out.bottom = out.top + minH;
        }
    }
    return out;
}

} // namespace windowmark
