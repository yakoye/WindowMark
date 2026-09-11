#pragma once

#include "windowmark/core/Types.h"

namespace windowmark {

// 把边框的外矩形夹回窗口所在的屏幕。
//
// 边框画在窗口外 reach 像素。窗口最大化时它的可见边界正好等于工作区，那圈外扩就落到
// 了边界外面——双屏时越过分界线画到隔壁屏上，底边则压在任务栏上面（边框窗口是 topmost，
// 不会被任务栏遮住）。半屏吸附同理。
//
// **两个矩形都要看，工作区优先**：
//   - 最大化 / 半屏吸附的窗口贴的是**工作区**边界，底边尤其如此（1020 而不是 1080）。
//     只跟监视器比的话底边差着整条任务栏的高度，判据不成立，边框就压上去了。
//   - 用户手动拖到盖住任务栏的窗口贴的是**监视器**边界。这时按工作区判断 gap 为负，
//     落到监视器这一档才夹得对。
//
// 只夹「窗口自己已经贴上去」的那几条边，判据是 gap 落在 [0, reach] 之内。下界 0 不能省：
// 手动横跨两块屏的窗口，边缘落在本监视器外面（gap 为负），没有下界那条边会被误裁。
//
// 只做几何，不认识 HMONITOR——两个矩形都由平台层查好后传进来，这样这段判断可以脱离
// 显示器配置被测试。
[[nodiscard]] Rect ClampBorderToScreen(const Rect& frame, const Rect& outer,
                                       const Rect& monitor, const Rect& workArea, int reach);

// 圆角边框环怎么画。
//
// corners 一开，**整圈**都是一条抗锯齿的弧矩形——直边也是它画出来的，不再有独立的直边
// 段。所以 cornerWidthExtra 加的是整圈的宽，不只是四个角，名字容易让人以为只影响角上。
//
// 加宽只能往**窗口外**长。窗口内侧该盖多少是 offset 一个人说了算：盖 -offset 像素，正好
// 压住 Windows 自己那条 1px 边框（实测 Explorer #646765、Chrome #4F5255），不多不少。
// 往内长会连窗口内容一起吃掉——实测 width=3 / offset=-1 / widthExtra=3 时窗口内被盖了
// 4px，而其中只有 1px 是压那条灰边需要的。
struct RoundedRing {
    float width{};   // 画笔宽度
    float inset{};   // 路径中心线相对外矩形往窗口里的距离，可为负（落在外矩形之外）
    int grow{};      // 裁剪单元要在外矩形基础上外扩多少，否则长出去的那截被自己切掉
};

// stroke = 线宽，widthExtra = 加宽，cornerInset = 整圈往窗口中心挪多少（负数往外）。
//
// 不收 reach：inset 是相对外矩形算的，而外矩形本身就是 frame - reach，reach 在式子里
// 自然抵消。收着它只会让人以为结果跟它有关。换算回窗口坐标时才需要它，那是下面两个
// 函数的事。
[[nodiscard]] RoundedRing RoundedRingOf(int stroke, int widthExtra, int cornerInset);

// 环的内沿 / 外沿相对**窗口边缘**的位置，正数表示在窗口里面。测试用它把契约钉死，
// 免得正负号和「相对谁」错一次就偏几个像素——那种偏差得盯着屏幕看才发现。
[[nodiscard]] float RingInnerEdge(const RoundedRing& ring, int reach);
[[nodiscard]] float RingOuterEdge(const RoundedRing& ring, int reach);

} // namespace windowmark
