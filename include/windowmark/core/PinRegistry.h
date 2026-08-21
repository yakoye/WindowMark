#pragma once

#include "windowmark/core/Types.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace windowmark {

// Which windows this process has pinned, and what their always-on-top state was before it
// touched them.
//
// Session-only, like custom bookmark labels and per-window selection, and for the same
// reason: an OS window handle has no reliable identity across runs, so a persisted entry
// would eventually re-attach itself to whatever window inherited the handle.
//
// No platform types here - the whole thing is testable without a window manager.
class PinRegistry {
public:
    // Returns false when the window is already pinned. The caller must not overwrite the
    // stored original state in that case: pin, pin again, unpin has to leave the window
    // the way it was found, not the way it was after the first pin.
    bool Add(WindowId id, bool wasTopmostBefore);

    // Removes the window and reports what it was before being pinned, so the caller can
    // restore that rather than blindly clearing the always-on-top style. Empty when the
    // window was not pinned.
    std::optional<bool> Remove(WindowId id);

    [[nodiscard]] bool Contains(WindowId id) const;
    [[nodiscard]] bool Empty() const noexcept { return records_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return records_.size(); }

    // Stable order: insertion order, so the tray menu does not reshuffle itself between
    // openings.
    [[nodiscard]] std::vector<PinRecord> Snapshot() const;

    // Empties the registry and hands back every record, for restoring on shutdown or when
    // the feature is switched off.
    std::vector<PinRecord> Drain();

private:
    struct Entry {
        bool wasTopmostBefore{};
        std::size_t order{};
    };

    std::unordered_map<WindowId, Entry> records_;
    std::size_t nextOrder_{0};
};

} // namespace windowmark
