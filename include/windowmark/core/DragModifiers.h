#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace windowmark {

// 触发窗口拖动的修饰键集合。**任一按下即触发**，不是组合键——这正是它不能复用 Hotkey
// 的原因：Hotkey 表达「同时按住 Ctrl 和 Alt 和 T」，这里表达「右 Alt 或者左 Win」。
//
// keys 存 Win32 虚拟键码。平台值放在 core 是刻意的，理由同 Hotkey.h：只有一个后端，
// 再定义一套平行枚举等于一张两边都没有内容的翻译表。
struct DragModifiers {
    // 规范顺序：预设按设置界面里复选框的排列在前，其余按键码。ParseDragModifiers
    // 保证这一点，所以 operator== 直接比较就是集合相等。
    std::vector<unsigned> keys;

    [[nodiscard]] bool Empty() const { return keys.empty(); }
    [[nodiscard]] bool Contains(unsigned vk) const;

    friend bool operator==(const DragModifiers& a, const DragModifiers& b) {
        return a.keys == b.keys;
    }
};

// 解析 "RAlt|LWin"。大小写不敏感，容忍分隔符周围的空格。
//
// 不认识的名字**跳过它自己**而不是拒绝整条：配置文件是手写的，一个拼错不该让整个功能
// 静默失效——那种失败最难排查，因为看起来一切正常。
[[nodiscard]] DragModifiers ParseDragModifiers(std::string_view text);

// 规范形式，按 LAlt RAlt LWin RWin LCtrl RCtrl 的固定顺序，其余键附在后面。
// 与输入顺序无关，这样往返一次就稳定下来，配置文件不会因为重排而产生无谓的 diff。
[[nodiscard]] std::string FormatDragModifiers(const DragModifiers& mods);

// 设置界面的六个复选框。name 是配置文件里的写法，label 是界面上显示的中文。
struct PresetModifier {
    unsigned vk;
    const char* name;
    const wchar_t* label;
};

[[nodiscard]] const std::vector<PresetModifier>& PresetModifiers();

} // namespace windowmark
