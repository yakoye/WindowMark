#pragma once
#include <windows.h>
#include <cstdint>
namespace ime_guard {
struct WidthHint {
    std::int64_t milliDip{};
    UINT dpi{};
    RECT taskbar{};
};
inline bool Valid(const WidthHint& h) {
    return h.milliDip >= 12000 && h.milliDip <= 512000 && h.dpi >= 48 && h.dpi <= 960 &&
        h.taskbar.right > h.taskbar.left && h.taskbar.bottom > h.taskbar.top;
}
inline bool Matches(const WidthHint& h, HWND bar) {
    RECT r{};
    return Valid(h) && bar && GetWindowRect(bar,&r) && GetDpiForWindow(bar)==h.dpi &&
        EqualRect(&r,&h.taskbar);
}
}
