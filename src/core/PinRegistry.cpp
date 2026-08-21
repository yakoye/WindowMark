#include "windowmark/core/PinRegistry.h"

#include <algorithm>

namespace windowmark {

bool PinRegistry::Add(WindowId id, bool wasTopmostBefore) {
    if (records_.contains(id)) return false;
    records_.emplace(id, Entry{wasTopmostBefore, nextOrder_++});
    return true;
}

std::optional<bool> PinRegistry::Remove(WindowId id) {
    const auto it = records_.find(id);
    if (it == records_.end()) return std::nullopt;
    const bool was = it->second.wasTopmostBefore;
    records_.erase(it);
    return was;
}

bool PinRegistry::Contains(WindowId id) const {
    return records_.contains(id);
}

std::vector<PinRecord> PinRegistry::Snapshot() const {
    std::vector<std::pair<std::size_t, PinRecord>> ordered;
    ordered.reserve(records_.size());
    for (const auto& [id, entry] : records_) {
        ordered.emplace_back(entry.order, PinRecord{id, entry.wasTopmostBefore});
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<PinRecord> out;
    out.reserve(ordered.size());
    for (auto& [order, record] : ordered) {
        (void)order;
        out.push_back(record);
    }
    return out;
}

std::vector<PinRecord> PinRegistry::Drain() {
    auto out = Snapshot();
    records_.clear();
    // Deliberately not resetting nextOrder_: it only has to be increasing, and restarting
    // it would let a later pin sort ahead of one that is somehow still around.
    return out;
}

} // namespace windowmark
