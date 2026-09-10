#include "windowmark/core/DragModifiers.h"

#include "windowmark/core/Hotkey.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace windowmark {
namespace {

// Win32 虚拟键码。写在这里而不是 include <windows.h>，与 Hotkey.cpp 同一个理由：
// core 不该为了八个常量拖进整个 windows.h。
constexpr unsigned kVkLShift = 0xA0;
constexpr unsigned kVkRShift = 0xA1;
constexpr unsigned kVkLControl = 0xA2;
constexpr unsigned kVkRControl = 0xA3;
constexpr unsigned kVkLMenu = 0xA4;
constexpr unsigned kVkRMenu = 0xA5;
constexpr unsigned kVkLWin = 0x5B;
constexpr unsigned kVkRWin = 0x5C;

// 顺序即 FormatDragModifiers 的输出顺序，也是设置界面里复选框的排列顺序。
const std::vector<PresetModifier>& Presets() {
    static const std::vector<PresetModifier> presets{
        {kVkLMenu, "LAlt", L"左 Alt 键"},
        {kVkRMenu, "RAlt", L"右 Alt 键"},
        {kVkLWin, "LWin", L"左 Win 键"},
        {kVkRWin, "RWin", L"右 Win 键"},
        {kVkLControl, "LCtrl", L"左 Ctrl 键"},
        {kVkRControl, "RCtrl", L"右 Ctrl 键"},
    };
    return presets;
}

// 预设之外还认的名字：左右 Shift。Shift 不在六个复选框里（它常被应用自己用作扩选），
// 但配置文件写了就认。
constexpr std::pair<const char*, unsigned> kExtraNames[]{
    {"LSHIFT", kVkLShift},
    {"RSHIFT", kVkRShift},
};

[[nodiscard]] std::string Upper(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

void Trim(std::string& value) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
}

[[nodiscard]] unsigned LookupOne(std::string_view token) {
    std::string name(token);
    Trim(name);
    if (name.empty()) return 0;
    const std::string upper = Upper(name);

    for (const auto& preset : Presets()) {
        if (upper == Upper(preset.name)) return preset.vk;
    }
    for (const auto& [text, vk] : kExtraNames) {
        if (upper == text) return vk;
    }
    // 落到 Hotkey 的键名表：F1-F24、Space、字母数字等。配置文件能补任意按键，而键名
    // 该长什么样这件事只该有一个答案——两份表必然漂移。
    return ParseKeyName(name);
}

// 预设在 Presets() 里的位置，不是预设的排在所有预设之后。
[[nodiscard]] std::size_t OrderOf(unsigned vk) {
    const auto& presets = Presets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].vk == vk) return i;
    }
    return presets.size();
}

// 规范顺序：预设按复选框的排列在前，其余按键码。
//
// 顺序无意义的东西就不该存成有顺序的样子——否则每个用到它的地方都要各自记得「顺序不
// 算数」，而总有一处会忘。排好之后 operator== 直接可用，格式化也不用再重排一遍。
void SortCanonical(std::vector<unsigned>& keys) {
    std::sort(keys.begin(), keys.end(), [](unsigned a, unsigned b) {
        const std::size_t oa = OrderOf(a);
        const std::size_t ob = OrderOf(b);
        if (oa != ob) return oa < ob;
        return a < b;
    });
}

} // namespace

bool DragModifiers::Contains(unsigned vk) const {
    return std::find(keys.begin(), keys.end(), vk) != keys.end();
}

DragModifiers ParseDragModifiers(std::string_view text) {
    DragModifiers out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t bar = text.find('|', start);
        const std::size_t end = bar == std::string_view::npos ? text.size() : bar;
        const unsigned vk = LookupOne(text.substr(start, end - start));
        if (vk != 0 && !out.Contains(vk)) out.keys.push_back(vk);
        if (bar == std::string_view::npos) break;
        start = bar + 1;
    }
    SortCanonical(out.keys);
    return out;
}

std::string FormatDragModifiers(const DragModifiers& mods) {
    std::string out;
    const auto append = [&out](const char* name) {
        if (!out.empty()) out.push_back('|');
        out += name;
    };

    // keys 已经是规范顺序（ParseDragModifiers 排过），顺着走即可。
    for (const unsigned vk : mods.keys) {
        const auto& presets = Presets();
        const auto preset = std::find_if(presets.begin(), presets.end(),
                                         [vk](const PresetModifier& p) { return p.vk == vk; });
        if (preset != presets.end()) {
            append(preset->name);
            continue;
        }
        bool named = false;
        for (const auto& [text, extraVk] : kExtraNames) {
            if (extraVk == vk) {
                append(text);
                named = true;
                break;
            }
        }
        if (!named) {
            const std::string keyName = FormatKeyName(vk);
            if (!keyName.empty()) append(keyName.c_str());
        }
    }
    return out;
}

const std::vector<PresetModifier>& PresetModifiers() { return Presets(); }

} // namespace windowmark
