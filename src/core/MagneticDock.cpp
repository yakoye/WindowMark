#include "windowmark/core/MagneticDock.h"

#include <algorithm>
#include <cmath>

namespace windowmark {
namespace {

constexpr float kPi = 3.14159265358979323846F;

// 书签 i 的峰值：设置给的峰值和它自己的 base 取大。只放大，不缩小——base 已经比峰值大的书签
// （比如激活标签的深度被设得很大）被吸到时保持原样，而不是反而变小。
[[nodiscard]] float PeakFor(float base, float peak) { return std::max(base, peak); }

} // namespace

float MagnetInfluence(float distance, float radius) {
    if (radius <= 0.0F) return distance <= 0.0F ? 1.0F : 0.0F;
    const float d = std::fabs(distance);
    if (d >= radius) return 0.0F;
    return 0.5F * (1.0F + std::cos(kPi * d / radius));
}

std::vector<float> DockBaseStarts(const std::vector<DockItemBase>& items, float gap, float origin) {
    std::vector<float> starts;
    starts.reserve(items.size());
    float at = origin;
    for (const DockItemBase& item : items) {
        starts.push_back(at);
        at += item.main + gap;
    }
    return starts;
}

int DockItemAt(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
               float pointer) {
    const std::size_t n = std::min(items.size(), baseStarts.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (pointer >= baseStarts[i] && pointer < baseStarts[i] + items[i].main) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void DockTargetSizes(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
                     const DockParams& params, float pointer, float strength,
                     std::vector<DockItemVisual>& out) {
    const std::size_t n = std::min(items.size(), baseStarts.size());
    out.resize(n);
    const float s = std::clamp(strength, 0.0F, 1.0F);
    for (std::size_t i = 0; i < n; ++i) {
        const DockItemBase& base = items[i];
        const float center = baseStarts[i] + base.main * 0.5F;
        const float influence = s * MagnetInfluence(pointer - center, params.radius);
        DockItemVisual& v = out[i];
        v.influence = influence;
        v.main = base.main + (PeakFor(base.main, params.peakMain) - base.main) * influence;
        v.cross = base.cross + (PeakFor(base.cross, params.peakCross) - base.cross) * influence;
    }
}

void DockArrange(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
                 float gap, float pointer, std::vector<DockItemVisual>& inOut) {
    const std::size_t n = std::min({items.size(), baseStarts.size(), inOut.size()});
    if (n == 0) return;

    // 先定一个锚点书签 k 和它的 visual 起点，其余的从它向两侧排开。
    std::size_t k = 0;
    float anchorStart = 0.0F;
    const float firstStart = baseStarts[0];
    const float lastEnd = baseStarts[n - 1] + items[n - 1].main;

    if (pointer < firstStart) {
        // 鼠标在第一个书签之前：鼠标和栏首之间是空白，不缩放，栏首原地不动。
        k = 0;
        anchorStart = firstStart;
    } else if (pointer >= lastEnd) {
        // 鼠标在最后一个书签之后：栏尾原地不动，整条栏只向前挤。
        k = n - 1;
        anchorStart = lastEnd - inOut[n - 1].main;
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            const float b = baseStarts[i];
            const float e = b + items[i].main;
            if (pointer >= b && pointer < e) {
                // 在书签内部：保持鼠标落在它 visual 长度上的同一比例处。
                const float f = items[i].main > 0.0F ? (pointer - b) / items[i].main : 0.0F;
                k = i;
                anchorStart = pointer - f * inOut[i].main;
                break;
            }
            if (i + 1 < n && pointer >= e && pointer < baseStarts[i + 1]) {
                // 在书签 i 和 i+1 之间的间隙里：间隙不缩放，鼠标到 i 尾部的距离保持不变。
                const float q = pointer - e;
                k = i;
                anchorStart = pointer - q - inOut[i].main;
                break;
            }
        }
    }

    inOut[k].start = anchorStart;
    for (std::size_t i = k + 1; i < n; ++i) {
        inOut[i].start = inOut[i - 1].start + inOut[i - 1].main + gap;
    }
    for (std::size_t i = k; i-- > 0;) {
        inOut[i].start = inOut[i + 1].start - gap - inOut[i].main;
    }
}

void LayoutDock(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
                const DockParams& params, float pointer, float strength,
                std::vector<DockItemVisual>& out) {
    DockTargetSizes(items, baseStarts, params, pointer, strength, out);
    DockArrange(items, baseStarts, params.gap, pointer, out);
}

int DockPrimary(const std::vector<DockItemVisual>& visual, int current, float hysteresis) {
    int best = -1;
    float bestInfluence = 0.0F;
    for (std::size_t i = 0; i < visual.size(); ++i) {
        if (visual[i].influence > bestInfluence) {
            bestInfluence = visual[i].influence;
            best = static_cast<int>(i);
        }
    }
    if (best < 0) return -1;
    if (current >= 0 && static_cast<std::size_t>(current) < visual.size() && current != best &&
        visual[static_cast<std::size_t>(best)].influence <
            visual[static_cast<std::size_t>(current)].influence + hysteresis) {
        return current;
    }
    return best;
}

float DockMaxGrowth(const std::vector<DockItemBase>& items, const DockParams& params) {
    if (items.empty()) return 0.0F;
    const std::vector<float> starts = DockBaseStarts(items, params.gap, 0.0F);
    const float from = -params.radius;
    const float to = starts.back() + items.back().main + params.radius;

    float best = 0.0F;
    std::vector<DockItemVisual> sizes;
    // 半像素步长：最大值出现在某个书签中心附近，函数光滑，这个精度足够，而且书签个数有限，
    // 这一趟只在布局变化时算一次，不在每帧里。
    for (float p = from; p <= to; p += 0.5F) {
        DockTargetSizes(items, starts, params, p, 1.0F, sizes);
        float growth = 0.0F;
        for (std::size_t i = 0; i < items.size(); ++i) growth += sizes[i].main - items[i].main;
        best = std::max(best, growth);
    }
    return best;
}

float SmoothToward(float current, float target, float dtMs, float tauMs) {
    if (tauMs <= 0.0F) return target;
    if (dtMs <= 0.0F) return current;
    const float next = current + (target - current) * (1.0F - std::exp(-dtMs / tauMs));
    return std::fabs(target - next) < 0.01F ? target : next;
}

} // namespace windowmark
