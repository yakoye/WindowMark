#pragma once

#include "windowmark/core/Settings.h"

#include <windows.h>

namespace windowmark::win {

// Bookmarks and borders are separate features and get separate windows, so neither one's
// settings are buried under the other's. They share this implementation but not their
// field lists, which keeps either side liftable on its own.
enum class SettingsPage {
    Bookmarks,
    Borders,
    Pinning,
};

class WinSettingsDialog {
public:
    // Edits `settings` in place. Returns true when the user accepted, in which case the
    // caller is responsible for persisting and applying the result.
    //
    // Only numbers and switches live here. Anything that opens another window - the app
    // pickers for bookmarks and for border exclusion - sits in the tray menu instead, so
    // this dialog stays a plain form with no way to reach the Coordinator.
    static bool ShowModal(HWND owner, Settings& settings, SettingsPage page);
};

} // namespace windowmark::win
