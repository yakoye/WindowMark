#pragma once

#include <vector>

namespace windowmark {

// 磁性书签栏的连续磁场。
//
// 鼠标是一块磁铁，书签是一排铁块：每个书签按它离鼠标的距离受到连续的「磁力」，最近的达到固定
// 峰值，周围的平滑衰减，并被连续地挤开。设计见 docs/superpowers/specs/2026-09-17-磁性书签栏-design.md。
//
// 这里只有纯函数，不认识窗口、不认识方向。
//
// 两个坐标轴：
//   main   沿书签栏方向（Top/Bottom 是 X，Left/Right 是 Y）
//   cross  从窗口边缘伸向窗口内容的方向
//
// 两套位置，严格分开：
//   base    书签静止时的位置和尺寸。只由设置和个数决定，只用来算距离——任何动画都不改它
//   visual  这一帧画出来的样子。由 base + 鼠标算出来，**从不回头参与下一次计算**
// 鼠标永远只和 base 比距离。拿 visual 去算距离会形成正反馈：标签变大 → 离鼠标更近 → 再变大。

// 一个书签在静止时的尺寸。
struct DockItemBase {
    float main{};
    float cross{};
};

struct DockParams {
    float gap{};
    // 峰值是**绝对像素**，不是倍率。激活标签的 base 比别人大，被吸到峰值时却和别人一样大——
    // 否则鼠标从普通标签滑到激活标签，峰值会跟着跳。
    float peakMain{};
    float peakCross{};
    // 磁场半径，主轴方向、base 像素。距离达到半径的书签严格不受影响。
    float radius{};
};

// 一个书签在当前这一帧的样子。
struct DockItemVisual {
    float influence{};   // 0..1，已经乘过磁场强度
    float main{};
    float cross{};
    float start{};       // 主轴起点，和 base 在同一个坐标系里
};

// 磁力随距离的衰减：余弦。
//
//   I(d) = d < R ? 0.5 * (1 + cos(pi * d / R)) : 0
//
// 中心为 1，在 R 处平滑归零，两端导数都连续——中心不是尖顶，边缘也不是折线。不用高斯：高斯
// 永远不到 0，远处的书签会永远带着一点点放大；余弦在 R 以外严格为 0。
[[nodiscard]] float MagnetInfluence(float distance, float radius);

// base 排布：第一个书签从 origin 开始，书签之间隔 gap。返回每个书签的主轴起点。
[[nodiscard]] std::vector<float> DockBaseStarts(const std::vector<DockItemBase>& items, float gap,
                                                float origin);

// 鼠标在 base 空间里落在哪个书签上。落在间隙里、或者栏外，返回 -1。
//
// 点击判定用它就够了：visual 排布以鼠标为不动点（见 DockArrange），鼠标所在的 base 书签和它
// 眼睛看到的那个书签始终是同一个。
[[nodiscard]] int DockItemAt(const std::vector<DockItemBase>& items,
                             const std::vector<float>& baseStarts, float pointer);

// 每个书签的目标尺寸和 influence。strength 是整个磁场的强度（0..1），进出书签栏时平滑变化。
// out 会被改成和 items 一样长；只写 influence、main、cross。
void DockTargetSizes(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
                     const DockParams& params, float pointer, float strength,
                     std::vector<DockItemVisual>& out);

// 主轴排布：以鼠标为不动点，把 inOut 里已经给定的尺寸（可能是平滑过的）从鼠标处向两侧排开。
// 只写 start。
//
// 鼠标在书签 k 内部、占它 base 长度的 f 处：visual 里 k 的起点放在 pointer - f * main_k，
// 于是鼠标下面那个点还在鼠标下面，书签不会「跑开」让鼠标去追。鼠标在间隙里时，间隙不缩放，
// 按它在间隙中的偏移排。鼠标跨过书签边界时两种算法给出同一个位置，所以排布对鼠标位置连续。
void DockArrange(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
                 float gap, float pointer, std::vector<DockItemVisual>& inOut);

// DockTargetSizes + DockArrange，一次算完一帧。
void LayoutDock(const std::vector<DockItemBase>& items, const std::vector<float>& baseStarts,
                const DockParams& params, float pointer, float strength,
                std::vector<DockItemVisual>& out);

// 主标签：influence 最大的那个，它的标题和缩略图会显示出来。
//
// current 是上一帧的主标签。新候选必须比它高出 hysteresis 才切换——鼠标停在两个书签正中间小幅
// 抖动时，两边 influence 几乎相等，没有这一条标题和缩略图会来回闪。没有任何书签受影响时返回 -1。
[[nodiscard]] int DockPrimary(const std::vector<DockItemVisual>& visual, int current,
                              float hysteresis);

// 鼠标放在任何位置时，所有书签主轴增长之和的最大值。
//
// 书签条窗口要在 base 两侧各留这么多余量，挤开的书签才不会被窗口边缘裁掉。按鼠标位置逐像素
// 扫一遍取最大，不是估计。
[[nodiscard]] float DockMaxGrowth(const std::vector<DockItemBase>& items, const DockParams& params);

// 指数逼近：经过 dtMs 毫秒，current 朝 target 走。tauMs 是时间常数；<= 0 直接到达。
// 离得足够近时直接落到 target，好让调用方判断「已经收敛，可以停掉定时器」。
[[nodiscard]] float SmoothToward(float current, float target, float dtMs, float tauMs);

} // namespace windowmark
