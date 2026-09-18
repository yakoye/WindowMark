#include "WinOverlayBackend.h"

#include "AppIdentity.h"
#include "WinLayeredSurface.h"
#include "WinUtil.h"
#include "windowmark/core/LayoutEngine.h"
#include "windowmark/core/MagneticDock.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <climits>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace windowmark::win {
namespace {

// Phase counters for the drag-latency work. External measurement narrowed the cost to
// this file but cannot see inside it, and every guess made from the outside - hook
// presence, z-order, cross-process ownership, hit-testing - was wrong when tested. These
// answer "which phase, how often, how long" directly.
//
// Off unless WINDOWMARK_DIAG=1 is in the environment, and the flag is read once, so the
// cost when off is one predictable branch per call site.
struct DiagState {
    long long apply{}, update{}, content{}, setpos{}, redraw{}, rebuild{};
    double applyUs{}, rebuildUs{}, drawUs{}, ulwUs{};
    double setposUs{}, setposMaxUs{};
    long long setposOver1ms{}, setposOver5ms{};
    long long overlays{}, overlaysVisible{};   // 最近一次 Apply 后的窗口数
    double lastFlushMs{};
};
DiagState g_diag;

[[nodiscard]] bool DiagOn() {
    static const bool on = [] {
        wchar_t buf[8]{};
        return GetEnvironmentVariableW(L"WINDOWMARK_DIAG", buf, 8) > 0 && buf[0] == L'1';
    }();
    return on;
}

[[nodiscard]] double DiagNowMs() {
    static const double perMs = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
}

// 磁场动画的帧时钟，毫秒。
[[nodiscard]] double NowMs() { return DiagNowMs(); }

// Scoped timer that adds elapsed microseconds to a field.
class DiagTimer {
public:
    explicit DiagTimer(double& sink) : sink_(sink), start_(DiagOn() ? DiagNowMs() : 0.0) {}
    ~DiagTimer() {
        if (DiagOn()) sink_ += (DiagNowMs() - start_) * 1000.0;
    }
    DiagTimer(const DiagTimer&) = delete;
    DiagTimer& operator=(const DiagTimer&) = delete;

private:
    double& sink_;
    double start_;
};

void DiagFlush() {
    if (!DiagOn()) return;
    const double now = DiagNowMs();
    if (g_diag.lastFlushMs == 0.0) {
        g_diag.lastFlushMs = now;
        return;
    }
    if (now - g_diag.lastFlushMs < 2000.0) return;
    const double span = (now - g_diag.lastFlushMs) / 1000.0;

    wchar_t dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) > 0) {
        std::wstring path = std::wstring(dir) + L"\\WindowMark\\diag.log";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f != nullptr) {
            std::fwprintf(
                f,
                L"%.1fs  书签窗口 %lld 个(可见 %lld)  Apply %lld (%.0fus/次)  Update %lld  内容变了 %lld"
                L"  | SetWindowPos %lld 次 均值%.0fus 最长%.0fus 超1ms %lld 超5ms %lld"
                L"  | 重排 %lld (%.0fus)  重绘 %lld (%.0fus, ULW %.0fus)\n",
                span, g_diag.overlays, g_diag.overlaysVisible,
                g_diag.apply, g_diag.apply ? g_diag.applyUs / g_diag.apply : 0.0,
                g_diag.update, g_diag.content,
                g_diag.setpos, g_diag.setpos ? g_diag.setposUs / g_diag.setpos : 0.0,
                g_diag.setposMaxUs, g_diag.setposOver1ms, g_diag.setposOver5ms,
                g_diag.rebuild, g_diag.rebuild ? g_diag.rebuildUs / g_diag.rebuild : 0.0,
                g_diag.redraw, g_diag.redraw ? g_diag.drawUs / g_diag.redraw : 0.0,
                g_diag.redraw ? g_diag.ulwUs / g_diag.redraw : 0.0);
            std::fclose(f);
        }
    }
    const long long keepOverlays = g_diag.overlays;
    const long long keepVisible = g_diag.overlaysVisible;
    g_diag = DiagState{};
    g_diag.overlays = keepOverlays;
    g_diag.overlaysVisible = keepVisible;
    g_diag.lastFlushMs = now;
}

constexpr wchar_t kOverlayClass[] = L"WindowMark.BookmarkOverlay";

// Z-order maintenance for the strip. Same shape as the outline windows use: never move,
// never resize, never activate, and never make the target's own thread field a
// WM_WINDOWPOSCHANGING for it.
constexpr UINT kOverlayZFlags =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING;

// Bounds on the GW_HWNDPREV walk. Two separate numbers on purpose: a desktop carries
// hundreds of hidden top-level windows - 144 sat above one Excel window here - and a
// single budget spent on stepping over them ran out long before the walk reached anything
// visible, every single time. Skipping an invisible sibling is nearly free, so only real
// insertion attempts are rationed; the step cap is just a guard against a pathological
// z-order spinning the UI thread.
constexpr int kZOrderStepLimit = 4096;
constexpr int kZOrderAttemptLimit = 16;

// Our own decoration windows for a host - the outline and the strip - stack in a fixed
// order between the host and everything else, so the z-order walk steps over them rather
// than treating them as obstacles to insert below.
// 自家画在别人窗口上的东西：书签条，和 v0.4.9 起的边框画布。v0.4.9 之前的边框是每个
// 窗口一个 WindowMark.WindowBorder，那种窗口已经不存在，这里曾经一直认的是它，新画布
// 反而不认。
[[nodiscard]] bool IsOwnDecoration(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0) return false;
    return std::wcscmp(cls, kOverlayClass) == 0 ||
           std::wcscmp(cls, windowmark::app::kOverlayWindowClass) == 0;
}

[[nodiscard]] bool IsBookmarkStrip(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0) return false;
    return std::wcscmp(cls, kOverlayClass) == 0;
}
constexpr UINT_PTR kAnimationTimerId = 1;
// 缩略图第一次出现前的延迟（preview.delay_ms）
constexpr UINT_PTR kPreviewTimerId = 2;
// 离开整条书签栏后的宽限（drawer.magnet_grace_ms）
constexpr UINT_PTR kGraceTimerId = 3;
constexpr UINT kAnimationTickMs = 16;
// 视觉值的轻微平滑：约一帧。所有标签同一个时间常数、在同一帧里统一推进。
constexpr float kVisualTauMs = 16.0F;
// 新的主标签要比当前的 influence 高出这么多才换人，鼠标停在两个标签正中间抖动时标题不闪。
constexpr float kPrimaryHysteresis = 0.02F;
// 定时器在跑时两帧之间最多按这么久推进（系统一卡，下一帧也只走一步，不跳）。
constexpr float kMaxFrameMs = 34.0F;
constexpr UINT kRenameCommand = 4001;
constexpr UINT kSettingsCommand = 4002;

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

constexpr wchar_t kMenuHostClass[] = L"WindowMark.BookmarkMenuHost";
// 菜单选中的命令，投递给菜单主人窗口执行。wParam = 命令，lParam = 书签指向的窗口。
constexpr UINT kRunMenuCommand = WM_APP + 41;

// 右键菜单为了能用，临时把前台拿到了本进程；用完还给宿主。只在前台仍是本进程的窗口（或者
// 没有前台）时才还——这期间用户已经点去了别的程序，就不去抢。
void ReturnForeground(WindowId hostId) {
    const HWND host = HwndFromId(hostId);
    if (!IsWindow(host)) return;
    const HWND foreground = GetForegroundWindow();
    if (foreground && !IsOwnProcessWindow(foreground)) return;
    SetForegroundWindow(host);
}

D2D1_COLOR_F D2DColor(const Color& color, float alphaMultiplier = 1.0F) {
    return D2D1::ColorF(color.r, color.g, color.b, std::clamp(color.a * alphaMultiplier, 0.0F, 1.0F));
}

Color Adjust(const Color& c, float delta) {
    return Color{
        std::clamp(c.r + delta, 0.0F, 1.0F),
        std::clamp(c.g + delta, 0.0F, 1.0F),
        std::clamp(c.b + delta, 0.0F, 1.0F),
        c.a,
    };
}


// A collapsed tab is only ~30px wide. An ellipsis would eat one of the two or three
// glyphs that actually fit, so truncation there is silent; the full label is in the
// floating title that appears on hover.
std::wstring Shorten(const std::wstring& input, int maxCodePoints) {
    if (maxCodePoints <= 0 || input.empty()) return {};
    std::wstring out;
    int count = 0;
    for (std::size_t i = 0; i < input.size() && count < maxCodePoints; ++i, ++count) {
        wchar_t ch = input[i];
        out.push_back(ch);
#if WCHAR_MAX <= 0xFFFF
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < input.size()) {
            wchar_t next = input[i + 1];
            if (next >= 0xDC00 && next <= 0xDFFF) {
                out.push_back(next);
                ++i;
            }
        }
#endif
    }
    return out;
}

// Surrogate pairs count as one, matching how Shorten walks the string.
int CountCodePoints(const std::wstring& text) {
    int count = 0;
    for (std::size_t i = 0; i < text.size(); ++i, ++count) {
#if WCHAR_MAX <= 0xFFFF
        if (text[i] >= 0xD800 && text[i] <= 0xDBFF && i + 1 < text.size() &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            ++i;
        }
#endif
    }
    return count;
}

struct LocalRect {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

bool Contains(const LocalRect& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

} // namespace

class WinOverlayBackend::OverlayWindow {
public:
    OverlayWindow(
        WinOverlayBackend& owner,
        const OverlayModel& model)
        : owner_(owner), model_(model) {}

    ~OverlayWindow() { Destroy(); }

    bool Create() {
        const auto& b = model_.screenBounds;
        // Deliberately unowned. Making the host the owner is the obvious way to keep the
        // strip above it, and it is what this did until it was measured: every move of
        // the host makes Windows keep the owned window's bookkeeping in step, and because
        // the owner lives in another process that needs a synchronous round trip to this
        // thread. Dragging an Excel window then ran 175ms behind the cursor and followed
        // only 2% of the mouse path. Dropping the owner brought both back to the numbers
        // measured with WindowMark not running at all. Z-order is maintained explicitly
        // in SyncZOrder instead, the same way the outline windows already do it.
        // WS_EX_TOPMOST is set here, at creation, and that placement is the whole reason
        // this works: Windows silently ignores z-order *raises* requested by a process
        // that does not own the foreground window - SetWindowPos returns TRUE, sets
        // nothing, and moves nothing. Creating the window topmost is not a raise, so it is
        // allowed. Since a strip only exists while its host is the window it belongs to
        // being shown for, being topmost is also the correct place for it to sit.
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kOverlayClass,
            L"",
            WS_POPUP,
            b.left,
            b.top,
            std::max(1, b.width()),
            std::max(1, b.height()),
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);
        if (!hwnd_) return false;

        RebuildDock(true);
        RebuildLabels();
        UpdatePositionAndVisibility();
        Redraw();
        SyncZOrder();
        return true;
    }

    void Destroy() noexcept {
        *alive_ = false;
        if (!hwnd_) return;
        KillTimer(hwnd_, kAnimationTimerId);
        KillTimer(hwnd_, kPreviewTimerId);
        KillTimer(hwnd_, kGraceTimerId);
        HidePreview();
        surface_.Reset();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    [[nodiscard]] bool IsShown() const { return appliedVisible_; }

    void UpdateModel(const OverlayModel& model) {
        const bool countChanged = model.items.size() != model_.items.size();
        const bool placementChanged = model.placement != model_.placement;
        // base 只由方向、个数、激活标签和它在窗口里的起点决定；这些不变，磁场的状态就原样
        // 接着用。
        const bool dockChanged = countChanged || placementChanged ||
                                 ActiveIndexOf(model.items) != ActiveIndexOf(model_.items) ||
                                 model.dockOrigin != model_.dockOrigin;
        // Dragging a host window fires a geometry event every throttle interval. Those
        // only move the overlay, so the expensive part - re-rendering and re-uploading
        // the layered bitmap - is skipped unless the pixels would actually differ.
        const bool contentChanged = dockChanged || ItemsDiffer(model.items) ||
                                    model.screenBounds.width() != model_.screenBounds.width() ||
                                    model.screenBounds.height() != model_.screenBounds.height();
        ++g_diag.update;
        if (contentChanged) ++g_diag.content;
        const bool wasVisible = model_.visible;
        model_ = model;

        if (countChanged || placementChanged) {
            // 旧的磁场状态对不上新的标签了：从静止重新开始。
            ResetInteraction();
        }
        if (dockChanged) RebuildDock(countChanged || placementChanged || !Engaged());
        if (contentChanged) {
            ++g_diag.rebuild;
            DiagTimer t(g_diag.rebuildUs);
            RebuildLabels();
        }

        UpdatePositionAndVisibility();
        if (dockChanged && Engaged()) {
            Frame();   // 鼠标正在栏上：新的 base 由下一帧平滑接过去
        } else if (contentChanged || (model_.visible && !wasVisible)) {
            Redraw();
        }
        SyncZOrder();
        // 宿主挪了，预览栈跟着挪。
        if (previewShown_) EmitPreview();
    }

    // Sit directly above the host window. The host is in another process, so ownership -
    // the mechanism that would do this automatically - is not an option: see Create.
    // Placing the strip by hand costs one GetWindow call in the steady state, because the
    // walk below stops as soon as it finds us and issues no SetWindowPos at all.
    void SyncZOrder() {
        if (!hwnd_) return;
        if (!appliedVisible_) { DiagZ(L"跳过: 未显示", nullptr, 0); return; }
        HWND host = HwndFromId(model_.hostWindowId);
        if (!IsWindow(host)) { DiagZ(L"跳过: 宿主已失效", nullptr, 0); return; }

        // 常见情形：宿主就是前台窗口。书签条挪到 topmost 层的末尾——见 MoveToTopmostBandTail。
        //
        // 宿主自己是置顶窗口时例外（被置顶功能钉住的，或者本来就 always-on-top 的程序）：
        // 层末尾在它下面，挪过去书签条就压到宿主身下，被宿主盖住。这种情况走下面「贴在宿主
        // 正上方」那段。
        if (GetForegroundWindow() == host &&
            (GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            MoveToTopmostBandTail();
            return;
        }

        // Host is buried, which only happens with drawer.active_window_only off. Lowering
        // is not restricted the way raising is, so the strip can be walked down to sit
        // just above its own host again.
        //
        // SetWindowPos inserts *after* hWndInsertAfter, i.e. below it, so passing the host
        // would bury the strip under the window it belongs to. Insert below whatever sits
        // directly above the host instead.
        HWND above = host;
        int attempts = 0;
        for (int step = 0; step < kZOrderStepLimit; ++step) {
            above = GetWindow(above, GW_HWNDPREV);
            if (!above) break;                  // host is already at the top of its band
            if (above == hwnd_) { DiagZ(L"已在位", nullptr, 0); return; }
            // Step over hidden helper windows - the per-thread Default IME window and the
            // like. Windows refuses to slot anything between an owner and a window it
            // owns, and that refusal is silent, so a hidden helper above the host would
            // otherwise strand the strip wherever it happened to be.
            if (!IsWindowVisible(above)) continue;
            // Our own outline for this same host belongs between the host and the strip.
            // Skipping it is what stops the two from swapping places forever, each
            // re-inserting itself just above the host on every update.
            if (IsOwnDecoration(above)) continue;

            // Coming down out of the topmost band has to be asked for explicitly: Windows
            // promotes a window inserted below a topmost one and never demotes it again.
            const bool targetTopmost = (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            if (!targetTopmost && (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, kOverlayZFlags);
            }
            if (SetWindowPos(hwnd_, above, 0, 0, 0, 0, kOverlayZFlags)) {
                DiagZ(L"插到下方成功", above, 0);
                return;
            }
            DiagZ(L"插到下方被拒", above, GetLastError());
            if (++attempts >= kZOrderAttemptLimit) break;
        }

        // Found nothing to anchor to. Staying topmost is the safe outcome: visible but
        // possibly in front of something it should be behind, rather than invisible.
        DiagZ(L"没找到锚点, 维持原状", nullptr, 0);
    }

    // 挪到 topmost 层的末尾：比所有普通窗口高（宿主就在普通层里），比别的程序的 topmost
    // 窗口低——右键菜单、输入法候选框、任务栏、悬浮小窗都能压在书签上面。
    //
    // 不能「保持置顶」了事：topmost 窗口之间谁在上面，看谁最后一次被显示。书签条跟着前台
    // 窗口隐藏、显示，右键一点焦点一晃，它就重新跳回 topmost 层最顶上，把刚弹出来的右键
    // 菜单盖住。
    //
    // 往下挪不算「抬高」，不会被系统静默拒绝。边框画布（WinOverlay 的 MoveToBandTail）
    // 靠同一个做法已经跑了几个版本。
    //
    // 找锚点要跳过自家的窗口。边框画布也在往层末尾挪，书签又应该压在边框线上面：要是把
    // 画布当成锚点，书签条就被插到画布下面，画布下一次再把书签条当锚点插回它下面，两边
    // 每次更新都互换一次位置。
    void MoveToTopmostBandTail() {
        HWND anchor = nullptr;   // 最靠后的一个「别人家的、可见的」topmost 窗口
        HWND hwnd = GetTopWindow(nullptr);
        for (int step = 0; step < kZOrderStepLimit && hwnd != nullptr; ++step) {
            if (hwnd != hwnd_ && IsWindowVisible(hwnd) != FALSE) {
                if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) break;
                if (!IsOwnDecoration(hwnd)) anchor = hwnd;
            }
            hwnd = GetWindow(hwnd, GW_HWNDNEXT);
        }
        if (anchor == nullptr) {
            // topmost 层里只有自家的窗口：已经在别人的 topmost 窗口下面了，没有要让的。
            DiagZ(L"层里没有别人家的置顶窗口", nullptr, 0);
            return;
        }

        // 已经紧挨在锚点下面就不动：中间只允许隔着看不见的窗口和别的书签条。拖动宿主时
        // 这里每次几何更新都会走一遍，稳态下必须一次 SetWindowPos 都不发。
        HWND below = GetWindow(anchor, GW_HWNDNEXT);
        for (int step = 0; step < kZOrderStepLimit && below != nullptr;
             ++step, below = GetWindow(below, GW_HWNDNEXT)) {
            if (below == hwnd_) {
                DiagZ(L"已在置顶层末尾", anchor, 0);
                return;
            }
            if (IsWindowVisible(below) == FALSE || IsBookmarkStrip(below)) continue;
            break;
        }

        if (SetWindowPos(hwnd_, anchor, 0, 0, 0, 0, kOverlayZFlags)) {
            DiagZ(L"挪到置顶层末尾", anchor, 0);
        } else {
            DiagZ(L"挪到置顶层末尾被拒", anchor, GetLastError());
        }
    }

    // 只在 WINDOWMARK_DIAG=1 时写；层级这条路径出问题时从外面完全看不见发生了什么。
    void DiagZ(const wchar_t* what, HWND other, DWORD err) const {
        if (!DiagOn()) return;
        wchar_t dir[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) == 0) return;
        const std::wstring path = std::wstring(dir) + L"\\WindowMark\\diag.log";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || f == nullptr) return;
        wchar_t cls[64]{};
        if (other) GetClassNameW(other, cls, static_cast<int>(std::size(cls)));
        // 调用完之后的真实状态：层级里我们上面还压着几个窗口、宿主上面压着几个、
        // 置顶标志到底有没有被真的设上。返回值说成功不等于生效。
        auto depth = [](HWND h) {
            int n = 0;
            for (HWND w = GetWindow(h, GW_HWNDPREV); w && n < 4096; w = GetWindow(w, GW_HWNDPREV)) {
                ++n;
            }
            return n;
        };
        const HWND host = HwndFromId(model_.hostWindowId);
        const bool topmostBit = (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        std::fwprintf(
            f,
            L"[Z] %-14ls 我=%d 宿主=%d 锚点=%d 置顶位=%d 前台=%d 可见=%d 系统可见=%d 对方=%ls err=%lu\n",
            what, depth(hwnd_), depth(host), other ? depth(other) : -1, topmostBit ? 1 : 0,
            GetForegroundWindow() == host ? 1 : 0, appliedVisible_ ? 1 : 0,
            IsWindowVisible(hwnd_) ? 1 : 0, other ? cls : L"-", err);
        std::fclose(f);
    }

private:
    // 主标签切换时同时存在的几层：最新的在最后，淡入；其余淡出，淡完就删。
    struct Layer {
        int index{};
        float opacity{};
    };

    [[nodiscard]] static int ActiveIndexOf(const std::vector<BookmarkItemModel>& items) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].isActive) return static_cast<int>(i);
        }
        return -1;
    }

    [[nodiscard]] bool ItemsDiffer(const std::vector<BookmarkItemModel>& items) const {
        if (items.size() != model_.items.size()) return true;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& a = items[i];
            const auto& b = model_.items[i];
            if (a.targetWindowId != b.targetWindowId || a.isSelf != b.isSelf ||
                a.isActive != b.isActive || a.label != b.label ||
                a.color.r != b.color.r || a.color.g != b.color.g || a.color.b != b.color.b) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool Side() const {
        return model_.placement == Placement::Left || model_.placement == Placement::Right;
    }

    [[nodiscard]] float WindowWidth() const {
        return static_cast<float>(std::max(1, model_.screenBounds.width()));
    }
    [[nodiscard]] float WindowHeight() const {
        return static_cast<float>(std::max(1, model_.screenBounds.height()));
    }

    // 鼠标在栏上（含离开后的宽限），或者磁场还没退干净。
    [[nodiscard]] bool Engaged() const { return hovering_ || strength_ > 0.0F; }

    // base 排布。只在方向、个数、激活标签或窗口里的起点变了时重算；鼠标怎么动都不碰它。
    void RebuildDock(bool snap) {
        spec_ = LayoutEngine::DockSpecFor(model_.placement, model_.items.size(),
                                          ActiveIndexOf(model_.items), owner_.settings_.drawer);
        baseStarts_ = DockBaseStarts(spec_.items, spec_.params.gap, model_.dockOrigin);
        const std::size_t n = spec_.items.size();
        if (snap || visual_.size() != n) {
            visual_.assign(n, DockItemVisual{});
            for (std::size_t i = 0; i < n; ++i) {
                visual_[i].main = spec_.items[i].main;
                visual_[i].cross = spec_.items[i].cross;
                visual_[i].start = baseStarts_[i];
            }
        }
        target_.resize(n);
    }

    [[nodiscard]] float MeasureWidth(const std::wstring& text, IDWriteTextFormat* format) const {
        if (text.empty() || !format || !owner_.dwriteFactory_) return 0.0F;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(owner_.dwriteFactory_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                format, 4096.0F, 256.0F, &layout))) {
            return 0.0F;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return 0.0F;
        return metrics.width;
    }

    // shortNameChars is an upper bound, not a target: three CJK glyphs are roughly twice
    // as wide as three Latin ones, so a fixed count overflows some labels and the clip
    // then swallows them whole - a tab that looks blank while its neighbours read fine.
    // Trim to what actually fits instead.
    [[nodiscard]] std::wstring FitToWidth(const std::wstring& text, int maxChars,
                                          float available, IDWriteTextFormat* format) const {
        std::wstring candidate = Shorten(text, maxChars);
        if (candidate.empty()) return candidate;

        int chars = CountCodePoints(candidate);
        while (chars > 1 && MeasureWidth(candidate, format) > available) {
            candidate = Shorten(text, --chars);
        }
        return candidate;
    }

    // 标签里只画短名。完整名字只出现在浮动标题里——标签放大时文字跟着等比放大，不换内容。
    void RebuildLabels() {
        labelsShort_.clear();
        labelsShort_.reserve(model_.items.size());

        // Measuring needs the text format, which is normally created on first draw; ask
        // for it now so the very first layout is trimmed correctly too.
        owner_.EnsureDrawingResources();

        // 按一个普通标签静止时的大小来裁：横排是 宽 × 平时高度，侧边是 伸进去的深度 × 厚度。
        const auto metrics = LayoutEngine::MetricsFor(model_.placement, owner_.settings_.drawer);
        labelWidth_ = static_cast<float>(metrics.collapsedExtent);
        labelHeight_ = static_cast<float>(metrics.restThickness);
        const float pad = LabelPad();
        const float available = std::max(1.0F, labelWidth_ - pad * 2.0F);
        labelFormat_ = owner_.FormatFor(WinOverlayBackend::FontSizeFor(labelHeight_), true);

        for (const auto& item : model_.items) {
            labelsShort_.push_back(FitToWidth(
                Utf8ToWide(item.label), owner_.settings_.drawer.shortNameChars, available,
                labelFormat_));
        }
    }

    // A collapsed tab can be as narrow as 30px, so the padding has to scale down with it
    // or the label is clipped away entirely.
    [[nodiscard]] float LabelPad() const { return std::clamp(labelWidth_ * 0.1F, 2.0F, 6.0F); }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<OverlayWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->HandleMessage(msg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT:
            // Layered windows are presented through UpdateLayeredWindow, not WM_PAINT,
            // but the update region still has to be cleared or Windows keeps resending it.
            ValidateRect(hwnd_, nullptr);
            Redraw();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            OnMouseLeave();
            return 0;
        case WM_LBUTTONUP:
            OnClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONUP:
            OnContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wParam == kAnimationTimerId) {
                Frame();
                return 0;
            }
            if (wParam == kPreviewTimerId) {
                KillTimer(hwnd_, kPreviewTimerId);
                if (hovering_) {
                    thumbnailArmed_ = true;
                    Frame();
                }
                return 0;
            }
            if (wParam == kGraceTimerId) {
                EndHover();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd_, nullptr,
                             suggested->left,
                             suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            // The system just repositioned us behind the model's back; force the next
            // model update to reassert the layout it wants.
            appliedBounds_ = Rect{};
            surface_.Reset();
            Redraw();
            return 0;
        }
        case WM_DESTROY:
            surface_.Reset();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    // Moving one host window rebuilds the models for its whole group, so this runs for
    // every sibling overlay too. Both calls below are cheap individually but make DWM
    // do work, so they are skipped whenever the state they would set is already current.
    void UpdatePositionAndVisibility() {
        if (!hwnd_) return;
        const auto& b = model_.screenBounds;
        const int width = std::max(1, b.width());
        const int height = std::max(1, b.height());

        if (b.left != appliedBounds_.left || b.top != appliedBounds_.top ||
            width != appliedBounds_.width() || height != appliedBounds_.height()) {
            ++g_diag.setpos;
            const double spStart = DiagOn() ? DiagNowMs() : 0.0;
            SetWindowPos(hwnd_, nullptr, b.left, b.top, width, height,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            if (DiagOn()) {
                const double us = (DiagNowMs() - spStart) * 1000.0;
                g_diag.setposUs += us;
                g_diag.setposMaxUs = std::max(g_diag.setposMaxUs, us);
                if (us > 1000.0) ++g_diag.setposOver1ms;
                if (us > 5000.0) ++g_diag.setposOver5ms;
            }
            appliedBounds_ = Rect{b.left, b.top, b.left + width, b.top + height};
        }

        const bool shouldShow = model_.visible && IsWindow(HwndFromId(model_.hostWindowId));
        if (shouldShow != appliedVisible_) {
            ShowWindow(hwnd_, shouldShow ? SW_SHOWNOACTIVATE : SW_HIDE);
            appliedVisible_ = shouldShow;
            // 书签条藏起来了，磁场和预览栈没有理由还留着。
            if (!shouldShow) {
                ResetInteraction();
                RebuildDock(true);
            }
        }
    }

    // 这一帧第 i 个标签画在哪，窗口内坐标。标签的根贴着宿主的那条边（Bottom 是窗口下沿、
    // Left 是窗口左沿……），从根往窗口内容方向长 cross 那么深。
    [[nodiscard]] LocalRect TabRect(std::size_t i) const {
        const DockItemVisual& v = visual_[i];
        return SpanRect(v.start, v.start + v.main, v.cross);
    }

    // 主轴区间 [from, to]、从根边起深 depth 的矩形。
    [[nodiscard]] LocalRect SpanRect(float from, float to, float depth) const {
        const float w = WindowWidth();
        const float h = WindowHeight();
        switch (model_.placement) {
        case Placement::Top: return LocalRect{from, 0.0F, to, depth};
        case Placement::Left: return LocalRect{0.0F, from, depth, to};
        case Placement::Right: return LocalRect{w - depth, from, w, to};
        default: return LocalRect{from, h - depth, to, h};
        }
    }

    // 点中的是画出来的那个标签：visual 以鼠标为不动点排开，平时和 base 命中一样，但只有
    // 按画面判定，才在任何情况下都和眼睛看到的一致。
    [[nodiscard]] int HitTest(int x, int y) const {
        const std::size_t n = std::min(visual_.size(), model_.items.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (Contains(TabRect(i), static_cast<float>(x), static_cast<float>(y))) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void OnMouseMove(int x, int y) {
        if (!mouseTracking_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);
            mouseTracking_ = true;
        }
        // 宽限期里回来了：就当没离开过。
        if (graceRunning_) {
            KillTimer(hwnd_, kGraceTimerId);
            graceRunning_ = false;
        }
        // 鼠标只换算成主轴坐标，只和 base 比距离。
        pointer_ = static_cast<float>(Side() ? y : x);
        if (!hovering_) {
            hovering_ = true;
            if (!thumbnailArmed_) {
                const int delay = owner_.settings_.preview.delayMs;
                if (delay <= 0) {
                    thumbnailArmed_ = true;
                } else {
                    SetTimer(hwnd_, kPreviewTimerId, static_cast<UINT>(delay), nullptr);
                }
            }
        }
        // 不等定时器：鼠标一动就算一帧，磁场即时跟上鼠标。
        Frame();
    }

    // 离开整条栏不立刻生效：先等一小段宽限。间隙里有包络带接着鼠标，标签之间不会触发这里。
    void OnMouseLeave() {
        mouseTracking_ = false;
        const int grace = owner_.settings_.drawer.magnetGraceMs;
        if (grace <= 0) {
            EndHover();
            return;
        }
        graceRunning_ = true;
        SetTimer(hwnd_, kGraceTimerId, static_cast<UINT>(grace), nullptr);
    }

    void EndHover() {
        KillTimer(hwnd_, kGraceTimerId);
        KillTimer(hwnd_, kPreviewTimerId);
        graceRunning_ = false;
        hovering_ = false;
        thumbnailArmed_ = false;
        Frame();
    }

    // 回到静止，不带动画：个数或方向变了、书签条被藏起来了。
    void ResetInteraction() {
        if (hwnd_) {
            KillTimer(hwnd_, kAnimationTimerId);
            KillTimer(hwnd_, kPreviewTimerId);
            KillTimer(hwnd_, kGraceTimerId);
        }
        timerRunning_ = false;
        graceRunning_ = false;
        hovering_ = false;
        thumbnailArmed_ = false;
        strength_ = 0.0F;
        primary_ = -1;
        layers_.clear();
        HidePreview();
    }

    // 推进一帧：算、画、交给预览端，没收敛就让定时器接着跑。整条书签栏在同一帧里一起算，
    // 没有哪个标签有自己的动画。
    void Frame() {
        if (!hwnd_) return;
        const double now = NowMs();
        float dt = static_cast<float>(now - lastFrameMs_);
        lastFrameMs_ = now;
        // 定时器停着的时候没有帧，下一帧的 dt 可能是几秒；不截断的话磁场一步到位，进栏
        // 那一下就成了突然放大。
        dt = std::clamp(dt, 0.0F, timerRunning_ ? kMaxFrameMs : static_cast<float>(kAnimationTickMs));
        Step(dt);
        Redraw();
        EmitPreview();

        if (Settled()) {
            if (timerRunning_) {
                KillTimer(hwnd_, kAnimationTimerId);
                timerRunning_ = false;
            }
        } else if (!timerRunning_) {
            SetTimer(hwnd_, kAnimationTimerId, kAnimationTickMs, nullptr);
            timerRunning_ = true;
        }
    }

    void Step(float dt) {
        const DrawerSettings& drawer = owner_.settings_.drawer;
        // 进出书签栏的唯一渐变：磁场强度。约 animation_ms 走完 95%。
        const float strengthTau = static_cast<float>(std::max(0, drawer.animationMs)) / 3.0F;
        strength_ = SmoothToward(strength_, hovering_ ? 1.0F : 0.0F, dt, strengthTau);

        DockTargetSizes(spec_.items, baseStarts_, spec_.params, pointer_, strength_, target_);
        for (std::size_t i = 0; i < visual_.size() && i < target_.size(); ++i) {
            visual_[i].influence = target_[i].influence;
            visual_[i].main = SmoothToward(visual_[i].main, target_[i].main, dt, kVisualTauMs);
            visual_[i].cross = SmoothToward(visual_[i].cross, target_[i].cross, dt, kVisualTauMs);
        }
        // 位置不平滑：用平滑后的尺寸、以鼠标为不动点重新排开。所以间隙永远精确等于 gap，
        // 鼠标下的点永远在鼠标下，标签不会被鼠标追着跑。
        DockArrange(spec_.items, baseStarts_, spec_.params.gap, pointer_, visual_);
        KeepInsideWindow();

        primary_ = DockPrimary(target_, primary_, kPrimaryHysteresis);
        StepLayers(dt);
    }

    // 标签多到窗口装不下整条栏时（工作区都放不下，很少见），把排布整体推回窗口里。
    // 平常 LayoutEngine 已经留足了余量，这里什么都不做。
    void KeepInsideWindow() {
        if (visual_.empty()) return;
        const float length = Side() ? WindowHeight() : WindowWidth();
        const float lo = visual_.front().start;
        const float hi = visual_.back().start + visual_.back().main;
        float shift = 0.0F;
        if (lo < 0.0F) {
            shift = -lo;
        } else if (hi > length) {
            shift = length - hi;
        }
        if (shift == 0.0F) return;
        for (auto& v : visual_) v.start += shift;
    }

    void StepLayers(float dt) {
        if (primary_ >= 0 && (layers_.empty() || layers_.back().index != primary_)) {
            // 回到一个还没淡完的标签：接着它现在的不透明度往上走，不从 0 重新开始。
            Layer next{primary_, layers_.empty() ? 1.0F : 0.0F};
            const auto it = std::find_if(layers_.begin(), layers_.end(),
                                         [&](const Layer& l) { return l.index == primary_; });
            if (it != layers_.end()) {
                next = *it;
                layers_.erase(it);
            }
            layers_.push_back(next);
        }
        if (layers_.empty()) return;

        const float fadeMs = static_cast<float>(std::max(0, owner_.settings_.preview.crossfadeMs));
        const float delta = fadeMs > 0.0F ? dt / fadeMs : 1.0F;
        for (std::size_t i = 0; i + 1 < layers_.size(); ++i) {
            layers_[i].opacity = std::max(0.0F, layers_[i].opacity - delta);
        }
        layers_.back().opacity = std::min(1.0F, layers_.back().opacity + delta);
        layers_.erase(std::remove_if(layers_.begin(), layers_.end() - 1,
                                     [](const Layer& l) { return l.opacity <= 0.0F; }),
                      layers_.end() - 1);
    }

    [[nodiscard]] bool Settled() const {
        if (strength_ != (hovering_ ? 1.0F : 0.0F)) return false;
        for (std::size_t i = 0; i < visual_.size() && i < target_.size(); ++i) {
            if (visual_[i].main != target_[i].main || visual_[i].cross != target_[i].cross) {
                return false;
            }
        }
        if (layers_.size() > 1) return false;
        return layers_.empty() || layers_.back().opacity >= 1.0F;
    }

    // 把这一帧交给预览端：标签矩形、主标签位置、各层标题。三段怎么排由它算。
    void EmitPreview() {
        if (menuOpen_) return;
        if (!hovering_ && strength_ <= 0.0F) {
            // 磁场退干净了：预览栈收起，下次进来从头开始。
            layers_.clear();
            primary_ = -1;
            HidePreview();
            return;
        }
        if (layers_.empty() || !appliedVisible_ || !owner_.callbacks_.onPreview) {
            HidePreview();
            return;
        }

        const Rect& b = appliedBounds_;
        const float originX = static_cast<float>(b.left);
        const float originY = static_cast<float>(b.top);

        PreviewRequest request;
        request.hostWindowId = model_.hostWindowId;
        request.placement = model_.placement;
        request.workArea = model_.workArea;
        switch (model_.placement) {
        case Placement::Top: request.rootEdge = static_cast<float>(b.top); break;
        case Placement::Left: request.rootEdge = static_cast<float>(b.left); break;
        case Placement::Right: request.rootEdge = static_cast<float>(b.right); break;
        default: request.rootEdge = static_cast<float>(b.bottom); break;
        }
        const std::size_t n = std::min(visual_.size(), model_.items.size());
        request.tabs.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const LocalRect r = TabRect(i);
            request.tabs.push_back(RectF{originX + r.left, originY + r.top,
                                         originX + r.right, originY + r.bottom});
        }

        // 各层所属标签的中心按不透明度加权：切换时标题和缩略图从旧主标签连续滑到新主标签，
        // 稳定时就正对主标签，跟着它一起动，没有追赶的延迟。
        const float mainOrigin = Side() ? originY : originX;
        float sum = 0.0F;
        float weight = 0.0F;
        for (const Layer& layer : layers_) {
            if (static_cast<std::size_t>(layer.index) >= n) continue;
            const DockItemVisual& v = visual_[static_cast<std::size_t>(layer.index)];
            sum += (mainOrigin + v.start + v.main * 0.5F) * layer.opacity;
            weight += layer.opacity;
        }
        if (weight <= 0.0F) {
            HidePreview();
            return;
        }
        request.anchorMain = sum / weight;
        request.opacity = strength_;
        request.thumbnailArmed = thumbnailArmed_;
        request.layers.reserve(layers_.size());
        for (const Layer& layer : layers_) {
            if (static_cast<std::size_t>(layer.index) >= n) continue;
            const BookmarkItemModel& item = model_.items[static_cast<std::size_t>(layer.index)];
            request.layers.push_back(
                PreviewLayer{item.targetWindowId, item.label, !item.isSelf, layer.opacity});
        }
        owner_.callbacks_.onPreview(request);
        previewShown_ = true;
    }

    void HidePreview() {
        if (!previewShown_) return;
        previewShown_ = false;
        if (owner_.callbacks_.onPreviewHide) owner_.callbacks_.onPreviewHide();
    }

    void OnClick(int x, int y) {
        const int index = HitTest(x, y);
        if (index < 0 || static_cast<std::size_t>(index) >= model_.items.size()) return;
        HidePreview();
        const auto& item = model_.items[static_cast<std::size_t>(index)];
        if (!item.isSelf && owner_.callbacks_.onActivate) {
            owner_.callbacks_.onActivate(item.targetWindowId);
        }
    }

    void OnContextMenu(int x, int y) {
        const int index = HitTest(x, y);
        if (index < 0 || static_cast<std::size_t>(index) >= model_.items.size()) return;
        // 菜单开着的时候这条书签条可能被销毁（宿主关了、设置改了），菜单返回以后只用这几个
        // 局部变量，不再碰成员。
        WinOverlayBackend& owner = owner_;
        const HWND menuHost = owner.menuHost_;
        if (!menuHost) return;
        const WindowId target = model_.items[static_cast<std::size_t>(index)].targetWindowId;
        const WindowId hostId = model_.hostWindowId;
        const std::shared_ptr<bool> alive = alive_;

        // 菜单是模态的，打开以后鼠标就不在书签栏上了：磁场开始回落，预览栈先收起来，菜单
        // 开着的这段时间也不再弹出来盖住它。
        menuOpen_ = true;
        KillTimer(hwnd_, kGraceTimerId);
        KillTimer(hwnd_, kPreviewTimerId);
        graceRunning_ = false;
        hovering_ = false;
        thumbnailArmed_ = false;
        HidePreview();

        HMENU menu = CreatePopupMenu();
        if (!menu) {
            menuOpen_ = false;
            Frame();
            return;
        }
        AppendMenuW(menu, MF_STRING, kRenameCommand, L"重命名...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSettingsCommand, L"设置...");

        POINT screen{x, y};
        ClientToScreen(hwnd_, &screen);

        // 菜单的主人必须是前台窗口——托盘菜单一直就是这么做的。书签条是 WS_EX_NOACTIVATE
        // 的，当不了前台。以前是把前台交给宿主窗口，可宿主在别的进程里，菜单所属的线程于是
        // 不是前台线程：菜单弹得出来，点上面的项却不算数（用户报告「重命名、设置点了不管用」），
        // 点别处也关不掉。现在由本进程里一个看不见、能激活的窗口来当菜单的主人。
        //
        // 前台临时落到本进程的隐藏窗口上，窗口跟踪看不到这件事（事件钩子跳过本进程），宿主
        // 仍被当作活动窗口，书签条和边框都不会因此变化。
        SetForegroundWindow(menuHost);
        Frame();   // 让回落动画在菜单的模态循环里跑起来
        const UINT choice = static_cast<UINT>(TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            screen.x, screen.y, 0, menuHost, nullptr));
        // 文档要求的配套动作：少了这一条，下一次点别处时菜单可能关不掉。
        PostMessageW(menuHost, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (choice == kRenameCommand || choice == kSettingsCommand) {
            // 命令留到这次消息处理结束之后再执行：对话框是模态的，不该嵌在书签条自己的消息
            // 处理里跑——对话框开着的时候这条书签条随时可能被销毁。
            owner.menuReturnHost_ = hostId;
            PostMessageW(menuHost, kRunMenuCommand, static_cast<WPARAM>(choice),
                         static_cast<LPARAM>(target));
        } else {
            // 什么都没选：把前台还给宿主，不然它的标题栏会一直是灰的。
            ReturnForeground(hostId);
        }

        if (!*alive) return;
        menuOpen_ = false;
    }

    void DrawItems(ID2D1RenderTarget& target) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush);
        if (!brush) return;

        const std::size_t n = std::min(visual_.size(), model_.items.size());

        // 包络带：相邻标签之间的间隙画 alpha = 1/255，肉眼看不见，但分层窗口只有 alpha 为 0
        // 的像素才让鼠标穿过去。没有它，鼠标滑过间隙就落到宿主窗口上，书签栏以为鼠标走了，
        // 整条栏在两个标签之间塌下去——这就是「间隙死区」。深度取两侧较矮的那个：从间隙往上
        // 走出这个高度，才算离开书签栏。两头各多伸 1px 压到标签底下，接缝处不留 0 像素。
        brush->SetColor(D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F / 255.0F));
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const float from = visual_[i].start + visual_[i].main - 1.0F;
            const float to = visual_[i + 1].start + 1.0F;
            const float depth = std::min(visual_[i].cross, visual_[i + 1].cross);
            const LocalRect band = SpanRect(from, to, depth);
            target.FillRectangle(D2D1::RectF(band.left, band.top, band.right, band.bottom),
                                 brush.Get());
        }

        const float opacity = 1.0F - static_cast<float>(
            std::clamp(owner_.settings_.drawer.transparency, 0, 90)) / 100.0F;
        const float pad = LabelPad();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& item = model_.items[i];
            const LocalRect r = TabRect(i);
            const float width = r.right - r.left;
            const float height = r.bottom - r.top;
            if (width <= 0.0F || height <= 0.0F) continue;

            // 越靠近鼠标越亮一点，跟着磁力连续变，没有「悬停 / 没悬停」两档。
            Color fill = Adjust(item.color, 0.05F * visual_[i].influence);
            fill.a *= opacity;

            const float radius = std::min(
                static_cast<float>(owner_.settings_.drawer.cornerRadius),
                std::min(width, height) * 0.5F);

            // Every tab grows out of one edge - its root - and is drawn square there so it
            // meets the window edge seamlessly, like a bookmark slipped between pages. The
            // root is the host's own edge the strip sits against: the bottom edge for a
            // bottom row, the left edge for a left strip, and so on.
            //
            // Rounding is extended past the root and then clipped away, which is cheaper
            // and steadier than building a part-rounded path geometry per frame.
            const D2D1_RECT_F body = D2D1::RectF(r.left, r.top, r.right, r.bottom);
            D2D1_RECT_F extended = body;
            switch (model_.placement) {
            case Placement::Left:   extended.left -= radius; break;
            case Placement::Right:  extended.right += radius; break;
            case Placement::Top:    extended.top -= radius; break;
            default:                extended.bottom += radius; break;
            }

            brush->SetColor(D2DColor(fill));
            target.PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target.FillRoundedRectangle(D2D1::RoundedRect(extended, radius, radius), brush.Get());
            target.PopAxisAlignedClip();

            // 短名按一个普通静止标签的大小排版，再整体等比放大到这一帧的标签大小：字号是
            // 连续变化的，不会在整数字号之间跳。取宽高两个比例里小的那个，字永远装得下。
            const std::wstring& text = labelsShort_[i];
            if (!text.empty() && labelFormat_) {
                const float scale = std::max(
                    1.0F, std::min(width / labelWidth_, height / labelHeight_));
                const float cx = (r.left + r.right) * 0.5F;
                const float cy = (r.top + r.bottom) * 0.5F;
                const D2D1_RECT_F box = D2D1::RectF(
                    cx - labelWidth_ * 0.5F + pad, cy - labelHeight_ * 0.5F,
                    cx + labelWidth_ * 0.5F - pad, cy + labelHeight_ * 0.5F);
                target.SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)));
                brush->SetColor(D2D1::ColorF(0.08F, 0.09F, 0.11F, 0.92F));
                target.DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), labelFormat_, box,
                                 brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                target.SetTransform(D2D1::Matrix3x2F::Identity());
            }

            // Only meaningful when drawer.active_window_only is off; with it on, the host
            // is the active window and the outline above already says so.
            if (item.isSelf && !item.isActive) {
                brush->SetColor(D2D1::ColorF(0.08F, 0.09F, 0.11F, 0.78F));
                float cx = r.right - 9.0F;
                if (model_.placement == Placement::Right) cx = r.left + 9.0F;
                target.FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cx, (r.top + r.bottom) * 0.5F), 2.4F, 2.4F),
                    brush.Get());
            }
        }
    }

    // Renders into an offscreen premultiplied-alpha DIB and presents it with
    // UpdateLayeredWindow. That is what gives the tabs real per-pixel transparency
    // and antialiased corners; an HWND render target composites onto an opaque window
    // surface, which is why a transparent Clear() previously showed up as solid black.
    void Redraw() {
        if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
        if (!owner_.EnsureDrawingResources()) return;
        ++g_diag.redraw;
        DiagTimer drawTimer(g_diag.drawUs);

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int width = std::max<LONG>(1, rc.right - rc.left);
        const int height = std::max<LONG>(1, rc.bottom - rc.top);
        if (!surface_.Ensure(width, height)) return;

        const RECT bind{0, 0, width, height};
        if (FAILED(owner_.renderTarget_->BindDC(surface_.dc(), &bind))) return;

        owner_.renderTarget_->BeginDraw();
        owner_.renderTarget_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
        DrawItems(*owner_.renderTarget_.Get());
        const HRESULT hr = owner_.renderTarget_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            owner_.renderTarget_.Reset();
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
        {
            DiagTimer ulwTimer(g_diag.ulwUs);
            UpdateLayeredWindow(hwnd_, screen, nullptr, &size, surface_.dc(), &source, 0, &blend,
                                ULW_ALPHA);
        }
        ReleaseDC(nullptr, screen);
    }

    WinOverlayBackend& owner_;
    OverlayModel model_;
    HWND hwnd_{};
    // 模态循环（右键菜单）返回时用来判断这个对象还在不在。
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    LayeredSurface surface_;

    // base：只由设置和模型决定
    DockSpec spec_;
    std::vector<float> baseStarts_;
    // 每帧的目标（未平滑）和画出来的样子（平滑后、以鼠标为不动点排开）
    std::vector<DockItemVisual> target_;
    std::vector<DockItemVisual> visual_;
    float pointer_{};          // 鼠标的主轴坐标，窗口内
    float strength_{};         // 磁场强度 0..1
    bool hovering_{false};     // 鼠标在栏上，含离开后的宽限
    bool graceRunning_{false};
    bool thumbnailArmed_{false};
    int primary_{-1};
    std::vector<Layer> layers_;
    bool previewShown_{false};
    bool menuOpen_{false};
    bool timerRunning_{false};
    double lastFrameMs_{};

    std::vector<std::wstring> labelsShort_;
    IDWriteTextFormat* labelFormat_{};
    float labelWidth_{1.0F};
    float labelHeight_{1.0F};
    Rect appliedBounds_{};
    bool appliedVisible_{false};
    bool mouseTracking_{false};

    friend class WinOverlayBackend;
};

WinOverlayBackend::WinOverlayBackend() = default;
WinOverlayBackend::~WinOverlayBackend() { Stop(); }

bool WinOverlayBackend::EnsureFactories() {
    if (!d2dFactory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
            return false;
        }
    }
    if (!dwriteFactory_) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
            return false;
        }
    }
    return true;
}

bool WinOverlayBackend::EnsureDrawingResources() {
    if (!renderTarget_) {
        // 96 DPI on purpose: every rectangle the LayoutEngine produces is already in
        // physical pixels, so 1 DIP must map to 1 pixel. Inheriting the system DPI is
        // what previously scaled the tabs off the right edge of their own window.
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F,
            96.0F);
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, &renderTarget_))) {
            return false;
        }
    }

    return true;
}

int WinOverlayBackend::FontSizeFor(float height) {
    // Measured: Segoe UI Variable Text reports a line height of roughly 1.33x the font
    // size (13px -> 17.3px). Dividing by 1.45 leaves headroom so the glyphs never reach
    // the clip edge - text that overflows vertically is clipped away completely, not
    // trimmed, which is exactly the "blank bookmark" symptom.
    //
    // The lower bound matters: drawer.thickness can go down to 20, halved to a 10px
    // collapsed tab, and a 8px font already needs 10.6px of line height there.
    const int size = static_cast<int>(height / 1.45F);
    return std::clamp(size, 7, 13);
}

IDWriteTextFormat* WinOverlayBackend::FormatFor(int pixelSize, bool centred) {
    const auto key = std::make_pair(pixelSize, centred);
    if (const auto it = textFormats_.find(key); it != textFormats_.end()) {
        return it->second.Get();
    }
    if (!dwriteFactory_) return nullptr;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    const float size = static_cast<float>(pixelSize);
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"zh-CN", &format);
    if (FAILED(hr)) {
        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"zh-CN", &format);
        if (FAILED(hr)) return nullptr;
    }
    format->SetTextAlignment(centred ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    auto [slot, _] = textFormats_.insert_or_assign(key, std::move(format));
    return slot->second.Get();
}

bool WinOverlayBackend::EnsureWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = OverlayWindow::WndProc;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (RegisterClassExW(&wc) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool WinOverlayBackend::EnsureMenuHost() {
    if (menuHost_) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = MenuHostProc;
    wc.lpszClassName = kMenuHostClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // 普通的顶层弹出窗口，从不显示。不能用 HWND_MESSAGE 的纯消息窗口：那种窗口当不了前台。
    menuHost_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMenuHostClass, L"", WS_POPUP, 0, 0, 0, 0,
                                nullptr, nullptr, wc.hInstance, this);
    return menuHost_ != nullptr;
}

LRESULT CALLBACK WinOverlayBackend::MenuHostProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<WinOverlayBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == kRunMenuCommand && self) {
        self->RunMenuCommand(static_cast<UINT>(wParam), static_cast<WindowId>(lParam));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WinOverlayBackend::RunMenuCommand(UINT command, WindowId target) {
    const WindowId host = menuReturnHost_;
    if (command == kRenameCommand) {
        if (callbacks_.onRename) callbacks_.onRename(target);
    } else if (command == kSettingsCommand) {
        if (callbacks_.onOpenSettings) callbacks_.onOpenSettings();
    }
    // 对话框关掉以后把前台还给宿主：菜单为了能用，把前台临时拿到了本进程。
    ReturnForeground(host);
}

bool WinOverlayBackend::Start(const Settings& settings, OverlayCallbacks callbacks) {
    if (started_) return true;
    settings_ = settings;
    callbacks_ = std::move(callbacks);
    if (!EnsureFactories() || !EnsureWindowClass()) {
        callbacks_ = {};
        return false;
    }
    // 建不出来只影响书签的右键菜单（OnContextMenu 会直接不弹），不值得让整个书签功能起不来。
    EnsureMenuHost();
    started_ = true;
    return true;
}

void WinOverlayBackend::Apply(const std::vector<OverlayModel>& models) {
    if (!started_) return;
    ++g_diag.apply;
    DiagTimer applyTimer(g_diag.applyUs);
    std::unordered_set<WindowId> desired;
    desired.reserve(models.size());

    for (const auto& model : models) {
        // An overlay nobody can see has no reason to exist as a window. With
        // drawer.active_window_only on, exactly one model per group is visible, yet a
        // window used to be created for every member - twelve of them on a normal
        // desktop, eleven permanently hidden.
        //
        // They were not free. Measured against a fast Excel drag, their mere existence
        // put the dragged window 69ms behind the cursor; hiding them, or never moving
        // them, changed nothing, and destroying them dropped it to the same level as
        // running with bookmarks off. Windows keeps owner/owned bookkeeping in step
        // whenever an owner moves, that bookkeeping needs an answer from this thread,
        // and this thread is busy draining location events. Every window that does not
        // need to exist is one more round trip the dragged app waits for.
        if (!model.visible) continue;
        desired.insert(model.hostWindowId);
        auto it = windows_.find(model.hostWindowId);
        if (it == windows_.end()) {
            auto overlay = std::make_unique<OverlayWindow>(*this, model);
            if (overlay->Create()) {
                windows_.emplace(model.hostWindowId, std::move(overlay));
            }
        } else {
            it->second->UpdateModel(model);
        }
    }

    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!desired.contains(it->first)) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
    if (DiagOn()) {
        g_diag.overlays = static_cast<long long>(windows_.size());
        g_diag.overlaysVisible = 0;
        for (const auto& [id, overlay] : windows_) {
            (void)id;
            if (overlay->IsShown()) ++g_diag.overlaysVisible;
        }
    }
    DiagFlush();
}


void WinOverlayBackend::UpdateSettings(const Settings& settings) {
    settings_ = settings;
    if (!started_) return;

    // Sizing, animation timing and placement all come from settings, so each overlay is
    // rebuilt from scratch rather than patched. Coordinator re-applies the models right
    // after this, which recreates them with the new numbers.
    windows_.clear();
}

void WinOverlayBackend::Stop() noexcept {
    windows_.clear();
    if (menuHost_) {
        DestroyWindow(menuHost_);
        menuHost_ = nullptr;
    }
    textFormats_.clear();
    renderTarget_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    callbacks_ = {};
    started_ = false;
}

} // namespace windowmark::win
