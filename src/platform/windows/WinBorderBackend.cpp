#include "WinBorderBackend.h"

#include "WinUtil.h"
#include "PinDiag.h"

#include "windowmark/core/BorderGeometry.h"

#include <cstdio>

#include "AppIdentity.h"

#include <d2d1.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace windowmark::win {
namespace {

constexpr const wchar_t* kBorderClass = app::kBorderWindowClass;

// ResyncIfDrifted 里两条 z 序遍历的上限。桌面上挂着数百个隐藏的顶级窗口（实测
// 单个 Excel 窗口上方就压着 144 个），遍历要把它们跳完才能碰到第一个**可见**的。
// 这个上限只是防止病态 z 序把 UI 线程转死。
//
// 只剩这一个数了。原来还有个 kZOrderAttemptLimit，允许插入失败后继续往上爬 16 次，
// 而能走到插入那一步的都是可见窗口，每爬一格就越过一个本该盖住边框的窗口。那
// 整套「把边框插到目标正上方」的机制现在已经删干净了，见 SyncZOrder。
constexpr int kZOrderStepLimit = 4096;

// 前台钩子的回调是静态的，没有 this 可用。守望窗口全进程只有一个（WinBorderBackend
// 是单实例后端），所以把它的句柄放在这里给回调取。
HWND g_watchWindow = nullptr;

// 注意这里**没有** SWP_NOSENDCHANGING。
//
// 带上它，z 序调整会变成一次「假成功」：SetWindowPos 返回 TRUE，topmost 位和 z 序
// 立刻都变了（实测 z 从 106 跳到 12），但两百毫秒内又退回原处，而且 GetLastError
// 是 0——从返回值和错误码完全看不出问题。11 个边框全试一遍：立刻生效 10/11，
// 300ms 后 0/11。去掉它之后同一次调用稳定保持。
//
// 原因是 WM_WINDOWPOSCHANGING 属于 z 序变更的提交路径，跳过它等于只改了状态没有
// 落实。这个 flag 当初是为省掉拖动时每帧的消息往返而加的——对**移动**是合理优化
// （Reposition 里仍然保留），对**改 z 序**是错的。
constexpr UINT kZOrderFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

// 把窗口放进 topmost 层，并确认真的放进去了。
//
// 少数窗口会卡住：`SetWindowPos(HWND_TOPMOST)` 返回 TRUE、`GetLastError()` 为 0，
// topmost 位却纹丝不动，而同一时刻同一进程的其他边框窗口做同样的调用都成功——两者
// 的 ex/style/owner/parent/线程/cloaked 属性逐项相同，从外面看不出任何差别。
//
// 实测唯一稳定有效的办法是补一次带 `SWP_FRAMECHANGED` 的调用：它强制 Windows 重算
// 窗口框架，把状态真正落实下去。其余办法都只是「立刻看起来对了，一百毫秒后又退回
// 去」——经 HWND_TOP 中转、经 HWND_BOTTOM 中转、直接写 `WS_EX_TOPMOST` 位，全都如此。
// 这和 kZOrderFlags 不能带 SWP_NOSENDCHANGING 是同一类问题：跳过消息会让状态改了
// 但没提交。
//
// 只在第一次失败后才付这个代价，正常路径不受影响。
void ForceTopmost(HWND hwnd, HWND target, UINT flags) {
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) return;

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_FRAMECHANGED);
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) return;

    // 连 FRAMECHANGED 也进不去。这时不再和 topmost 层较劲——反正目的只是「边框盖在
    // 目标上面」，普通层里同样能做到：插到目标**正上方**那个可见窗口之后。
    //
    // 这一步只越过目标自己，不越过任何别的窗口，所以不会重蹈「边框画到别人身上」的
    // 覆辙——当初那个灾难是失败后**连续**往上爬，每爬一格都越过一个本该盖住边框的
    // 窗口。这里取一格就停，而那一格之上的窗口本来就该压在边框上面。
    //
    // 代价：会被别的 topmost 窗口（菜单、悬浮窗）盖住——但那本来就是期望行为。
    HWND above = target;
    for (int step = 0; step < kZOrderStepLimit; ++step) {
        above = GetWindow(above, GW_HWNDPREV);
        if (!above) return;                     // 目标已在最前，无处可插
        if (!IsWindowVisible(above)) continue;  // 隐藏窗口不画像素，跳过
        break;
    }
    if (above) SetWindowPos(hwnd, above, 0, 0, 0, 0, flags);
}

// 边框和目标是不是紧挨着——中间只允许夹不可见窗口，不论谁上谁下。
[[nodiscard]] bool IsAdjacentTo(HWND border, HWND target) {
    for (const UINT dir : {GW_HWNDPREV, GW_HWNDNEXT}) {
        HWND probe = border;
        for (int step = 0; step < kZOrderStepLimit; ++step) {
            probe = GetWindow(probe, dir);
            if (!probe) break;
            if (probe == target) return true;
            if (!IsWindowVisible(probe)) continue;
            break;
        }
    }
    return false;
}

// topmost 层里最靠后的那个可见窗口。边框插到它后面就落在 topmost 层的末尾——
// 在所有普通窗口之上，在所有 topmost 窗口之下。
//
// Windows 只有普通层和 topmost 层两个 band，没有可以插空的数值层级，而这个位置是
// 两个 band 的交界，也就是「比谁都高，但比系统 UI 低」唯一能表达的地方：右键菜单、
// 输入法候选框、任务栏、悬浮的会议小窗、别人置顶的窗口全都留在上面。
//
// 返回 nullptr 表示当前没有别的 topmost 窗口，那时 HWND_TOPMOST 就是同一个位置。
[[nodiscard]] HWND LastTopmostWindow(HWND self) {
    HWND last = nullptr;
    HWND h = GetTopWindow(nullptr);
    for (int step = 0; step < kZOrderStepLimit && h; ++step) {
        if (h != self && IsWindowVisible(h)) {
            if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) break;
            last = h;
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }
    return last;
}

// 目标自己那些浮在它上面的对话框中，最靠下的一个（Word 的查找替换、Notepad++ 的
// 查找都是这种 owned window）。一个都没有就返回 nullptr。
//
// 只往上走**目标自己的** owned 窗口，碰到第一个不属于它的立刻停——这一条是全部的
// 安全性所在。当初「插入失败后往上再爬一格」之所以是灾难，正因为它越过的是任意
// 窗口；这里越过的只有目标自己的对话框，而边框本来就该排在它们下面。
[[nodiscard]] HWND LowestOwnedDialog(HWND target) {
    RECT targetRect{};
    if (!GetWindowRect(target, &targetRect)) return nullptr;

    HWND probe = target;
    for (int step = 0; step < kZOrderStepLimit; ++step) {
        probe = GetWindow(probe, GW_HWNDPREV);
        if (!probe) break;
        if (!IsWindowVisible(probe)) continue;
        if (GetWindow(probe, GW_OWNER) != target) break;

        // 只有真会挡住边框的才算数：有实际尺寸，而且和目标窗口有重叠。
        //
        // `IsWindowVisible` 远不足以判断「这是个对话框」。Windows Terminal 挂着一个
        // 0x0 的 PseudoConsoleWindow，可见标志是 true；Electron 应用也常带一堆可见的
        // 辅助窗口。把它们当对话框，边框就被无谓地降进普通层，然后被任何一个普通
        // 窗口盖住——现象是「切换应用后边框线不全」。
        // IntersectRect 对空矩形返回 FALSE，零尺寸的因此自动出局。
        RECT probeRect{};
        RECT overlap{};
        if (!GetWindowRect(probe, &probeRect)) continue;
        if (!IntersectRect(&overlap, &probeRect, &targetRect)) continue;

        // 第一个通过的就是最靠下的那个，正是要找的锚点：边框插到它之后，就落在所有
        // 对话框之下、目标之上。继续往上找会取到更上面的对话框，那样边框会夹在两个
        // 对话框中间，把下面那个盖住。
        return probe;
    }
    return nullptr;
}

// The system accent colour, as 0xAARRGGBB.
//
// Read on demand rather than cached: this is only reached when a pinned outline actually
// repaints, which happens on pin, unpin, resize and activation - never per frame, because
// a move does not repaint. A registry query at that rate is free, and reading it fresh is
// what makes the highlight follow a theme change without any plumbing to notice one.
// Every window that ever gets an outline passes through Apply's creation branch, so one
// line written there is a *complete* record. An external poller cannot match that: at one
// sample per ~170ms it can miss a popup that only lives 0.2s, and then "not captured"
// looks exactly like "never bordered".
[[nodiscard]] std::wstring DescribeBorderedWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return L"(窗口已消失)";

    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    wchar_t title[96]{};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));

    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                     sizeof(frame)))) {
        GetWindowRect(hwnd, &frame);
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring exe = QueryProcessPath(pid);
    if (const auto slash = exe.find_last_of(L"/\\"); slash != std::wstring::npos) {
        exe = exe.substr(slash + 1);
    }
    if (exe.empty()) exe = L"?";

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    wchar_t buffer[640]{};
    swprintf_s(buffer,
               L"%s [%s] %ldx%ld @(%ld,%ld) pid=%lu style=0x%08lX ex=0x%08lX%s%s%s%s%s "
               L"标题「%s」",
               exe.c_str(), cls,
               frame.right - frame.left, frame.bottom - frame.top, frame.left, frame.top,
               pid,
               static_cast<unsigned long>(style), static_cast<unsigned long>(exStyle),
               (style & WS_CAPTION) == WS_CAPTION ? L" CAPTION" : L"",
               (style & WS_THICKFRAME) != 0 ? L" THICKFRAME" : L"",
               (style & WS_POPUP) != 0 ? L" POPUP" : L"",
               (exStyle & WS_EX_NOACTIVATE) != 0 ? L" NOACTIVATE" : L"",
               IsIconic(hwnd) ? L" 最小化" : L"",
               title);
    return buffer;
}

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

// Colours carry their own alpha as 0xAARRGGBB, matching how tacky-borders expresses them.
D2D1_COLOR_F ToD2DColor(unsigned argb) {
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0F;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0F;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0F;
    const float b = static_cast<float>(argb & 0xFF) / 255.0F;
    return D2D1::ColorF(r, g, b, a);
}

// The radius Windows 11 uses for a normal window, and for compact ones.
constexpr float kRoundRadius = 8.0F;
constexpr float kRoundSmallRadius = 4.0F;

// Windows 11 rounds windows, and by how much depends on the window's own preference.
// Windows 10 has no rounding at all, where the query simply fails.
float SystemCornerRadius(HWND hwnd) {
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif
    enum : int { kDefault = 0, kDoNotRound = 1, kRound = 2, kRoundSmall = 3 };

    int preference = kDefault;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                     &preference, sizeof(preference)))) {
        return 0.0F;  // Windows 10, or the window opted out of the API entirely.
    }
    switch (preference) {
    case kDoNotRound: return 0.0F;
    case kRoundSmall: return kRoundSmallRadius;
    case kRound:
    case kDefault:
    default:         return kRoundRadius;
    }
}

} // namespace

// Backing store for UpdateLayeredWindow: a top-down 32bpp DIB rendered with premultiplied
// alpha, which is what ULW_ALPHA expects.
class WinBorderBackend::LayeredSurface {
public:
    ~LayeredSurface() { Reset(); }

    // Grows to fit and never shrinks: this is shared scratch, so a bigger window simply
    // raises the high-water mark instead of forcing a reallocation on every switch.
    bool EnsureAtLeast(int width, int height) {
        if (dc_ && width <= width_ && height <= height_) return true;
        return Ensure(std::max(width, width_), std::max(height, height_));
    }

    bool Ensure(int width, int height) {
        if (dc_ && width == width_ && height == height_) return true;
        Reset();
        if (width <= 0 || height <= 0) return false;

        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!dc_) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap_) {
            Reset();
            return false;
        }
        previous_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));
        width_ = width;
        height_ = height;
        return true;
    }

    void Reset() {
        if (dc_) {
            if (previous_) SelectObject(dc_, previous_);
            DeleteDC(dc_);
        }
        if (bitmap_) DeleteObject(bitmap_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        previous_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

    [[nodiscard]] HDC dc() const noexcept { return dc_; }

private:
    HDC dc_{};
    HBITMAP bitmap_{};
    HBITMAP previous_{};
    int width_{};
    int height_{};
};

// One window per outline, presented with UpdateLayeredWindow.
//
// An earlier version split each outline into four thin strips so the bitmap would scale
// with the perimeter instead of the area. That fixed memory but cost latency: a drag has
// to reposition four windows instead of one, and measured against tacky-borders it landed
// at 50/80 perfectly-tracked frames versus its 80/80.
//
// Going back to a single window makes a move one SetWindowPos again, and the memory that
// motivated the split is solved differently: the bitmap is shared process-wide rather
// than owned per outline. UpdateLayeredWindow copies what it is handed, so one scratch
// bitmap the size of the largest window serves every border in turn.
class WinBorderBackend::BorderWindow {
public:
    BorderWindow(WinBorderBackend& owner, const BorderModel& model,
                 bool alwaysTopmost = false)
        : owner_(owner), model_(model), alwaysTopmost_(alwaysTopmost) {}

    ~BorderWindow() { Destroy(); }

    bool Create() {
        const RECT outer = OuterRect();
        hwnd_ = CreateWindowExW(
            // TRANSPARENT is essential: the outline straddles the window edge, and
            // without it every click near an edge would land on us, not the app.
            //
            // 常驻 topmost 的那个在这里就带上 WS_EX_TOPMOST。**创建时就在 topmost 层**
            // 和「事后用 SetWindowPos 提上去」是两回事：后者正是会卡死的那个操作，
            // 实测存在这样的窗口——SetWindowPos 返回 TRUE、GetLastError 为 0、z 序和
            // topmost 位纹丝不动，换锚点、加 SWP_FRAMECHANGED、从别的进程调，全都推
            // 不动，而同一线程的其他边框窗口做同样的调用都正常。绕开它最可靠的办法
            // 就是从一开始就不需要提升。
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                (alwaysTopmost_ ? WS_EX_TOPMOST : 0U),
            // WS_DISABLED on top of WS_EX_TRANSPARENT: the outline must never take
            // input, and a disabled window is skipped by hit-testing outright.
            kBorderClass, L"", WS_POPUP | WS_DISABLED,
            outer.left, outer.top,
            std::max<LONG>(1, outer.right - outer.left),
            std::max<LONG>(1, outer.bottom - outer.top),
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) return false;

        outer_ = outer;
        ApplyVisibility();
        Redraw();
        if (!alwaysTopmost_) SyncZOrder();
        return true;
    }

    void Destroy() noexcept {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void Update(const BorderModel& model) {
        const bool sizeChanged = model.frame.width() != model_.frame.width() ||
                                 model.frame.height() != model_.frame.height();
        const bool activeChanged = model.active != model_.active;
        // A pin changes both the colour and the line width, and the width feeds Reach(),
        // so the outline has to be re-laid-out as well as repainted.
        const bool pinnedChanged = model.pinned != model_.pinned;
        const bool visibleChanged = model.visible != model_.visible;
        model_ = model;

        Reposition();
        if (visibleChanged) ApplyVisibility();

        // z 序在重绘**之前**。两件事互不依赖，而 Redraw 要做一次 UpdateLayeredWindow，
        // 把整个边框窗口的位图交给 DWM——1500x900 的边框就是 5.4MB。把 z 序排在它后面
        // 等着，「边框出现」的延迟里就白白含进了一次重绘；而视觉上「出现」等的正是
        // z 序。倒过来之后边框先站到位（暂时还带着上一次的颜色），一帧之内再上色。
        //
        // 每次都检查，但只有真漂了才动手。这里曾经无条件调 SyncZOrder()，而 Apply 会
        // 遍历所有边框——于是每刷新一次，屏幕上每个边框都做一次 SetWindowPos，后一个
        // 的插入打乱前一个刚排好的位置，窗口越多越乱。也不能改回「只在 activeChanged
        // 时调」，那样 z 序会从看不见的事件漂走且再也回不来。
        if (alwaysTopmost_) {
            // 层内挪到末尾：比所有普通窗口高，比所有 topmost 窗口低，这样右键菜单、
            // 输入法候选框、任务栏、悬浮小窗都能压在边框上面。
            //
            // 这一步是安全的，和那个会卡死的操作不是一回事：窗口已经在 topmost 层，
            // 插到另一个 topmost 窗口后面只是**层内换位**；卡死的是「从普通层提进
            // topmost 层」。真挪不动也无所谓——位置照样是对的，顶多盖住点系统 UI。
            if (HWND lastTop = LastTopmostWindow(hwnd_); lastTop) {
                SetWindowPos(hwnd_, lastTop, 0, 0, 0, 0, kZOrderFlags);
            }
        } else {
            ResyncIfDrifted();
        }

        // 只有填充色和位图尺寸会改变像素，纯移动不会。
        if (sizeChanged || activeChanged || pinnedChanged ||
            (visibleChanged && model_.visible)) {
            Redraw();
        }
    }

    // 边框的 z 序只有两条规则，都不需要指定锚点窗口：
    //   活动窗口的边框 -> 放到最顶端；
    //   其余边框       -> 贴在自己目标的正下方。
    //
    // 一个目标窗口只有一个边框窗口（四条边画在同一个 layered 窗口上），所以这里排的
    // 是「这个边框」的位置，不存在给四条边分别排序这回事。
    // 边框的 z 序规则，全部在这里。一个目标窗口只有一个边框窗口（四条边画在同一个
    // layered 窗口上），所以排的是「这个边框」的位置，没有给四条边分别排序这回事。
    //
    // 三条规则，为什么是这样见 docs/Windows开发避坑规则.md 里「给别人的窗口加装饰」
    // 一节——那里记着每一条对应的实测教训，代码里不再重复。
    void SyncZOrder() {
        if (!hwnd_ || !shown_) return;
        HWND target = HwndFromId(model_.windowId);
        if (!IsWindow(target)) return;

        // 判据必须是系统认定的前台窗口，不能只看 model_.active。后者是 UI 层的活动
        // 标记，焦点切换途中两者会分家，那时照样提 topmost 就会在别的窗口上留下一条
        // 与谁都不搭界的孤立线。active 决定颜色和线宽，前台身份决定能不能上浮。
        if (model_.active && GetForegroundWindow() == target) {
            // 目标自己的对话框浮在它上面时，边框要排在这些对话框**之下**、目标之上。
            // 否则 topmost 层会把它们整个盖穿——Word 的查找替换、Notepad++ 的查找都
            // 是普通层窗口，再靠前也压不过 topmost。
            if (HWND dialog = LowestOwnedDialog(target);
                dialog && SetWindowPos(hwnd_, dialog, 0, 0, 0, 0, kZOrderFlags)) {
                return;
            }
            // 目标自己被置顶了：那时它就该在最上面，边框跟上去。
            if ((GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                ForceTopmost(hwnd_, target, kZOrderFlags);
                return;
            }

            // 常规情况：落到 topmost 层的**末尾**，不是最前。
            //
            // 末尾这个位置刚好是两个 band 的交界：比所有普通窗口高，比所有 topmost
            // 窗口低。右键菜单、输入法候选框、任务栏、悬浮的会议小窗、别人置顶的窗口
            // 因而都能压在边框上面，而边框仍然盖得住自己目标的四条边。
            //
            // 这比「提到最前然后不去抢」稳当：那种做法只对短暂弹出的东西有效，遇到
            // 长期悬浮的窗口时，下一次焦点切换又会把边框提到它上面去。
            // 两步，顺序不能反：先**进** topmost 层，再在层内挪到末尾。
            //
            // 反过来（先锚定到最后一个 topmost 窗口）有个窗口期：`LastTopmostWindow`
            // 找到它、到真正调用 SetWindowPos 之间，那个窗口可能已经掉出 topmost 层
            // （悬浮窗关闭、别人取消置顶都会），于是边框跟着落进普通层——而实测这时
            // 紧随其后的 HWND_TOPMOST 救不回来。现象就是边框位置分毫不差地贴在目标
            // 上方，却在普通层里被任何一个普通窗口盖住。
            //
            // 先提上去就没有这个窗口期：HWND_TOPMOST 不引用任何窗口，必定成功；之后
            // 插到另一个 topmost 窗口后面只是层内换位，不会掉出去。
            ForceTopmost(hwnd_, target, kZOrderFlags);
            NoteZOrderResult((GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
            if (HWND lastTop = LastTopmostWindow(hwnd_); lastTop) {
                SetWindowPos(hwnd_, lastTop, 0, 0, 0, 0, kZOrderFlags);
                // 万一锚点正好在这一瞬掉出了 topmost 层，把位补回来。宁可停在层内
                // 最前（可能盖住悬浮窗），也不能掉回普通层（边框整个被盖住）。
                if ((GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
                    ForceTopmost(hwnd_, target, kZOrderFlags);
                }
            }
            return;
        }

        const bool targetTopmost =
            (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

        // 其余窗口：优先紧贴在目标**上方**。
        //
        // 那是最准的位置——边框夹在目标和压着目标的那个窗口之间，目标被谁盖住边框就被
        // 谁盖住，一模一样地跟随。贴在下方是次一等的选择：先丢掉与窗框重叠的那 1px，
        // 更糟的是目标正下方插不进去时（任务管理器就是，UIPI 拒绝）只能再往下退一格，
        // 而那一格可能是个**与目标重叠**的窗口，边框于是被它整片盖住——实测任务管理器
        // 的边框就这样掉到了一个和它重叠的 chrome 窗口下面。
        //
        // 锚点是目标**上面**那个窗口而不是目标自己，所以不受目标进程完整性的限制。
        // 只取一格就停：那一格之上的窗口本来就该压在边框上面。当初「插入失败后连续
        // 往上爬」之所以是灾难，正在于爬第二格、第三格。
        HWND above = target;
        for (int step = 0; step < kZOrderStepLimit; ++step) {
            above = GetWindow(above, GW_HWNDPREV);
            if (!above) break;
            if (!IsWindowVisible(above)) continue;
            break;
        }
        // 不能锚定到 topmost 窗口——插到它后面会把边框一起提进 topmost 层，于是浮在
        // 所有普通窗口上面。目标自己就是置顶窗口时不在此列。
        if (above && (targetTopmost ||
                      (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)) {
            SetWindowPos(hwnd_, above, 0, 0, 0, 0, kZOrderFlags);
            if (IsAdjacentTo(hwnd_, target)) return;
        }

        // 退而求其次：贴在目标正下方。插到一个非 topmost 窗口后面时 Windows 会顺手
        // 摘掉 topmost 位，所以这一次调用既降层又定位，中间没有空档。
        SetWindowPos(hwnd_, target, 0, 0, 0, 0, kZOrderFlags);
        if (IsAdjacentTo(hwnd_, target)) return;

        // 都被拒了。UIPI 不区分方向：往高完整性窗口下面插同样不行。这时必须先把
        // topmost 位摘掉，否则边框卡在普通层最前，浮在新前台窗口上面——那就是「窗口
        // 都失去焦点了，边框线还留着」。
        if (!targetTopmost &&
            (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, kZOrderFlags);
        }

        // 退一格：锚定到目标**下面**那个可见窗口。往下找是安全的——往上每爬一格都越过
        // 一个本该盖住边框的窗口，往下每退一格只是被更多窗口遮住。
        HWND below = GetWindow(target, GW_HWNDNEXT);
        for (int step = 0; step < kZOrderStepLimit && below; ++step) {
            if (IsWindowVisible(below)) break;
            below = GetWindow(below, GW_HWNDNEXT);
        }
        if (below && SetWindowPos(hwnd_, below, 0, 0, 0, 0, kZOrderFlags)) return;

        // 还不行就沉底。边框会完全看不见，但那远好过浮在别人窗口上面：看不见只是自己
        // 缺了一块，浮上去是把别人的画面也弄坏。
        SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0, kZOrderFlags);
    }

    // 这个边框窗口的 z 序是不是已经完全推不动了。
    //
    // 实测存在这样的状态：`SetWindowPos` 返回 TRUE、`GetLastError()` 为 0，窗口的
    // z 序和 topmost 位却纹丝不动——换 HWND_TOPMOST、换具体锚点、加 SWP_FRAMECHANGED、
    // 从别的进程调，全都一样，而同一进程同一线程的其他边框窗口做同样的调用都正常。
    // 两者的 ex/style/owner/parent/线程/cloaked/showCmd 属性逐项相同，从外面看不出
    // 任何差别。窗口自己进了坏状态，调用方改不动它。
    //
    // 唯一有效的办法是把它扔掉重建。
    //
    // 一次失败就判定，不再等几轮确认：ForceTopmost 内部本身已经是三层（HWND_TOPMOST
    // → 加 SWP_FRAMECHANGED → 插到目标正上方那个窗口之后），三层全部落空就已经是确凿
    // 的冻结，多等只是让使用者多看几百毫秒的残缺边框。
    [[nodiscard]] bool IsStuck() const { return zOrderFailures_ >= kStuckThreshold; }

    [[nodiscard]] const BorderModel& Model() const { return model_; }

    void Hide() {
        model_.visible = false;
        ApplyVisibility();
    }

    void NoteZOrderResult(bool ok) {
        if (ok) {
            zOrderFailures_ = 0;
            return;
        }
        if (zOrderFailures_ < kStuckThreshold) ++zOrderFailures_;
        // 当场投递重建请求，不等 500ms 轮询——那半秒是肉眼看得见的残缺边框。
        // 只能投消息，不能就地销毁：这是这个对象自己的方法，销毁自己会留下悬垂指针。
        if (IsStuck()) owner_.RequestStuckRecheck();
    }

    // 轮询用：先只读地判断边框还在不在该在的位置，漂了才动手。
    //
    // 存在的理由是实测出来的：**纯 z 序变化不产生任何 WinEvent**。把窗口置顶、取消
    // 置顶、压到最底，逐一试过 FOREGROUND / REORDER / LOCATIONCHANGE / SHOW / HIDE
    // 等全部事件，目标窗口一条都不发。REORDER 更是连顶级窗口都不报（20 秒 1 次，
    // 还是桌面子窗口发的）。所以「来回切换焦点后边框错位」这类问题，除了轮询没有
    // 别的办法——tacky-borders 的作者也在源码注释里写了同样的结论。
    //
    // 代价压到最低：位置没漂时只有几次 GetWindow，一次 SetWindowPos 都不做。绝大
    // 多数轮询都会在这里返回。
    void ResyncIfDrifted() {
        if (!hwnd_ || !shown_) return;
        HWND target = HwndFromId(model_.windowId);
        if (!IsWindow(target)) return;

        const bool wantTop = model_.active && GetForegroundWindow() == target;
        const bool isTopmost =
            (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

        // 目标不再是前台，边框却还留在 topmost 层——这就是那条孤立浮线的状态，
        // 必须马上降下来。轮询存在的意义有一半在这里：焦点变化会发事件，但
        // 「前台换人导致某个**别的**窗口不再是前台」不会通知到那个窗口。
        if (!wantTop) {
            if (isTopmost) {
                SyncZOrder();
                return;
            }
        } else if (HWND dialog = LowestOwnedDialog(target); dialog) {
            // 目标有自己的对话框浮着：边框该紧跟在最下面那个对话框之后，而且**不该**
            // 在 topmost 层——在的话就会把对话框盖穿。
            HWND next = GetWindow(dialog, GW_HWNDNEXT);
            for (int step = 0; step < kZOrderStepLimit && next; ++step) {
                if (IsWindowVisible(next)) break;
                next = GetWindow(next, GW_HWNDNEXT);
            }
            if (next != hwnd_ || isTopmost) SyncZOrder();
            return;
        } else {
            // 期望：边框在 topmost 层，而且它**下面**紧接着的就是普通层——也就是它待在
            // topmost 层的末尾。压在它上面的都是 topmost 窗口，那是对的，不用管。
            if (!isTopmost) {
                SyncZOrder();
                return;
            }
            // 往下看一眼：如果下面还有别的可见 topmost 窗口，说明边框没在末尾，
            // 那个窗口正被边框盖着。
            HWND below = GetWindow(hwnd_, GW_HWNDNEXT);
            for (int step = 0; step < kZOrderStepLimit && below; ++step) {
                if (IsWindowVisible(below)) {
                    if ((GetWindowLongPtrW(below, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                        SyncZOrder();
                    }
                    return;
                }
                below = GetWindow(below, GW_HWNDNEXT);
            }
            return;
        }

        // 其余窗口：只要边框排在目标**下方**就算对，不要求紧贴。
        //
        // 不能要求紧贴。目标完整性更高时（任务管理器）插不进它的正下方，SyncZOrder 会
        // 退到再往下一格——要求紧贴的话每 500ms 都判定「漂了」，重排，再判定漂了，
        // 白白空转。真正要防的只有一件事：边框跑到目标**上方**去。
        //
        // 从边框往**上**找目标，不是从目标往下找边框。问的是同一件事，代价差一个
        // 数量级：往下要走过目标下面的所有窗口（桌面上几百个顶级窗口）；往上则是
        // 紧贴时一步、退了一格时两步命中。而 ApplyBorders 每次焦点切换都会对每一个
        // 边框跑一遍这个检查，那正是「切换窗口时边框反应慢半拍」的来源。
        HWND probe = hwnd_;
        for (int step = 0; step < kZOrderStepLimit; ++step) {
            probe = GetWindow(probe, GW_HWNDPREV);
            if (!probe) break;           // 走到顶都没碰到目标：边框在目标上方，要重排
            if (probe == target) return; // 目标在边框上面：位置对
        }
        SyncZOrder();
    }

    // 几何事件的快速路径：只移动，从不重绘。
    //
    // 这里一个节流都没有，是故意的。原来有个 15ms 的最小间隔，理由是「一帧之内只有
    // 一次移动看得见」——但那个推理有个洞：**我们不知道帧边界在哪**。15ms 和 60Hz 的
    // 16.7ms 打拍，每隔几帧就有一个事件恰好落在节流窗口里被丢掉，边框于是周期性地
    // 落后一帧。使用者看到的「不丝滑、有残影」就是这个拍频。
    //
    // 不节流的代价很小：Reposition 自己带「位置没变就跳过」的短路，而真正需要移动时
    // 每个事件对应一次真实的窗口移动，跟着走才是最准的。一次 80 步的拖动约 274 个
    // 事件，每个只是一次不重绘的 SetWindowPos。
    void MoveTo(const Rect& frame) {
        const bool sizeChanged = frame.width() != model_.frame.width() ||
                                 frame.height() != model_.frame.height();
        model_.frame = frame;
        Reposition();
        // 尺寸变了必须重绘：位图和窗口对不上了，拖到下一个节拍会露出明显错位的边框。
        if (sizeChanged) Redraw();
    }

private:
    // How far the outline extends beyond the window frame. A negative offset pulls it
    // inward over the window, which is the tacky-borders convention and the reason this
    // must not clamp the offset at zero.
    // Pinned wins over active. "This window is stuck in front of everything" is the more
    // surprising state and the one worth spotting across a busy desktop, so it gets the
    // distinct colour and the thicker line; active/inactive is the everyday distinction.
    [[nodiscard]] int StrokeWidth() const {
        const auto& s = owner_.settings_;
        return std::max(1, model_.pinned ? s.pin.width : s.border.width);
    }

    [[nodiscard]] unsigned StrokeColor() const {
        const auto& s = owner_.settings_;
        if (model_.pinned) {
            return s.pin.color == PinSettings::kAccentColor ? SystemAccentColor() : s.pin.color;
        }
        return model_.active ? s.border.activeColor : s.border.inactiveColor;
    }

    [[nodiscard]] int Reach() const {
        const auto& border = owner_.settings_.border;
        return std::max(0, StrokeWidth() + border.offset);
    }

    [[nodiscard]] RECT OuterRect() const {
        const int reach = Reach();
        Rect outer{
            model_.frame.left - reach,
            model_.frame.top - reach,
            model_.frame.right + reach,
            model_.frame.bottom + reach,
        };

        // 夹回窗口所在的屏幕，别让那圈外扩落到隔壁屏幕上，也别压在任务栏上面。
        //
        // 用 MonitorFromWindow 而不是 MonitorFromRect：这里的 model_.frame 本身就是从这个
        // 窗口算出来的（还带一层内缩量缓存），frame 一旦偏了，拿它去问监视器会连屏都选错，
        // 于是夹到隔壁屏的边界上——那正是要修的毛病，不能让修复本身依赖同一个可疑输入。
        // 直接问 HWND 归哪块屏是独立的、可信的。
        if (HWND target = HwndFromId(model_.windowId)) {
            if (HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST)) {
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info)) {
                    outer = ClampBorderToScreen(model_.frame, outer, ToCoreRect(info.rcMonitor),
                                                ToCoreRect(info.rcWork), reach);
                }
            }
        }
        return ToWinRect(outer);
    }

    void Reposition() {
        if (!hwnd_) return;
        const RECT outer = OuterRect();
        const int width = std::max<LONG>(1, outer.right - outer.left);
        const int height = std::max<LONG>(1, outer.bottom - outer.top);
        if (outer.left == outer_.left && outer.top == outer_.top &&
            width == outer_.right - outer_.left && height == outer_.bottom - outer_.top) {
            return;
        }

        // A single SetWindowPos is the whole point of the one-window design: this runs on
        // every location event during a drag. NOSENDCHANGING skips the
        // WM_WINDOWPOSCHANGING round trip; NOREDRAW and NOCOPYBITS stop Windows
        // invalidating and blitting content that has not changed - a move is just a move.
        SetWindowPos(hwnd_, nullptr, outer.left, outer.top, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING |
                     SWP_NOREDRAW | SWP_NOCOPYBITS);
        outer_ = RECT{outer.left, outer.top, outer.left + width, outer.top + height};
    }

    void ApplyVisibility() {
        if (!hwnd_) return;
        HWND target = HwndFromId(model_.windowId);
        const bool show = model_.visible && IsWindow(target);
        if (show == shown_) return;
        shown_ = show;
        ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }

    [[nodiscard]] float CornerRadius() const {
        const auto& border = owner_.settings_.border;
        switch (border.corners) {
        case BorderCorners::Square:     return 0.0F;
        case BorderCorners::Round:      return kRoundRadius;
        case BorderCorners::RoundSmall: return kRoundSmallRadius;
        case BorderCorners::Custom:     return static_cast<float>(std::max(0, border.cornerRadius));
        case BorderCorners::Auto:
        default:                        return SystemCornerRadius(HwndFromId(model_.windowId));
        }
    }

    void Redraw() {
        if (!hwnd_ || !shown_) return;
        if (!owner_.EnsureRenderTarget()) return;

        const int width = std::max<LONG>(1, outer_.right - outer_.left);
        const int height = std::max<LONG>(1, outer_.bottom - outer_.top);

        // Shared scratch, sized to the largest window seen so far. UpdateLayeredWindow
        // copies the source, so the next outline reuses the same bitmap immediately -
        // one allocation for the whole process instead of one per border.
        LayeredSurface* surface = owner_.surface_.get();
        if (!surface || !surface->EnsureAtLeast(width, height)) return;

        const float stroke = static_cast<float>(StrokeWidth());
        const float inset = stroke * 0.5F;

        float radius = CornerRadius();
        // The stroke is centred on the path, which sits `reach - inset` in from the edge
        // of the outline; grow the radius by that offset so the curve stays concentric
        // with the window's own corner instead of pinching.
        if (radius > 0.0F) radius += static_cast<float>(Reach()) - inset;

        auto& target = owner_.renderTarget_;
        const RECT bind{0, 0, width, height};
        if (FAILED(target->BindDC(surface->dc(), &bind))) return;

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(
            ToD2DColor(StrokeColor()), &brush);
        if (brush) {
            const D2D1_RECT_F path = D2D1::RectF(inset, inset,
                                                 static_cast<float>(width) - inset,
                                                 static_cast<float>(height) - inset);
            if (radius > 0.0F) {
                target->DrawRoundedRectangle(
                    D2D1::RoundedRect(path, radius, radius), brush.Get(), stroke);
            } else {
                target->DrawRectangle(path, brush.Get(), stroke);
            }
        }

        const HRESULT hr = target->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            target.Reset();
            return;
        }
        if (FAILED(hr)) return;

        GdiFlush();

        POINT source{0, 0};
        SIZE size{width, height};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        HDC screen = GetDC(nullptr);
        UpdateLayeredWindow(hwnd_, screen, nullptr, &size, surface->dc(), &source, 0,
                            &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

    WinBorderBackend& owner_;
    const bool alwaysTopmost_{false};
    static constexpr int kStuckThreshold = 1;

    BorderModel model_;
    HWND hwnd_{};
    RECT outer_{};
    int zOrderFailures_{0};
    bool shown_{false};

    friend class WinBorderBackend;
};

WinBorderBackend::WinBorderBackend() = default;
WinBorderBackend::~WinBorderBackend() { Stop(); }

bool WinBorderBackend::EnsureFactory() {
    if (d2dFactory_) return true;
    return SUCCEEDED(
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()));
}

bool WinBorderBackend::EnsureRenderTarget() {
    if (renderTarget_) return true;
    if (!EnsureFactory()) return false;

    // 96 DPI so one DIP is one physical pixel: every rectangle here is already in physical
    // coordinates, the same reason the bookmark overlay pins its own target.
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0F, 96.0F);
    return SUCCEEDED(d2dFactory_->CreateDCRenderTarget(&properties, &renderTarget_));
}

bool WinBorderBackend::EnsureWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.lpszClassName = kBorderClass;
    wc.hbrBackground = nullptr;
    if (RegisterClassExW(&wc) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool WinBorderBackend::Start(const Settings& settings) {
    if (started_) return true;
    settings_ = settings;
    if (!EnsureFactory() || !EnsureWindowClass()) return false;
    surface_ = std::make_unique<LayeredSurface>();
    StartZOrderWatch();
    started_ = true;
    return true;
}

// z 序守望：一个 message-only 窗口加一个定时器。
//
// 为什么必须轮询，见 BorderWindow::ResyncIfDrifted 的注释——纯 z 序变化不发任何事件。
// 周期取 500ms：错位最多存在半秒，而每次滴答只是对每个边框走几次 GetWindow，
// 位置对就立刻返回，一次 SetWindowPos 都不做。
void WinBorderBackend::StartZOrderWatch() {
    if (watchWindow_) return;

    static const ATOM watchClass = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WatchProc;
        wc.lpszClassName = L"WindowMark.BorderZOrderWatch";
        return RegisterClassExW(&wc);
    }();
    if (watchClass == 0) return;

    watchWindow_ = CreateWindowExW(0, L"WindowMark.BorderZOrderWatch", L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (!watchWindow_) return;
    SetWindowLongPtrW(watchWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetTimer(watchWindow_, kZOrderWatchTimerId, kZOrderWatchIntervalMs, nullptr);

    // 定时器兜底，钩子负责及时。
    //
    // 只靠 500ms 定时器的话，刚刚失去前台的那个窗口的边框会带着 topmost 位多留半秒，
    // 在盖住它的窗口上画出一条孤立线（实测 121 次采样撞见 1 次）。而 Windows 只把
    // EVENT_SYSTEM_FOREGROUND 发给**新的**前台窗口，失去前台的那个什么都收不到——
    // 所以这里挂的是全局钩子，收到就把所有边框重排一遍。
    g_watchWindow = watchWindow_;
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, ForegroundProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

// 钩子回调跑在事件源的时间线上，做重活会拖慢整个系统的窗口切换。只投递一条消息，
// 真正的重排在 WatchProc 里做。
void CALLBACK WinBorderBackend::ForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                               DWORD, DWORD) {
    if (g_watchWindow) PostMessageW(g_watchWindow, kZOrderRecheckMsg, 0, 0);
}

void WinBorderBackend::StopZOrderWatch() noexcept {
    if (!watchWindow_) return;
    if (foregroundHook_) {
        UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
    }
    g_watchWindow = nullptr;
    KillTimer(watchWindow_, kZOrderWatchTimerId);
    DestroyWindow(watchWindow_);
    watchWindow_ = nullptr;
}

LRESULT CALLBACK WinBorderBackend::WatchProc(HWND hwnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam) {
    if ((msg == WM_TIMER && wParam == kZOrderWatchTimerId) || msg == kZOrderRecheckMsg) {
        auto* self = reinterpret_cast<WinBorderBackend*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            for (auto& [id, window] : self->windows_) {
                if (window) window->ResyncIfDrifted();
            }
            self->RecreateStuckBorders();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 请后端在下一个消息循环里处理卡死的边框。
//
// 去重：多个边框同时卡住时只投一条消息，处理时一并扫完。
void WinBorderBackend::RequestStuckRecheck() {
    if (!watchWindow_) return;
    if (stuckRecheckPosted_.exchange(true)) return;
    PostMessageW(watchWindow_, kZOrderRecheckMsg, 0, 0);
}

// 把 z 序冻住的边框窗口就地换成新的。
//
// 由守望轮询调用，不必等下一个窗口事件——卡住的窗口不会自己产生事件，干等就是让
// 使用者一直看着残缺的边框。
void WinBorderBackend::RecreateStuckBorders() {
    stuckRecheckPosted_.store(false);
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!it->second || !it->second->IsStuck()) {
            ++it;
            continue;
        }
        const WindowId id = it->first;
        // 重建也救不回来的极少数情况：再建几次就放弃，让边框留在原处。无限重建会
        // 变成每帧销毁重建一个窗口，那比一条不完整的边框糟糕得多。
        int& tries = recreateTries_[id];
        if (tries >= kMaxRecreateTries) {
            ++it;
            continue;
        }
        ++tries;
        const BorderModel model = it->second->Model();
        PinDiag(L"重建 z序冻住的边框 hwnd=%llu 第%d次",
                static_cast<unsigned long long>(id), tries);
        it = windows_.erase(it);
        auto border = std::make_unique<BorderWindow>(*this, model);
        if (border->Create()) windows_.emplace(id, std::move(border));
    }
}

void WinBorderBackend::ApplyOne(const BorderModel& model) {
    auto it = windows_.find(model.windowId);
    // z 序推不动的边框窗口只能重建——见 BorderWindow::IsStuck。丢掉它，下面的创建
    // 分支立刻建一个新的，使用者看到的顶多是一帧的空档。
    if (it != windows_.end() && it->second->IsStuck()) {
        PinDiag(L"重建 z序冻住的边框 hwnd=%llu",
                static_cast<unsigned long long>(it->first));
        windows_.erase(it);
        it = windows_.end();
    }
    if (it == windows_.end()) {
        auto border = std::make_unique<BorderWindow>(*this, model);
        if (border->Create()) {
            PinDiag(L"描边 %s",
                    DescribeBorderedWindow(HwndFromId(model.windowId)).c_str());
            windows_.emplace(model.windowId, std::move(border));
        }
        return;
    }
    it->second->Update(model);
}

void WinBorderBackend::Apply(const std::vector<BorderModel>& models) {
    if (!started_) return;

    std::unordered_set<WindowId> desired;
    desired.reserve(models.size());
    for (const auto& model : models) desired.insert(model.windowId);

    // 前台窗口的边框走一个专用窗口，别的都走 windows_ 里的普通边框。
    //
    // 那个专用窗口在创建时就带 WS_EX_TOPMOST，此后只移动、只改颜色，永远不做 z 序
    // 调整。这是整段逻辑里最要紧的一条：「把普通层窗口提进 topmost 层」正是会卡死的
    // 那个操作，而生来就在 topmost 层的窗口根本不需要它。之前所有「边框不全、闪一下
    // 才好」的现象都出在那次提升上。
    //
    // 顺带也解决了处理顺序：使用者盯着的那个窗口第一个更新，不必排在十几个边框后面
    // 等一串 UpdateLayeredWindow。
    const BorderModel* active = nullptr;
    for (const auto& model : models) {
        if (model.active) {
            active = &model;
            break;
        }
    }

    if (active) {
        if (!activeBorder_) {
            auto border = std::make_unique<BorderWindow>(*this, *active, true);
            if (border->Create()) activeBorder_ = std::move(border);
        } else {
            activeBorder_->Update(*active);
        }
    } else if (activeBorder_) {
        // 没有活动窗口（比如焦点落在托盘或别的进程），把专用边框收起来，别留一圈
        // 悬在半空的线。
        activeBorder_->Hide();
    }

    for (const auto& model : models) {
        if (&model == active) {
            // 前台窗口的普通边框**隐藏**，不销毁：那一圈线此刻由专用边框画着，但这个
            // 窗口迟早会变回非前台，那时它需要立刻有边框。销毁的话要重新
            // CreateWindow + 绘制 + 排 z 序，那一下看得见；留着只是多一个隐藏窗口。
            BorderModel hidden = model;
            hidden.visible = false;
            ApplyOne(hidden);
            continue;
        }
        ApplyOne(model);
    }

    for (auto it = windows_.begin(); it != windows_.end();) {
        if (desired.contains(it->first)) {
            ++it;
            continue;
        }
        // Paired with the 描边 line above, this gives the outline's lifetime - which is
        // what tells a real window apart from a popup that flashed for a moment.
        PinDiag(L"撤边 hwnd=%llu", static_cast<unsigned long long>(it->first));
        recreateTries_.erase(it->first);
        it = windows_.erase(it);
    }
}

void WinBorderBackend::MoveBorder(WindowId id, const Rect& frame) {
    if (!started_) return;
    const auto it = windows_.find(id);
    if (it == windows_.end()) return;
    it->second->MoveTo(frame);
}


void WinBorderBackend::UpdateSettings(const Settings& settings) {
    settings_ = settings;
    if (!started_) return;
    // Width, offset and colours all change the rendered pixels and the window size, so
    // rebuild rather than patch. Coordinator re-applies the models straight after.
    activeBorder_.reset();
    windows_.clear();
}

void WinBorderBackend::Stop() noexcept {
    StopZOrderWatch();
    activeBorder_.reset();
    windows_.clear();
    surface_.reset();
    renderTarget_.Reset();
    d2dFactory_.Reset();
    started_ = false;
}

} // namespace windowmark::win
