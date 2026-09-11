#include "WinBorderPlan.h"

#include "windowmark/core/BorderGeometry.h"

#include "WinUtil.h"

#include "windowmark/core/BorderOcclusion.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace windowmark::win {
namespace {

[[nodiscard]] Rect ToCore(const RECT& r) {
    return Rect{r.left, r.top, r.right, r.bottom};
}

[[nodiscard]] HWND HwndOf(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

// 线有多粗。置顶的更粗——「这个窗口被钉在最前面」值得一眼认出来。
[[nodiscard]] int StrokeOf(const BorderModel& model, const Settings& settings) {
    return std::max(1, model.pinned ? settings.pin.width : settings.border.width);
}

// 边框往窗口外伸多远。
[[nodiscard]] int ReachOf(const BorderModel& model, const Settings& settings) {
    return std::max(0, StrokeOf(model, settings) + settings.border.offset);
}

// 边框环：外边界在窗口外 reach，内边界往窗口里进 stroke-reach。
//
// 内侧那一两像素必须盖在窗口自己身上，不能只画在窗口外面。offset 默认 -1 就是为了
// 这个：Windows 会给窗口画一条 1px 的边（实测 Explorer #646765、Chrome #4F5255），
// 边框如果停在窗口外沿，那条灰边就从缝里透出来，看着像边框和窗口之间有 1~2px 的
// 空隙——Windows Terminal 上尤其明显，它那条边颜色更浅。
[[nodiscard]] Rect RingInner(const Rect& frame, int stroke, int reach) {
    const int inset = stroke - reach;   // 等于 -offset
    Rect inner{frame.left + inset, frame.top + inset, frame.right - inset,
               frame.bottom - inset};
    // 窗口小到内边界翻转时退回窗口边界，宁可少盖一像素也不要画出一个空环。
    if (inner.right <= inner.left || inner.bottom <= inner.top) return frame;
    return inner;
}

// Windows 11 的圆角半径，单位是 **DIP** 不是物理像素：普通窗口 8，紧凑窗口 4。
constexpr float kRoundRadiusDip = 8.0F;
constexpr float kRoundSmallRadiusDip = 4.0F;

// 这个窗口所在的缩放比例。
//
// 半径必须跟着缩放走。125% 下窗口的圆角实际是 8 x 1.25 = 10 个物理像素，按 8 画出来
// 的弧比窗口自己的弧方 2px——肉眼看就是「边框没有贴着窗口的角」。
[[nodiscard]] float DpiScaleOf(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;
}

// 这个窗口自己想要多圆。Windows 10 没有圆角，那边这个查询直接失败。
[[nodiscard]] float SystemCornerRadius(HWND hwnd) {
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif
    enum : int { kDefault = 0, kDoNotRound = 1, kRound = 2, kRoundSmall = 3 };

    int preference = kDefault;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                                     sizeof(preference)))) {
        return 0.0F;   // Windows 10，或者这个窗口整个不参与该 API
    }
    const float scale = DpiScaleOf(hwnd);
    switch (preference) {
    case kDoNotRound: return 0.0F;
    case kRoundSmall: return kRoundSmallRadiusDip * scale;
    case kRound:
    case kDefault:
    default:          return kRoundRadiusDip * scale;
    }
}

// 边框该画多圆。
//
// Auto 跟着窗口自己走——Windows 11 的窗口本来就是圆的，边框跟着圆才贴合；想要直角
// 就显式选 Square。Round / RoundSmall 是固定的两档，同样按 DPI 缩放。
//
// Custom 例外：那个数直接就是物理像素，所见即所得。想微调贴合度就用它——先看 Auto
// 画出来多大（125% 缩放下是 10），再往上下试一两个像素。
[[nodiscard]] float CornerRadiusOf(HWND hwnd, const Settings& settings) {
    switch (settings.border.corners) {
    case BorderCorners::Square:     return 0.0F;
    case BorderCorners::Round:      return kRoundRadiusDip * DpiScaleOf(hwnd);
    case BorderCorners::RoundSmall: return kRoundSmallRadiusDip * DpiScaleOf(hwnd);
    case BorderCorners::Custom:     return static_cast<float>(
                                        std::max(0, settings.border.cornerRadius));
    case BorderCorners::Auto:
    default:                        return SystemCornerRadius(hwnd);
    }
}

// 当遮挡物用时，矩形往里咬掉这么多。
//
// 正好齐平的话，边框的最后一像素和被挡窗口的第一像素挨着但不相交，两边各带半像素的
// 抗锯齿过渡，看着就是一条缝。咬进去一像素，接缝被边框自己盖住，视觉上才连得上。
//
// 只作用于「它挡住别人」这一面。给它自己画边框时用的是原矩形——那个要贴着它看得见
// 的边缘，咬进去就该细一圈了。
constexpr int kOccluderBite = 1;

[[nodiscard]] Rect AsOccluder(const RECT& frame) {
    Rect out{frame.left + kOccluderBite, frame.top + kOccluderBite,
             frame.right - kOccluderBite, frame.bottom - kOccluderBite};
    // 窄到翻转的窗口就别咬了，宁可多裁一像素也不要凭空长出一块负数矩形。
    if (out.right <= out.left || out.bottom <= out.top) {
        return Rect{frame.left, frame.top, frame.right, frame.bottom};
    }
    return out;
}

// 窗口中心落在哪块屏上。找不到就返回空——那时不夹，宁可多画一点也不要凭一个错的
// 矩形去裁。
[[nodiscard]] const MonitorArea* ScreenOf(const std::vector<MonitorArea>& monitors,
                                          const RECT& frame) {
    const LONG cx = frame.left + (frame.right - frame.left) / 2;
    const LONG cy = frame.top + (frame.bottom - frame.top) / 2;
    for (const auto& one : monitors) {
        if (cx >= one.bounds.left && cx < one.bounds.right && cy >= one.bounds.top &&
            cy < one.bounds.bottom) {
            return &one;
        }
    }
    return nullptr;
}

// 置顶压过活动状态：「这个窗口被钉在最前面」是更少见、也更值得一眼认出来的状态。
[[nodiscard]] unsigned ColorOf(const BorderModel& model, const Settings& settings,
                               unsigned accent) {
    if (model.pinned) {
        return settings.pin.color == PinSettings::kAccentColor ? accent : settings.pin.color;
    }
    return model.active ? settings.border.activeColor : settings.border.inactiveColor;
}

} // namespace

std::vector<BorderStroke> PlanBorders(const DesktopSnapshot& snapshot,
                                      const std::vector<BorderModel>& models,
                                      const Settings& settings) {
    std::vector<BorderStroke> strokes;
    if (models.empty() || snapshot.windows.empty()) return strokes;

    // 系统主题色只在真有置顶边框时才需要，整帧读一次就够。
    unsigned accent = 0;
    bool accentNeeded = false;
    for (const auto& model : models) {
        if (model.pinned && settings.pin.color == PinSettings::kAccentColor) {
            accentNeeded = true;
            break;
        }
    }
    if (accentNeeded) accent = SystemAccentColor();

    std::unordered_map<HWND, const BorderModel*> wanted;
    wanted.reserve(models.size());
    for (const auto& model : models) wanted.emplace(HwndOf(model.windowId), &model);

    std::vector<Rect> occluders;
    occluders.reserve(snapshot.windows.size());

    // 前台窗口的矩形，单独拎出来。
    //
    // 它无条件遮挡其他所有窗口的边框，**不看快照里的 z 序**。因为 Windows 切换前台时
    // 先发 EVENT_SYSTEM_FOREGROUND、再调整 z 序，我们收到事件立刻取的快照可能还是
    // 旧顺序——实测过一次：MobaXterm 已经在前台，快照里却还排在 Terminal 后面，于是
    // Terminal 整条右边框都没被裁掉（156 个采样点里 95 个本该不画却画了）。
    //
    // GetForegroundWindow() 不参与那个异步过程，它是实时权威的。用它比等 z 序追上来
    // 可靠——而且这不是多扫几遍碰运气，是换了个不会滞后的信息源。
    Rect foregroundRect{};
    bool hasForeground = false;
    for (const auto& entry : snapshot.windows) {
        if (entry.hwnd != snapshot.foreground) continue;
        if (entry.cloaked || entry.minimized) break;
        // 桌面当了前台（用户点了一下桌面空白处）时这条规则不适用：它铺满整个
        // 虚拟桌面却永远在 z 序最底，谁都不挡。拿它的矩形去裁就是一次裁光所有边框。
        if (entry.desktop) break;
        foregroundRect = AsOccluder(entry.frame);
        hasForeground = true;
        break;
    }

    // 从上往下扫：轮到某个窗口时，occluders 里正好是**排在它上面**的所有窗口。
    // 一趟就够，不需要两两比较。
    //
    // 激活窗口不再有「不裁剪」的特权。它本来就在最前，上面没有普通窗口，裁出来自然
    // 是完整的一圈；而当它自己弹出对话框时（Word 的查找替换、Notepad++ 的查找都是
    // 这种 owned window，z 序上位于主窗口之上），那段边框就该让开——开后门的结果是
    // 边框横穿对话框把它切成两半。
    for (const auto& entry : snapshot.windows) {
        const bool paintable = !entry.cloaked && !entry.minimized;

        if (paintable) {
            if (const auto it = wanted.find(entry.hwnd); it != wanted.end()) {
                const BorderModel& model = *it->second;
                // 最大化窗口不画边框，置顶的除外——置顶是显式操作，边框是它生效的
                // 唯一视觉反馈。
                if (!entry.maximized || model.pinned) {
                    const Rect frame = ToCore(entry.frame);
                    const int stroke = StrokeOf(model, settings);
                    const int reach = ReachOf(model, settings);
                    const Rect outer{frame.left - reach, frame.top - reach,
                                     frame.right + reach, frame.bottom + reach};
                    const Rect inner = RingInner(frame, stroke, reach);
                    const unsigned color = ColorOf(model, settings, accent);

                    // 圆角走另一套线宽和位置：比直边宽 cornerWidthExtra，路径再往
                    // 窗口中心挪 cornerInset。
                    //
                    // 路径先内收半个线宽，弧的外沿因此正好落在环外沿上，加宽出来的
                    // 部分全长在窗口内侧——直边是像素填充、边界锐利，圆角是抗锯齿的
                    // 弧，两者只有外沿对齐才看不出接缝。
                    //
                    // 半径跟着路径走：弧要和窗口自己的圆角同心，圆心不动，所以路径
                    // 往里收多少，半径就减多少。
                    const float windowRadius = CornerRadiusOf(entry.hwnd, settings);
                    RoundedRing ring;
                    float radius = 0.0F;
                    if (windowRadius > 0.0F) {
                        ring = RoundedRingOf(stroke, settings.border.cornerWidthExtra,
                                             settings.border.cornerInset);
                        // 半径跟着路径走：弧要和窗口自己的圆角同心，圆心不动，所以
                        // 路径往里收多少半径就减多少（往外长则加）。
                        radius = std::max(0.0F, windowRadius + static_cast<float>(reach) -
                                                    ring.inset);
                    }
                    const float roundWidth = ring.width;
                    const float roundInset = ring.inset;

                    // 裁剪单元：直角用四条边（互不重叠，填满即可）；圆角用整个
                    // 外矩形一块——角上的弧跨越相邻两条边，按四条边裁会把角削平，
                    // 内沿在角上会突然被切成直线。
                    //
                    // cornerInset 为负时弧往外顶出环外沿，单元跟着放大同样多，否则
                    // 顶出去的那部分会被自己的裁剪框切掉，调了等于没调。
                    const int grow = ring.grow;
                    const Rect paintOuter{outer.left - grow, outer.top - grow,
                                          outer.right + grow, outer.bottom + grow};
                    std::vector<Rect> units =
                        roundWidth > 0.0F ? std::vector<Rect>{paintOuter}
                                          : BorderRingSegments(outer, inner);

                    // 边框不该越过屏幕边界。窗口贴着工作区底边时，往外那两像素正好
                    // 落在任务栏上——画布覆盖整块监视器，那里是有地方落笔的（跨屏那种
                    // 越界画布边界自己就挡住了，这种挡不住）。
                    //
                    // 夹的是**可见范围**，不是 outer 本身：圆角的路径以 outer 为基准
                    // 算半径和位置，改了 outer 弧就错位。outer 不动，弧还是原来那条，
                    // 只是压出去的那截不画。
                    //
                    // 夹的对象是 paintOuter 而不是 outer，判据也用 paintReach：圆角环
                    // 可以长到 outer 之外（加宽往外长、cornerInset 为负），传 outer 进去
                    // 等于把长出去的那一截又裁回来，加宽白加。判据同理——窗口离任务栏
                    // 4px 时按 reach=2 算「没贴上」，可边框实际往外画 5px，照样压上去。
                    const int paintReach = reach + grow;
                    if (const MonitorArea* screen = ScreenOf(snapshot.monitors, entry.frame);
                        screen != nullptr) {
                        const Rect limit = ClampBorderToScreen(
                            frame, paintOuter, ToCore(screen->bounds),
                            ToCore(screen->work), paintReach);
                        if (limit.right > limit.left && limit.bottom > limit.top) {
                            units = ClipToBounds(units, limit);
                        }
                    }
                    std::vector<Rect> visible;
                    if (entry.hwnd == snapshot.foreground) {
                        // 前台窗口：只有 topmost 窗口、用户点名「视为置顶」的窗口，
                        // 以及它**自己的** owned 对话框能盖住它。普通窗口排在它前面是
                        // 不可能的——它是前台，这是定义。
                        //
                        // 不能照 occluders 来算：快照里的 z 序可能还没跟上（Windows
                        // 先发 FOREGROUND 事件、再调整 z 序），那样会把前台自己的边框
                        // 裁掉一块。实测 MobaXterm 激活后，它和 Terminal 交界处的那段
                        // 激活边框整条消失，就是这么来的。
                        std::vector<Rect> fgOccluders;
                        for (const auto& other : snapshot.windows) {
                            if (other.hwnd == entry.hwnd) break;   // 只看排在它前面的
                            if (other.cloaked || other.minimized) continue;
                            if (other.topmost || other.treatAsTopmost ||
                                other.owner == entry.hwnd) {
                                fgOccluders.push_back(AsOccluder(other.frame));
                            }
                        }
                        visible = ClipSegments(units, fgOccluders);
                    } else {
                        visible = ClipSegments(units, occluders);
                        // 其余窗口一律再减一次前台矩形——理由同上，只是方向相反：
                        // 前台一定在最前，它盖住的地方不该有别人的边框。
                        if (hasForeground) {
                            visible = ClipSegments(visible, {foregroundRect});
                        }
                    }
                    for (const Rect& seg : visible) {
                        strokes.push_back(BorderStroke{seg, color, outer, stroke, radius,
                                                       roundWidth, roundInset});
                    }
                }
            }
        }

        // 不管这个窗口有没有边框，它都会挡住排在它下面的窗口。
        if (paintable) occluders.push_back(AsOccluder(entry.frame));
    }
    return strokes;
}

} // namespace windowmark::win
