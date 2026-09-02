#pragma once

#include "windowmark/core/ConfigLocation.h"

#include <windows.h>

#include <filesystem>

namespace windowmark::win {

// 选配置文件放在哪。三个选项正好对应三层查找，不引入第四种概念。
//
// 对话框顶部显示当前实际生效的那一份——因为便携排在显式指定之前，用户可能指定了路径
// 却因为 exe 旁边有个 conf 而没生效，不显示出来没人能想明白。
class WinConfigPathDialog {
public:
    // 用户按下确定时返回 true，chosen 写入选中的完整路径（含文件名）。
    // 调用方据此决定是否写注册表：只有「自定义」才写，另外两个都要清掉。
    static bool ShowModal(HWND owner, const std::filesystem::path& current,
                          ConfigSource currentSource, std::filesystem::path& chosen);
};

} // namespace windowmark::win
