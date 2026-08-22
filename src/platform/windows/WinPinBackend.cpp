#include "WinPinBackend.h"

#include "PinDiag.h"

#include <cstdint>
#include <iterator>
#include <utility>

namespace windowmark::win {
namespace {

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

[[nodiscard]] bool IsTopmost(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

} // namespace

bool WinPinBackend::Start(const Settings& settings, PinCallbacks callbacks) {
    settings_ = settings;
    callbacks_ = std::move(callbacks);
    started_ = true;
    return true;
}

// Setting the always-on-top style on another process's window is allowed, and was measured
// to be: the style bit really does flip. Worth stating because a neighbouring restriction
// looks like it should apply and does not - Windows silently ignores z-order *raises*
// requested by a process that does not own the foreground window, returning TRUE and
// changing nothing. That rule governs moving within a band, not entering the topmost one.
std::optional<bool> WinPinBackend::SetTopmost(WindowId id, bool topmost) {
    HWND hwnd = HwndFromId(id);
    if (!IsWindow(hwnd)) {
        PinDiag(L"SetTopmost: hwnd 已失效 id=%llu", (unsigned long long)id);
        return std::nullopt;
    }

    const bool was = IsTopmost(hwnd);
    if (was == topmost) return was;

    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    const bool now = IsTopmost(hwnd);
    PinDiag(L"SetTopmost: %ls 目标=%d 之前=%d 之后=%d 最小化=%d", cls, topmost ? 1 : 0,
            was ? 1 : 0, now ? 1 : 0, IsIconic(hwnd) ? 1 : 0);
    return was;
}

void WinPinBackend::Apply(const std::vector<PinRecord>& pinned) {
    pinned_ = pinned;
}

void WinPinBackend::UpdateSettings(const Settings& settings) {
    settings_ = settings;
}

void WinPinBackend::Stop() noexcept {
    // No unpinning here. The Coordinator owns the registry and restores every window
    // before it stops the backends, because it is the only side that knows what each
    // window's state was before it was pinned.
    pinned_.clear();
    callbacks_ = {};
    started_ = false;
}

} // namespace windowmark::win
