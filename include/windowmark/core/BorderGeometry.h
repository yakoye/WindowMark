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

} // namespace windowmark
