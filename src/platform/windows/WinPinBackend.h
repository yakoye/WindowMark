#pragma once

#include "windowmark/core/Interfaces.h"
#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <windows.h>

#include <optional>
#include <vector>

namespace windowmark::win {

// Always-on-top for other processes' windows.
//
// Deliberately does not render anything. The outline a pinned window gets comes from the
// border backend, which is already the one thing on this desktop that draws outlines -
// a second renderer would mean deciding, every frame, which of the two owns a window that
// is both active and pinned.
class WinPinBackend final : public IPinBackend {
public:
    bool Start(const Settings& settings, PinCallbacks callbacks) override;
    std::optional<bool> SetTopmost(WindowId id, bool topmost) override;
    void Apply(const std::vector<PinRecord>& pinned) override;
    void UpdateSettings(const Settings& settings) override;
    void Stop() noexcept override;

    // What the tray submenu lists. Held here rather than re-derived, so opening the menu
    // costs nothing.
    [[nodiscard]] const std::vector<PinRecord>& Pinned() const noexcept { return pinned_; }

private:
    Settings settings_;
    PinCallbacks callbacks_;
    std::vector<PinRecord> pinned_;
    bool started_{false};
};

} // namespace windowmark::win
