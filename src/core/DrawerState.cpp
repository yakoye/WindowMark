#include "windowmark/core/DrawerState.h"

#include <algorithm>

namespace windowmark {
namespace {

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    const float p = 1.0F - t;
    return 1.0F - p * p * p;
}

} // namespace

DrawerState::DrawerState(int collapsedExtent, int expandedExtent, int animationMs)
    : collapsedExtent_(std::max(1, collapsedExtent)),
      expandedExtent_(std::max(collapsedExtent_, expandedExtent)),
      animationMs_(std::max(0, animationMs)) {}

void DrawerState::Reset(std::size_t itemCount) {
    hoveredIndex_ = -1;
    animating_ = false;
    current_.assign(itemCount, static_cast<float>(collapsedExtent_));
    start_ = current_;
}

void DrawerState::SetHovered(int index, std::uint64_t nowMs) {
    if (index < -1 || index >= static_cast<int>(current_.size())) index = -1;
    if (index == hoveredIndex_) return;

    hoveredIndex_ = index;
    animationStartMs_ = nowMs;
    start_ = current_;

    if (animationMs_ == 0) {
        for (std::size_t i = 0; i < current_.size(); ++i) current_[i] = TargetFor(i);
        animating_ = false;
    } else {
        animating_ = !current_.empty();
    }
}

bool DrawerState::Tick(std::uint64_t nowMs) {
    if (!animating_) return false;

    const auto elapsed = nowMs >= animationStartMs_ ? nowMs - animationStartMs_ : 0;
    const float raw = static_cast<float>(elapsed) / static_cast<float>(std::max(1, animationMs_));
    const float t = EaseOutCubic(raw);

    for (std::size_t i = 0; i < current_.size(); ++i) {
        const float start = i < start_.size() ? start_[i] : TargetFor(i);
        const float target = TargetFor(i);
        current_[i] = start + (target - start) * t;
    }

    if (raw >= 1.0F) {
        for (std::size_t i = 0; i < current_.size(); ++i) current_[i] = TargetFor(i);
        animating_ = false;
    }
    return true;
}

float DrawerState::TargetFor(std::size_t index) const noexcept {
    return static_cast<int>(index) == hoveredIndex_
        ? static_cast<float>(expandedExtent_)
        : static_cast<float>(collapsedExtent_);
}

} // namespace windowmark
