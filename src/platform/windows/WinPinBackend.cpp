#include "WinPinBackend.h"

#include <cstdint>
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
    if (!IsWindow(hwnd)) return std::nullopt;

    const bool was = IsTopmost(hwnd);
    if (was == topmost) return was;

    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
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
