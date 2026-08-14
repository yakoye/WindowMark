#include "WinOverlayBackend.h"

#include "AppIdentity.h"
#include "WinUtil.h"
#include "windowmark/core/DrawerState.h"
#include "windowmark/core/LayoutEngine.h"

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
[[nodiscard]] bool IsOwnDecoration(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0) return false;
    return std::wcscmp(cls, kOverlayClass) == 0 ||
           std::wcscmp(cls, windowmark::app::kBorderWindowClass) == 0;
}
constexpr UINT_PTR kAnimationTimerId = 1;
constexpr UINT_PTR kPreviewTimerId = 2;
constexpr UINT kAnimationTickMs = 16;
constexpr UINT kRenameCommand = 4001;
constexpr UINT kSettingsCommand = 4002;

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
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
// glyphs that actually fit, so truncation there is silent; the full label is one hover
// away regardless.
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

// Backing store for UpdateLayeredWindow: a top-down 32bpp DIB that Direct2D renders
// into with premultiplied alpha, which is exactly the format ULW_ALPHA expects.
class LayeredSurface {
public:
    ~LayeredSurface() { Reset(); }

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
        info.bmiHeader.biHeight = -height; // top-down
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
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

private:
    HDC dc_{};
    HBITMAP bitmap_{};
    HBITMAP previous_{};
    int width_{};
    int height_{};
};

} // namespace

class WinOverlayBackend::OverlayWindow {
public:
    OverlayWindow(
        WinOverlayBackend& owner,
        const OverlayModel& model)
        : owner_(owner), model_(model), drawer_(MakeDrawer(owner, model.placement)) {}

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

        drawer_.Reset(model_.items.size());
        RebuildLabels();
        UpdatePositionAndVisibility();
        Redraw();
        SyncZOrder();
        return true;
    }

    void Destroy() noexcept {
        if (!hwnd_) return;
        KillTimer(hwnd_, kAnimationTimerId);
        KillTimer(hwnd_, kPreviewTimerId);
        surface_.Reset();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    // Side and row placements size their tabs from different settings, so a host that
    // switches between them (maximize/restore) needs its drawer rebuilt.
    static DrawerState MakeDrawer(WinOverlayBackend& owner, Placement placement) {
        const auto metrics = LayoutEngine::MetricsFor(placement, owner.settings_.drawer);
        return DrawerState(metrics.collapsedExtent, metrics.expandedExtent,
                           owner.settings_.drawer.animationMs);
    }

    [[nodiscard]] bool IsShown() const { return appliedVisible_; }

    void UpdateModel(const OverlayModel& model) {
        const bool countChanged = model.items.size() != model_.items.size();
        const bool placementChanged = model.placement != model_.placement;
        // Dragging a host window fires a geometry event every throttle interval. Those
        // only move the overlay, so the expensive part - re-rendering and re-uploading
        // the layered bitmap - is skipped unless the pixels would actually differ.
        const bool contentChanged = countChanged || placementChanged || ItemsDiffer(model.items) ||
                                    model.screenBounds.width() != model_.screenBounds.width() ||
                                    model.screenBounds.height() != model_.screenBounds.height();
        ++g_diag.update;
        if (contentChanged) ++g_diag.content;
        const bool wasVisible = model_.visible;
        model_ = model;

        if (placementChanged) {
            drawer_ = MakeDrawer(owner_, model_.placement);
        }
        if (countChanged || placementChanged) {
            hoveredIndex_ = -1;
            drawer_.Reset(model_.items.size());
            if (owner_.callbacks_.onPreviewHide) owner_.callbacks_.onPreviewHide();
        }
        if (contentChanged) {
            ++g_diag.rebuild;
            DiagTimer t(g_diag.rebuildUs);
            RebuildLabels();
        }

        UpdatePositionAndVisibility();
        if (contentChanged || (model_.visible && !wasVisible)) Redraw();
        SyncZOrder();
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

        // The common case: the host is the window in front, the strip was created topmost,
        // and it is already exactly where it belongs. Nothing to do, and nothing may be
        // done - every raise from here would be ignored anyway.
        if (GetForegroundWindow() == host) { DiagZ(L"宿主在前台, 保持置顶", nullptr, 0); return; }

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

    void RebuildLabels() {
        labelsFull_.clear();
        labelsShort_.clear();
        labelsFull_.reserve(model_.items.size());
        labelsShort_.reserve(model_.items.size());

        // Measuring needs the text format, which is normally created on first draw; ask
        // for it now so the very first layout is trimmed correctly too.
        owner_.EnsureDrawingResources();

        const auto metrics = LayoutEngine::MetricsFor(model_.placement, owner_.settings_.drawer);
        const float extent = static_cast<float>(metrics.collapsedExtent);
        const float pad = std::clamp(extent * 0.1F, 2.0F, 6.0F);
        const float available = std::max(1.0F, extent - pad * 2.0F);
        // Trim against the same size the collapsed tab will actually be drawn with.
        IDWriteTextFormat* format = owner_.FormatFor(
            WinOverlayBackend::FontSizeFor(static_cast<float>(metrics.restThickness)), true);

        for (const auto& item : model_.items) {
            labelsFull_.push_back(Utf8ToWide(item.label));
            labelsShort_.push_back(FitToWidth(
                labelsFull_.back(), owner_.settings_.drawer.shortNameChars, available, format));
        }
    }

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
            mouseTracking_ = false;
            SetHovered(-1);
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
                OnAnimationTick();
                return 0;
            }
            if (wParam == kPreviewTimerId) {
                KillTimer(hwnd_, kPreviewTimerId);
                ShowPreviewForHovered();
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
        }
    }

    // Returns a reference into a reused buffer: this runs on every animation frame and
    // every mouse move, so it must not allocate once the capacity has settled.
    const std::vector<LocalRect>& ItemRects() const {
        std::vector<LocalRect>& rects = rectCache_;
        rects.clear();
        rects.reserve(model_.items.size());
        const float width = static_cast<float>(std::max(1, model_.screenBounds.width()));
        const float height = static_cast<float>(std::max(1, model_.screenBounds.height()));
        const float thickness = static_cast<float>(owner_.settings_.drawer.thickness);
        const float gap = static_cast<float>(owner_.settings_.drawer.gap);

        const auto& extents = drawer_.Extents();
        const float overlap = static_cast<float>(owner_.settings_.drawer.attachOverlap);
        const bool activeOnTop = model_.items.size() == extents.size();

        const auto isActive = [&](std::size_t i) {
            return activeOnTop && model_.items[i].isActive;
        };
        const auto extentAt = [&](std::size_t i) {
            // The bookmark for the window currently in front reaches further out.
            return extents[i] + (isActive(i)
                ? static_cast<float>(owner_.settings_.drawer.activeExtraExtent)
                : 0.0F);
        };

        if (model_.placement == Placement::Left || model_.placement == Placement::Right) {
            float y = 0.0F;
            for (std::size_t i = 0; i < extents.size(); ++i) {
                const float extent = extentAt(i);
                LocalRect r;
                r.top = y;
                r.bottom = std::min(height, y + thickness);
                // The overlay window overhangs the host by attachOverlap. Only the active
                // tab uses that overhang, reaching over the window edge; the others stop
                // at it.
                const float inset = isActive(i) ? 0.0F : overlap;
                if (model_.placement == Placement::Left) {
                    r.right = std::max(1.0F, width - inset);
                    r.left = std::max(0.0F, r.right - extent);
                } else {
                    r.left = std::min(width - 1.0F, inset);
                    r.right = std::min(width, r.left + extent);
                }
                rects.push_back(r);
                y += thickness + gap;
            }
            return rects;
        }

        // Row placements rest at part of their thickness against the window edge and
        // grow toward the host on hover. Both the active tab and the ceiling a hovered
        // tab grows to are the active thickness, so nothing ever stands taller than the
        // bookmark for the window you are actually looking at.
        const auto metrics = LayoutEngine::MetricsFor(model_.placement, owner_.settings_.drawer);
        const float rest = static_cast<float>(metrics.restThickness);
        const float active = static_cast<float>(metrics.activeThickness);
        const float span = std::max(1.0F, static_cast<float>(metrics.expandedExtent - metrics.collapsedExtent));
        const auto thicknessAt = [&](std::size_t i) {
            if (isActive(i)) return active;
            const float progress = std::clamp(
                (extents[i] - static_cast<float>(metrics.collapsedExtent)) / span, 0.0F, 1.0F);
            return rest + (active - rest) * progress;
        };

        float total = 0.0F;
        for (std::size_t i = 0; i < extents.size(); ++i) total += extentAt(i);
        if (!extents.empty()) total += gap * static_cast<float>(extents.size() - 1);

        float x = std::max(0.0F, (width - total) * 0.5F);
        for (std::size_t i = 0; i < extents.size(); ++i) {
            const float extent = extentAt(i);
            const float depth = std::min(height, thicknessAt(i));
            LocalRect r;
            r.left = x;
            r.right = std::min(width, x + extent);
            if (model_.placement == Placement::Top) {
                r.top = 0.0F;
                r.bottom = depth;
            } else {
                r.bottom = height;
                r.top = std::max(0.0F, height - depth);
            }
            rects.push_back(r);
            x += extent + gap;
        }
        return rects;
    }

    int HitTest(int x, int y) const {
        const auto& rects = ItemRects();
        for (std::size_t i = 0; i < rects.size(); ++i) {
            if (Contains(rects[i], static_cast<float>(x), static_cast<float>(y))) {
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
        SetHovered(HitTest(x, y));
    }

    void SetHovered(int index) {
        if (index == hoveredIndex_) return;
        hoveredIndex_ = index;
        if (owner_.callbacks_.onPreviewHide) owner_.callbacks_.onPreviewHide();
        KillTimer(hwnd_, kPreviewTimerId);

        drawer_.SetHovered(index, GetTickCount64());
        if (drawer_.IsAnimating()) {
            SetTimer(hwnd_, kAnimationTimerId, kAnimationTickMs, nullptr);
        } else {
            KillTimer(hwnd_, kAnimationTimerId);
            Redraw();
        }

        if (hoveredIndex_ >= 0 && owner_.settings_.preview.enabled) {
            const auto& item = model_.items[static_cast<std::size_t>(hoveredIndex_)];
            if (!item.isSelf) {
                const UINT delay = static_cast<UINT>(std::max(1, owner_.settings_.preview.delayMs));
                SetTimer(hwnd_, kPreviewTimerId, delay, nullptr);
            }
        }
    }

    void OnAnimationTick() {
        if (!drawer_.Tick(GetTickCount64())) {
            KillTimer(hwnd_, kAnimationTimerId);
            return;
        }
        Redraw();
        if (!drawer_.IsAnimating()) KillTimer(hwnd_, kAnimationTimerId);
    }

    void OnClick(int x, int y) {
        const int index = HitTest(x, y);
        if (index < 0 || static_cast<std::size_t>(index) >= model_.items.size()) return;
        if (owner_.callbacks_.onPreviewHide) owner_.callbacks_.onPreviewHide();
        const auto& item = model_.items[static_cast<std::size_t>(index)];
        if (!item.isSelf && owner_.callbacks_.onActivate) {
            owner_.callbacks_.onActivate(item.targetWindowId);
        }
    }

    void OnContextMenu(int x, int y) {
        const int index = HitTest(x, y);
        if (index < 0 || static_cast<std::size_t>(index) >= model_.items.size()) return;
        if (owner_.callbacks_.onPreviewHide) owner_.callbacks_.onPreviewHide();
        KillTimer(hwnd_, kPreviewTimerId);

        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kRenameCommand, L"重命名...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSettingsCommand, L"设置...");

        POINT screen{x, y};
        ClientToScreen(hwnd_, &screen);
        // The overlay is WS_EX_NOACTIVATE, so it never becomes the foreground window and
        // TrackPopupMenu would leave a menu that does not dismiss on click-away. Handing
        // foreground to the host first is the documented workaround.
        SetForegroundWindow(HwndFromId(model_.hostWindowId));
        const int choice = static_cast<int>(TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            screen.x, screen.y, 0, hwnd_, nullptr));
        DestroyMenu(menu);

        const WindowId target = model_.items[static_cast<std::size_t>(index)].targetWindowId;
        if (choice == static_cast<int>(kRenameCommand)) {
            if (owner_.callbacks_.onRename) owner_.callbacks_.onRename(target);
        } else if (choice == static_cast<int>(kSettingsCommand)) {
            if (owner_.callbacks_.onOpenSettings) owner_.callbacks_.onOpenSettings();
        }
    }

    void ShowPreviewForHovered() {
        if (hoveredIndex_ < 0 || static_cast<std::size_t>(hoveredIndex_) >= model_.items.size()) return;
        const auto& item = model_.items[static_cast<std::size_t>(hoveredIndex_)];
        if (item.isSelf || !owner_.callbacks_.onPreview) return;

        const auto& rects = ItemRects();
        const auto& local = rects[static_cast<std::size_t>(hoveredIndex_)];
        const auto& bounds = model_.screenBounds;
        PreviewRequest request;
        request.hostWindowId = model_.hostWindowId;
        request.sourceWindowId = item.targetWindowId;
        request.placement = model_.placement;
        request.anchorScreenRect = Rect{
            bounds.left + static_cast<int>(std::lround(local.left)),
            bounds.top + static_cast<int>(std::lround(local.top)),
            bounds.left + static_cast<int>(std::lround(local.right)),
            bounds.top + static_cast<int>(std::lround(local.bottom)),
        };
        request.hostFrame = model_.hostFrame;
        request.workArea = model_.workArea;
        owner_.callbacks_.onPreview(request);
    }

    void DrawItems(ID2D1RenderTarget& target) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush);
        if (!brush) return;

        const auto& rects = ItemRects();
        for (std::size_t i = 0; i < rects.size() && i < model_.items.size(); ++i) {
            const auto& item = model_.items[i];
            const bool hovered = static_cast<int>(i) == hoveredIndex_;

            // Every tab is drawn identically. The active one is told apart purely by
            // geometry - it is longer, and it sits over the window edge instead of
            // stopping at it - with no shadow, outline or opacity trickery.
            Color fill = hovered ? Adjust(item.color, 0.05F) : item.color;
            fill.a *= 1.0F - static_cast<float>(
                std::clamp(owner_.settings_.drawer.transparency, 0, 90)) / 100.0F;

            const auto& r = rects[i];
            const float radius = std::min(
                static_cast<float>(owner_.settings_.drawer.cornerRadius),
                std::min(r.right - r.left, r.bottom - r.top) * 0.5F);

            // Every tab grows out of one edge - its root - and is drawn square there so
            // it meets the window seamlessly, like a bookmark slipped between pages.
            // Which edge that is depends on placement: side tabs reach outward from the
            // window, so their root is the edge nearest it; row tabs grow inward from the
            // window's own boundary, so their root is that outer edge.
            //
            // Rounding is extended past the root and then clipped away, which is cheaper
            // and steadier than building a part-rounded path geometry per frame.
            D2D1_RECT_F body = D2D1::RectF(r.left, r.top, r.right, r.bottom);
            D2D1_RECT_F extended = body;
            switch (model_.placement) {
            case Placement::Left:   extended.right += radius; break;
            case Placement::Right:  extended.left -= radius; break;
            case Placement::Top:    extended.top -= radius; break;
            default:                extended.bottom += radius; break;
            }

            brush->SetColor(D2DColor(fill));
            target.PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target.FillRoundedRectangle(D2D1::RoundedRect(extended, radius, radius), brush.Get());
            target.PopAxisAlignedClip();

            // A collapsed tab can be as narrow as 30px, so the padding has to scale
            // down with it or the label is clipped away entirely.
            const float available = r.right - r.left;
            const float pad = hovered ? 10.0F : std::clamp(available * 0.1F, 2.0F, 6.0F);

            // Size the font to this tab's own height. Tabs differ: the active one is at
            // full thickness while the rest rest at part of it, and a font that fits the
            // tall one is clipped out of existence on the short one.
            const std::wstring& text = hovered ? labelsFull_[i] : labelsShort_[i];
            IDWriteTextFormat* format = owner_.FormatFor(
                WinOverlayBackend::FontSizeFor(r.bottom - r.top), !hovered);
            if (!format) continue;
            brush->SetColor(D2D1::ColorF(0.08F, 0.09F, 0.11F, 0.92F));
            const auto textRect = D2D1::RectF(
                r.left + pad,
                r.top,
                std::max(r.left + pad, r.right - pad),
                r.bottom);
            target.DrawTextW(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                format,
                textRect,
                brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);

            // Only meaningful when drawer.active_window_only is off; with it on, the host
            // is the active window and the outline above already says so.
            if (item.isSelf && !item.isActive) {
                brush->SetColor(D2D1::ColorF(0.08F, 0.09F, 0.11F, 0.78F));
                float cx = r.right - 9.0F;
                if (model_.placement == Placement::Right) cx = r.left + 9.0F;
                target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, (r.top + r.bottom) * 0.5F), 2.4F, 2.4F), brush.Get());
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
    LayeredSurface surface_;
    DrawerState drawer_;
    std::vector<std::wstring> labelsFull_;
    std::vector<std::wstring> labelsShort_;
    mutable std::vector<LocalRect> rectCache_;
    Rect appliedBounds_{};
    int hoveredIndex_{-1};
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

bool WinOverlayBackend::Start(const Settings& settings, OverlayCallbacks callbacks) {
    if (started_) return true;
    settings_ = settings;
    callbacks_ = std::move(callbacks);
    if (!EnsureFactories() || !EnsureWindowClass()) {
        callbacks_ = {};
        return false;
    }
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
    textFormats_.clear();
    renderTarget_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    callbacks_ = {};
    started_ = false;
}

} // namespace windowmark::win
