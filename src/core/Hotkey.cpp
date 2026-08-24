#include "windowmark/core/Hotkey.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace windowmark {
namespace {

// Virtual-key codes, spelled out so this file stays free of <windows.h>. Only the keys a
// shortcut is plausibly bound to are here; anything else parses as "no shortcut", which
// is the right answer for a typo in a hand-edited file.
constexpr unsigned kVkBack = 0x08;
constexpr unsigned kVkTab = 0x09;
constexpr unsigned kVkReturn = 0x0D;
constexpr unsigned kVkPause = 0x13;
constexpr unsigned kVkEscape = 0x1B;
constexpr unsigned kVkSpace = 0x20;
constexpr unsigned kVkPageUp = 0x21;
constexpr unsigned kVkPageDown = 0x22;
constexpr unsigned kVkEnd = 0x23;
constexpr unsigned kVkHome = 0x24;
constexpr unsigned kVkLeft = 0x25;
constexpr unsigned kVkUp = 0x26;
constexpr unsigned kVkRight = 0x27;
constexpr unsigned kVkDown = 0x28;
constexpr unsigned kVkInsert = 0x2D;
constexpr unsigned kVkDelete = 0x2E;
constexpr unsigned kVkF1 = 0x70;   // F1..F24 are contiguous from here

struct NamedKey {
    const char* name;
    unsigned vk;
};

// Longest first is not needed - lookup is exact, not prefix - but the aliases are grouped
// with the canonical name so the reverse lookup below can take the first match as the one
// to print.
constexpr std::array<NamedKey, 24> kNamedKeys{{
    {"SPACE", kVkSpace},
    {"ENTER", kVkReturn},   {"RETURN", kVkReturn},
    {"TAB", kVkTab},
    {"BACKSPACE", kVkBack}, {"BACK", kVkBack},
    {"ESC", kVkEscape},     {"ESCAPE", kVkEscape},
    {"INSERT", kVkInsert},  {"INS", kVkInsert},
    {"DELETE", kVkDelete},  {"DEL", kVkDelete},
    {"HOME", kVkHome},
    {"END", kVkEnd},
    {"PAGEUP", kVkPageUp},  {"PGUP", kVkPageUp},
    {"PAGEDOWN", kVkPageDown}, {"PGDN", kVkPageDown},
    {"LEFT", kVkLeft},
    {"UP", kVkUp},
    {"RIGHT", kVkRight},
    {"DOWN", kVkDown},
    {"PAUSE", kVkPause},
    {"", 0},
}};

[[nodiscard]] std::string Upper(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] std::string Trim(std::string_view text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if(text.begin(), text.end(),
                              [&](char c) { return notSpace(static_cast<unsigned char>(c)); });
    auto end = std::find_if(text.rbegin(), text.rend(),
                            [&](char c) { return notSpace(static_cast<unsigned char>(c)); }).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

// Returns 0 when the token is not a modifier.
[[nodiscard]] unsigned ModifierFromToken(const std::string& token) {
    if (token == "CTRL" || token == "CONTROL" || token == "CTL") return Hotkey::kCtrl;
    if (token == "ALT" || token == "MENU") return Hotkey::kAlt;
    if (token == "SHIFT") return Hotkey::kShift;
    if (token == "WIN" || token == "WINDOWS" || token == "META" || token == "SUPER") {
        return Hotkey::kWin;
    }
    return 0;
}

// Returns 0 when the token is not a key this accepts.
[[nodiscard]] unsigned KeyFromToken(const std::string& token) {
    if (token.size() == 1) {
        const char c = token.front();
        if (c >= 'A' && c <= 'Z') return static_cast<unsigned>(c);
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c);
        return 0;
    }
    // F1..F24. Checked before the named table so "F1" never collides with a name.
    if (token.size() >= 2 && token.front() == 'F') {
        const std::string digits = token.substr(1);
        if (!digits.empty() &&
            std::all_of(digits.begin(), digits.end(),
                        [](char c) { return c >= '0' && c <= '9'; })) {
            const int number = std::stoi(digits);
            if (number >= 1 && number <= 24) {
                return kVkF1 + static_cast<unsigned>(number - 1);
            }
            return 0;
        }
    }
    for (const auto& named : kNamedKeys) {
        if (named.vk != 0 && token == named.name) return named.vk;
    }
    return 0;
}

[[nodiscard]] std::string KeyToken(unsigned vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= kVkF1 && vk <= kVkF1 + 23) {
        return "F" + std::to_string(vk - kVkF1 + 1);
    }
    for (const auto& named : kNamedKeys) {
        if (named.vk == vk) return named.name;   // first match is the canonical spelling
    }
    return {};
}

} // namespace

Hotkey ParseHotkey(std::string_view text) {
    Hotkey result{};
    const std::string upper = Upper(text);

    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start <= upper.size()) {
        const std::size_t plus = upper.find('+', start);
        const std::size_t end = plus == std::string::npos ? upper.size() : plus;
        tokens.push_back(Trim(std::string_view(upper).substr(start, end - start)));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    // A trailing '+' means the key itself is '+', which none of this accepts; and an empty
    // token anywhere else is a typo. Either way the whole string is rejected.
    if (tokens.empty()) return {};

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token.empty()) return {};
        if (const unsigned mod = ModifierFromToken(token); mod != 0) {
            if ((result.mods & mod) != 0) return {};   // "Ctrl+Ctrl+T"
            result.mods |= mod;
            continue;
        }
        // Not a modifier, so it has to be the key - and it has to be the last token.
        if (i + 1 != tokens.size()) return {};
        result.key = KeyFromToken(token);
        if (result.key == 0) return {};
    }

    if (!result.Valid()) return {};
    return result;
}

std::string FormatHotkey(const Hotkey& hotkey) {
    if (!hotkey.Valid()) return {};
    const std::string key = KeyToken(hotkey.key);
    if (key.empty()) return {};

    std::string out;
    // Fixed order regardless of how it was typed, so the settings file does not churn.
    if ((hotkey.mods & Hotkey::kCtrl) != 0) out += "Ctrl+";
    if ((hotkey.mods & Hotkey::kAlt) != 0) out += "Alt+";
    if ((hotkey.mods & Hotkey::kShift) != 0) out += "Shift+";
    if ((hotkey.mods & Hotkey::kWin) != 0) out += "Win+";
    out += key;
    return out;
}

std::wstring FormatHotkeyWide(const Hotkey& hotkey) {
    const std::string narrow = FormatHotkey(hotkey);
    // Every character this can produce is ASCII, so widening one byte at a time is exact.
    return std::wstring(narrow.begin(), narrow.end());
}

} // namespace windowmark
