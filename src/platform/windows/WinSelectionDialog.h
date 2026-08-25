#pragma once

#include "windowmark/core/Types.h"

#include <windows.h>

#include <functional>
#include <vector>

namespace windowmark::win {

// Two features use this panel now - bookmarks and border exclusion - and they differ only
// in wording and in what they do as the highlight moves. Everything else, including the
// two-level app/window checkbox model, is identical.
struct SelectionDialogOptions {
    const wchar_t* title{L"WindowMark - 选择需要书签的应用/窗口"};
    const wchar_t* note{
        L"说明：应用勾选状态会保存；单个窗口勾选状态只对本次运行有效。"
        L"应用未勾选时，其下面窗口即使勾选也不会显示书签。"};
    // Called with the window id as the selected row changes, and with 0 when the selection
    // is an app row or the panel closes. The border page uses it to outline whatever the
    // cursor is on, which answers "which one is this" without numbering every window on
    // screen the way the inspect tool has to.
    std::function<void(WindowId)> onHighlight;
    // What a tick means. The bookmark panel is a "choose what participates" list, so a tick
    // is `enabled`. The border panel is reached from a menu item that says 排除应用, and
    // behind a label like that a tick has to mean excluded - otherwise the user is reading
    // a double negative and will click the wrong row.
    //
    // Only the display flips. AppSelectionModel::enabled keeps its one meaning everywhere,
    // so the Coordinator never has to know which panel it came from.
    bool checkedMeansExcluded{false};
};

class WinSelectionDialog {
public:
    // Returns true only when the user pressed Apply. The supplied model is
    // updated in-place from the checkbox states.
    static bool ShowModal(HWND owner, std::vector<AppSelectionModel>& selection,
                          const SelectionDialogOptions& options = {});
};

} // namespace windowmark::win
