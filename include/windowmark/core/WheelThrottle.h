#pragma once
#include <array>
#include <cstdint>
namespace windowmark {
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
private:
    std::array<bool, 4> seen_{};
    std::array<std::uint64_t, 4> last_{};
};
}
