#pragma once

#include <windows.h>

#include <string>

namespace windowmark::win {

// 拖动的触发键设置：六个复选框，按住其中**任意一个**再拖鼠标即可。
//
// 只呈现那六个预设。配置文件里可以写别的键（F13 之类），界面不显示它们，确定时也原样
// 保留——界面看不见的东西不该被界面悄悄删掉。
class WinDragSettingsDialog {
public:
    // 用户按下确定时返回 true，modifiers 被改写成新的规范形式（"RAlt|LWin"）。
    // 取消则原样不动。
    static bool ShowModal(HWND owner, std::string& modifiers);
};

} // namespace windowmark::win
