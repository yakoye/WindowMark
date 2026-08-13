#pragma once

#include <string>
#include <windows.h>

namespace windowmark::win {

class WinRenameDialog {
public:
    // Shows a one-field prompt seeded with `name`. Returns true when accepted; `name`
    // then holds the new text, which may be empty to mean "go back to the window title".
    static bool ShowModal(HWND owner, const std::wstring& windowTitle, std::wstring& name);
};

} // namespace windowmark::win
