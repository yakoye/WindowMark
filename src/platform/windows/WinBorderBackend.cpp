#include "WinBorderBackend.h"

#include "WinUtil.h"

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

// Bounds on the GW_HWNDPREV walk in SyncZOrder. Two numbers rather than one because a
// desktop carries hundreds of hidden top-level windows - 144 sat above a single Excel
// window here - and a shared budget was being spent entirely on stepping over them,
// reaching nothing visible and then falling through to a fallback that cannot work. Only
// real insertion attempts are rationed; the step cap just stops a pathological z-order
// from spinning the UI thread.
constexpr int kZOrderStepLimit = 4096;
constexpr int kZOrderAttemptLimit = 16;

constexpr UINT kZOrderFlags =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING;

[[nodiscard]] bool IsBookmarkStrip(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0) return false;
    return std::wcscmp(cls, L"WindowMark.BookmarkOverlay") == 0;
}

// The unthrottled geometry path is what makes the outline track a drag instead of
// trailing it, but location events arrive far faster than the screen can show them:
// measured at ~110 per second during a fast drag against a 60Hz display, so roughly every
// other SetWindowPos was overwritten before it was ever presented. One move per frame is
// the most anyone can see. The newest frame is always remembered, and the throttled Apply
// puts the outline exactly where it belongs within 33ms of the drag stopping.
// One move per frame at 60Hz. Raising this to 33ms was tried, to halve the number of
// SetWindowPos calls contending for the desktop-wide window lock with the app being
// dragged - but five interleaved rounds could not tell the two settings apart, while the
// cost is certain: at 33ms the outline trails a full extra frame, tens of pixels during a
// fast drag. Unproven gain, certain cost, so it stays at 15.
constexpr double kMinMoveIntervalMs = 15.0;

// GetTickCount64 has ~15.6ms granularity, which is the same size as the interval being
// enforced - it would gate to either 0 or 15.6ms at random. QPC is exact and just as cheap.
[[nodiscard]] double NowMs() {
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return (static_cast<double>(now.QuadPart) * 1000.0) / frequency;
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
    BorderWindow(WinBorderBackend& owner, const BorderModel& model)
        : owner_(owner), model_(model) {}

    ~BorderWindow() { Destroy(); }

    bool Create() {
        const RECT outer = OuterRect();
        hwnd_ = CreateWindowExW(
            // TRANSPARENT is essential: the outline straddles the window edge, and
            // without it every click near an edge would land on us, not the app.
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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
        SyncZOrder();
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
        const bool visibleChanged = model.visible != model_.visible;
        model_ = model;

        Reposition();
        if (visibleChanged) ApplyVisibility();
        // Only the fill colour and the bitmap size affect the pixels; a pure move does not.
        if (sizeChanged || activeChanged || (visibleChanged && model_.visible)) Redraw();
        // Tried gating this on activeChanged || visibleChanged, on the theory that moving a
        // window cannot restack it. The theory is right and the change was still wrong:
        // z-order also drifts from events we never see, and once an outline is stranded
        // nothing brings it back - one Excel window's outline ended up below both its own
        // host and the other Excel window, which is exactly what "the border is
        // incomplete" looks like. The saving was 34ms -> 30ms, inside the noise. Not a
        // trade worth making.
        SyncZOrder();
    }

    // Sit directly above the window being outlined rather than at the top of the desktop.
    // Anchoring to the target keeps the outline at the target's own depth, so it never
    // floats over an unrelated window in front of it. Owner/owned would have been the
    // obvious mechanism, but the target belongs to another process and Windows does not
    // keep cross-process owner z-order in sync - the same trap the bookmark overlays hit
    // before they were restricted to the foreground window.
    void SyncZOrder() {
        if (!hwnd_ || !shown_) return;
        HWND target = HwndFromId(model_.windowId);
        if (!IsWindow(target)) return;

        // Match the target's topmost state before anything else. Windows promotes a
        // window to topmost on its own when it is inserted below a topmost one, which is
        // why an always-on-top app gets a correct outline for free - but it never demotes.
        // Inserting a topmost window below an ordinary one leaves the flag set, so when an
        // app turns always-on-top back off its outline stays stuck in the topmost band,
        // floating over every unrelated window, permanently. Only HWND_NOTOPMOST clears it.
        const bool targetTopmost =
            (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (!targetTopmost &&
            (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, kZOrderFlags);
        }

        // SetWindowPos places the window *after* hWndInsertAfter in z-order, i.e. below
        // it. Passing the target therefore buries the outline underneath the very window
        // it outlines - only the few pixels of overhang stay visible. To sit just above
        // the target, insert below whatever is currently directly above it.
        //
        // "Directly above" has to mean directly above *visibly*. A window's own hidden
        // helpers - the Default IME window every thread gets, MSCTFIME UI - sit right on
        // top of it, and Windows will not slot anything between an owner and a window it
        // owns: that SetWindowPos fails with ERROR_ACCESS_DENIED and the outline silently
        // stays wherever it was. Task Manager is the visible casualty, and it never
        // recovers, because every later attempt hits the same hidden window. Stepping
        // over invisible siblings costs nothing - they paint no pixels, so sitting above
        // them looks identical to sitting below them.
        HWND above = target;
        int attempts = 0;
        for (int step = 0; step < kZOrderStepLimit; ++step) {
            above = GetWindow(above, GW_HWNDPREV);
            if (!above) break;             // target is already at the top of its band
            if (above == hwnd_) return;    // first visible thing above it is us: done
            if (!IsWindowVisible(above)) continue;
            // Never anchor to a window in the topmost band. SetWindowPos promotes whatever
            // it inserts below one, so a single such anchor turns the outline into an
            // always-on-top window floating over unrelated apps - observed, an outline for
            // an ordinary window sitting at z=14 with the topmost bit set. The bookmark
            // strip is deliberately topmost and is the most likely one to be met first.
            if (!targetTopmost && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                break;
            }
            if (SetWindowPos(hwnd_, above, 0, 0, 0, 0, kZOrderFlags)) return;
            // Refused, which means this one is owned by something. Try a couple more, but
            // not many: every extra step climbs past a window the outline should be
            // *below*, and ending up too high is worse than ending up slightly too low.
            if (++attempts >= kZOrderAttemptLimit) break;
        }
        // Nothing to anchor above. For an always-on-top target the outline has to be in
        // that band too, and asking for it is allowed.
        if (targetTopmost) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, kZOrderFlags);
            return;
        }

        // Otherwise the target is the window in front, and nothing can be placed above it
        // from here: HWND_TOP returns TRUE, sets nothing and moves nothing when the
        // request comes from a process that does not own the foreground window.
        //
        // Settle for directly *below* the target, which is allowed - lowering never is
        // restricted. That costs the single pixel of ring that overlaps the frame and
        // keeps everything else: the three pixels outside it, and being above every other
        // window on the desktop. Doing nothing here instead left an outline stranded
        // underneath an unrelated window, which is what "the border is incomplete" looked
        // like on screen.
        SetWindowPos(hwnd_, target, 0, 0, 0, 0, kZOrderFlags);
    }

    // Fast path for the unthrottled geometry sink: move only, never repaint.
    void MoveTo(const Rect& frame) {
        const bool sizeChanged = frame.width() != model_.frame.width() ||
                                 frame.height() != model_.frame.height();
        // Always remember the newest frame, even when the move itself is skipped: whatever
        // runs next - the following event or the throttled Apply - then lands on the real
        // position rather than a stale one.
        model_.frame = frame;

        // A size change has to go through: the bitmap no longer matches the window, and
        // leaving that until the next tick shows a visibly wrong outline.
        if (!sizeChanged) {
            const double now = NowMs();
            if (now - lastMoveMs_ < kMinMoveIntervalMs) return;
            lastMoveMs_ = now;
        } else {
            lastMoveMs_ = NowMs();
        }

        Reposition();
        if (sizeChanged) Redraw();
    }

private:
    // How far the outline extends beyond the window frame. A negative offset pulls it
    // inward over the window, which is the tacky-borders convention and the reason this
    // must not clamp the offset at zero.
    [[nodiscard]] int Reach() const {
        const auto& border = owner_.settings_.border;
        return std::max(0, std::max(1, border.width) + border.offset);
    }

    [[nodiscard]] RECT OuterRect() const {
        const int reach = Reach();
        return RECT{
            model_.frame.left - reach,
            model_.frame.top - reach,
            model_.frame.right + reach,
            model_.frame.bottom + reach,
        };
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

        const auto& border = owner_.settings_.border;
        const float stroke = static_cast<float>(std::max(1, border.width));
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
            ToD2DColor(model_.active ? border.activeColor : border.inactiveColor), &brush);
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
    BorderModel model_;
    HWND hwnd_{};
    RECT outer_{};
    // When the unthrottled path last actually moved this window; see kMinMoveIntervalMs.
    double lastMoveMs_{0.0};
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
    started_ = true;
    return true;
}

void WinBorderBackend::Apply(const std::vector<BorderModel>& models) {
    if (!started_) return;

    std::unordered_set<WindowId> desired;
    desired.reserve(models.size());

    for (const auto& model : models) {
        desired.insert(model.windowId);
        auto it = windows_.find(model.windowId);
        if (it == windows_.end()) {
            auto border = std::make_unique<BorderWindow>(*this, model);
            if (border->Create()) {
                windows_.emplace(model.windowId, std::move(border));
            }
        } else {
            it->second->Update(model);
        }
    }

    for (auto it = windows_.begin(); it != windows_.end();) {
        it = desired.contains(it->first) ? std::next(it) : windows_.erase(it);
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
    windows_.clear();
}

void WinBorderBackend::Stop() noexcept {
    windows_.clear();
    surface_.reset();
    renderTarget_.Reset();
    d2dFactory_.Reset();
    started_ = false;
}

} // namespace windowmark::win
