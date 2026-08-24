#pragma once

#include <string>
#include <string_view>

namespace windowmark {

// A global shortcut, parsed from the human-editable form the settings file stores
// ("Ctrl+Alt+T"). Kept in core rather than the platform layer because the parsing is
// where the mistakes live and core is the half that has tests.
//
// `key` is a Win32 virtual-key code. That is a platform value sitting in core, which is
// deliberate: there is one backend, and a parallel enum would be a translation table with
// nothing on the other side. The few codes needed are spelled out below so this header
// does not drag in <windows.h>.
struct Hotkey {
    // Bit values match Win32 MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN so the platform
    // layer can hand `mods` straight to RegisterHotKey.
    static constexpr unsigned kAlt = 0x0001;
    static constexpr unsigned kCtrl = 0x0002;
    static constexpr unsigned kShift = 0x0004;
    static constexpr unsigned kWin = 0x0008;

    unsigned mods{0};
    unsigned key{0};

    // A shortcut with no modifier is rejected on purpose: RegisterHotKey would happily
    // take plain "T" away from every other program on the desktop.
    [[nodiscard]] bool Valid() const { return key != 0 && mods != 0; }
    [[nodiscard]] bool Empty() const { return key == 0 && mods == 0; }

    friend bool operator==(const Hotkey& a, const Hotkey& b) {
        return a.mods == b.mods && a.key == b.key;
    }
};

// Parses "Ctrl+Alt+T" and its variants. Case-insensitive, tolerant of spaces around the
// separators. An empty or unparseable string yields an empty Hotkey, which callers treat
// as "no shortcut" - there is no separate error channel because the settings file is
// hand-editable and a typo should disable the shortcut, not refuse to start.
[[nodiscard]] Hotkey ParseHotkey(std::string_view text);

// Canonical form, always "Ctrl+Alt+Shift+Win+KEY" order regardless of how it was typed.
// An empty or invalid Hotkey formats as "".
[[nodiscard]] std::string FormatHotkey(const Hotkey& hotkey);

// Same text as FormatHotkey, for the settings dialog. Separated so the UI can show a
// placeholder for the empty case without the config file gaining one.
[[nodiscard]] std::wstring FormatHotkeyWide(const Hotkey& hotkey);

} // namespace windowmark
