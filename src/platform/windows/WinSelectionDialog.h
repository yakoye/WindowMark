#pragma once

#include "windowmark/core/Types.h"

#include <windows.h>

#include <vector>

namespace windowmark::win {

class WinSelectionDialog {
public:
    // Returns true only when the user pressed Apply. The supplied model is
    // updated in-place from the checkbox states.
    static bool ShowModal(HWND owner, std::vector<AppSelectionModel>& selection);
};

} // namespace windowmark::win
