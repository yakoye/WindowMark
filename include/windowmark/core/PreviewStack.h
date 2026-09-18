#pragma once

#include "windowmark/core/Types.h"

#include <vector>

namespace windowmark {

// 书签 / 浮动标题 / 缩略图，三段依次排开，任何时刻互不重叠。
//
// 从窗口边缘往窗口内容方向，永远是：书签 → 标题 → 缩略图。四个方向只差两件事：
//   主轴是哪根轴（Top/Bottom 是 X，Left/Right 是 Y）
//   交叉轴往哪边长（Bottom 向上、Top 向下、Left 向右、Right 向左）
// 所以只有这一个函数，Top/Bottom、Left/Right 的镜像关系是算出来的，不是分别写出来的。
//
// 它只管空间关系，不知道磁场：书签的 visual 矩形是输入，谁是主标签、标签多大都由调用方算好。
struct PreviewStackInput {
    Placement placement{Placement::Bottom};   // Auto 按 Bottom 处理
    RectF workArea;
    // 书签条贴着的那条窗口边，在交叉轴上的屏幕坐标（Bottom 是书签底边的 y，Left 是书签左边的 x）
    float rootEdge{};
    // 这一帧所有书签的 visual 矩形，屏幕坐标
    std::vector<RectF> tabs;
    // 标题和缩略图在主轴上以它为中心
    float anchorMain{};
    // 尺寸都按「主轴、交叉轴」给：横排标题是 (宽, 高)，竖排标题是 (高, 宽)
    float titleMain{};
    float titleCross{};
    float thumbnailMain{};    // 0 = 没有缩略图
    float thumbnailCross{};
    float titleGap{};         // 书签 → 标题
    float thumbnailGap{};     // 标题 → 缩略图
    // 缩略图在交叉轴上放不下时按比例缩小；缩到比这还小就不显示
    float minThumbnailMain{};
    float minThumbnailCross{};
};

struct PreviewStackLayout {
    RectF title;
    RectF thumbnail;
    bool thumbnailVisible{false};
};

// 交叉轴：
//   标题起点 = 根边 + 标题主轴跨度内所有书签的最大深度 + titleGap
//   缩略图起点 = max(标题尾 + thumbnailGap, 根边 + 缩略图跨度内所有书签的最大深度 + titleGap)
// 主轴：以 anchorMain 为中心，夹进工作区。
//
// 取「跨度内所有书签的最大深度」而不是主标签的深度：标题比书签宽，会盖到邻居头上，只看主标签
// 就可能压住一个正在长高的邻居。最大值对每个书签的深度连续，所以书签长高时标题和缩略图也是连续
// 地被推开，间距始终不变。
[[nodiscard]] PreviewStackLayout LayoutPreviewStack(const PreviewStackInput& input);

// 两个矩形是否重叠（边贴边不算）。给测试和调用方自检用。
[[nodiscard]] bool RectsOverlap(const RectF& a, const RectF& b) noexcept;

} // namespace windowmark
