#pragma once

#include "windowmark/core/Types.h"

namespace windowmark {

// 把边框的外矩形夹回窗口所在的监视器。
//
// 边框画在窗口外 reach 像素。窗口最大化时它的可见边界正好等于工作区，那圈外扩就落到
// 了屏幕外面——单屏被裁掉看不见，双屏则越过分界线画到隔壁屏上。半屏吸附同理。
//
// 只夹「窗口自己已经贴到监视器边界」的那几条边，判据是 gap 落在 [0, reach] 之内。
// 下界 0 不能省：手动横跨两块屏的窗口，边缘落在本监视器外面（gap 为负），没有下界
// 那条边会被误裁，那是回归而不是修复。
//
// 只做几何，不认识 HMONITOR——monitor 由平台层查好后传进来，这样这段判断可以脱离
// 显示器配置被测试。
[[nodiscard]] Rect ClampBorderToMonitor(const Rect& frame, const Rect& outer,
                                        const Rect& monitor, int reach);

} // namespace windowmark
