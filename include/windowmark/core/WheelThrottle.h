#pragma once
#include <array>
#include <cstdint>
namespace windowmark {
enum class WheelAction { Pass, Drop, NormalizeLater };
// 只记录已放行的时间，各方向独立，连续输入不会推迟下次放行。
class WheelThrottle {
public:
    bool Accept(bool horizontal, int delta, std::uint64_t now, unsigned interval) {
        if (!delta) return false;
        const auto i = static_cast<unsigned>((horizontal ? 2 : 0) + (delta < 0 ? 1 : 0));
        if (seen_[i] && now >= last_[i] && now - last_[i] < interval) return false;
        seen_[i] = true; last_[i] = now; return true;
    }
    void Reset() { seen_ = {}; last_ = {}; }
    WheelAction Route(bool horizontal, int delta, std::uint64_t now, unsigned interval, bool own) {
        if (own || !delta) return WheelAction::Pass;
        if (!Accept(horizontal, delta, now, interval)) return WheelAction::Drop;
        return delta > 120 || delta < -120 ? WheelAction::NormalizeLater : WheelAction::Pass;
    }
private:
    std::array<bool, 4> seen_{};
    std::array<std::uint64_t, 4> last_{};
};
}
