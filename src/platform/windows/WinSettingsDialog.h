#pragma once

#include "windowmark/core/Settings.h"

#include <windows.h>

namespace windowmark::win {

class WinSettingsDialog {
public:
    // Edits `settings` in place. Returns true when the user accepted, in which case the
    // caller is responsible for persisting and applying the result.
    static bool ShowModal(HWND owner, Settings& settings);
};

} // namespace windowmark::win
