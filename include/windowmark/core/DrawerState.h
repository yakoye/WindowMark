#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace windowmark {

// Platform-neutral drawer interaction state.
// UI backends provide timestamps and render the returned extents.
class DrawerState {
public:
    DrawerState(int collapsedExtent, int expandedExtent, int animationMs);

    void Reset(std::size_t itemCount);
    void SetHovered(int index, std::uint64_t nowMs);
    [[nodiscard]] bool Tick(std::uint64_t nowMs);

    [[nodiscard]] int HoveredIndex() const noexcept { return hoveredIndex_; }
    [[nodiscard]] bool IsAnimating() const noexcept { return animating_; }
    [[nodiscard]] const std::vector<float>& Extents() const noexcept { return current_; }

private:
    [[nodiscard]] float TargetFor(std::size_t index) const noexcept;

    int collapsedExtent_{};
    int expandedExtent_{};
    int animationMs_{};
    int hoveredIndex_{-1};
    bool animating_{false};
    std::uint64_t animationStartMs_{};
    std::vector<float> current_;
    std::vector<float> start_;
};

} // namespace windowmark
