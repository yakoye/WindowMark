#include "WinOverlay.h"

#include <algorithm>
#include <vector>

namespace windowmark::win {
namespace {

constexpr const wchar_t* kOverlayClass = L"WindowMark.Overlay";

// 遍历 z 序的上限，防止病态 z 序把线程转死。
constexpr int kZOrderLimit = 4096;

bool EnsureClass() {
    static const ATOM atom = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM w, LPARAM l) -> LRESULT {
            // 这层画布只负责显示，任何命中测试都让它穿透到下面的窗口。
            if (msg == WM_NCHITTEST) return HTTRANSPARENT;
            return DefWindowProcW(hwnd, msg, w, l);
        };
        wc.lpszClassName = kOverlayClass;
        return RegisterClassExW(&wc);
    }();
    return atom != 0;
}

// topmost 层里最靠后的那个可见窗口。
//
// overlay 插到它后面就落在两个 band 的交界：比所有普通窗口高，比所有 topmost 窗口低。
// Windows 只有普通层和 topmost 层两档，没有可以插空的数值层级，而这个交界处正是
// 「比谁都高，但比系统 UI 低」唯一能表达的位置——右键菜单、输入法候选框、任务栏、
// 悬浮的会议小窗因而都能压在边框上面。
[[nodiscard]] HWND LastTopmostWindow(HWND self) {
    HWND last = nullptr;
    HWND hwnd = GetTopWindow(nullptr);
    for (int step = 0; step < kZOrderLimit && hwnd != nullptr; ++step) {
        if (hwnd != self && IsWindowVisible(hwnd) != FALSE) {
            if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) break;
            last = hwnd;
        }
        hwnd = GetWindow(hwnd, GW_HWNDNEXT);
    }
    return last;
}

// D2D 工厂全进程一个。只有画圆角时才会用到，但建一次的代价可以忽略，不值得为它
// 再搭一套延迟初始化。
[[nodiscard]] ID2D1Factory* SharedFactory() {
    static Microsoft::WRL::ComPtr<ID2D1Factory> factory = [] {
        Microsoft::WRL::ComPtr<ID2D1Factory> f;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
        return f;
    }();
    return factory.Get();
}

[[nodiscard]] D2D1_COLOR_F ToD2DColor(unsigned argb) {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0F;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0F;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0F;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0F;
    return D2D1::ColorF(r, g, b, a);
}

// ULW_ALPHA 要求源位图的颜色已经乘过 alpha，否则半透明边缘会发白。
[[nodiscard]] unsigned Premultiply(unsigned argb) {
    const unsigned a = (argb >> 24) & 0xFFu;
    if (a == 0xFFu) return argb;
    const unsigned r = ((argb >> 16) & 0xFFu) * a / 255u;
    const unsigned g = ((argb >> 8) & 0xFFu) * a / 255u;
    const unsigned b = (argb & 0xFFu) * a / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

} // namespace

MonitorOverlay::~MonitorOverlay() { Destroy(); }

bool MonitorOverlay::Create(const RECT& monitorRect) {
    if (!EnsureClass()) return false;
    bounds_ = monitorRect;
    const int width = std::max<LONG>(1, bounds_.right - bounds_.left);
    const int height = std::max<LONG>(1, bounds_.bottom - bounds_.top);

    hwnd_ = CreateWindowExW(
        // TRANSPARENT 让点击穿透；NOACTIVATE 让它永远不抢焦点；TOPMOST 在这里给定，
        // 之后再也不用去「提升」。
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST,
        kOverlayClass, L"", WS_POPUP | WS_DISABLED,
        bounds_.left, bounds_.top, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd_ == nullptr) return false;

    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (dc_ == nullptr) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   // 负数 = 自上而下，配 ULW_ALPHA 用
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (bitmap_ == nullptr) return false;
    oldBitmap_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    MoveToBandTail();
    return true;
}

bool MonitorOverlay::EnsureRenderTarget() {
    if (target_) return true;
    ID2D1Factory* factory = SharedFactory();
    if (factory == nullptr) return false;

    // 96 DPI，让一个 DIP 正好是一个物理像素：这里所有矩形都已经是物理坐标了。
    // 位图是预乘 alpha 的（UpdateLayeredWindow 的要求），render target 得跟着声明。
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0F, 96.0F);
    return SUCCEEDED(factory->CreateDCRenderTarget(&props, target_.GetAddressOf()));
}

void MonitorOverlay::MoveToBandTail() {
    if (hwnd_ == nullptr) return;
    // 层内换位，不是「提进层」——窗口已经在 topmost 层里，这一步踩不到那个会卡死的
    // 操作。真挪不动也无所谓：位置照样对，顶多盖住一点系统 UI。
    if (HWND lastTop = LastTopmostWindow(hwnd_); lastTop != nullptr) {
        SetWindowPos(hwnd_, lastTop, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void MonitorOverlay::Render(const std::vector<BorderStroke>& strokes) {
    if (hwnd_ == nullptr || bits_ == nullptr) return;
    const int width = static_cast<int>(bounds_.right - bounds_.left);
    const int height = static_cast<int>(bounds_.bottom - bounds_.top);
    auto* pixels = static_cast<unsigned*>(bits_);

    auto clearRect = [&](const RECT& r) {
        const int x0 = std::max<LONG>(0, r.left);
        const int y0 = std::max<LONG>(0, r.top);
        const int x1 = std::min<LONG>(width, r.right);
        const int y1 = std::min<LONG>(height, r.bottom);
        for (int y = y0; y < y1; ++y) {
            unsigned* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(width);
            std::fill(row + x0, row + x1, 0u);
        }
    };

    // 先擦掉上一帧画过的地方，再画这一帧。两者的并集就是要提交的脏区。
    if (hasLastPainted_) clearRect(lastPainted_);

    RECT painted{};
    bool anyPainted = false;

    // 圆角段先攒着，等像素填充做完再一次性交给 D2D——中途来回切换会让 GDI 和 D2D
    // 对同一块位图的写入次序变得难以推理。
    struct RoundedSeg {
        const BorderStroke* stroke;
        RECT local;
    };
    std::vector<RoundedSeg> rounded;

    for (const BorderStroke& stroke : strokes) {
        const RECT r{stroke.rect.left, stroke.rect.top, stroke.rect.right,
                     stroke.rect.bottom};
        RECT clipped{};
        // 线段是全虚拟桌面坐标，这块画布只画落在自己显示器里的那部分。
        if (IntersectRect(&clipped, &r, &bounds_) == FALSE) continue;

        const int x0 = static_cast<int>(clipped.left - bounds_.left);
        const int y0 = static_cast<int>(clipped.top - bounds_.top);
        const int x1 = static_cast<int>(clipped.right - bounds_.left);
        const int y1 = static_cast<int>(clipped.bottom - bounds_.top);
        // 记的是画布本地坐标，和 prcDirty 的口径一致。
        const RECT local{static_cast<LONG>(x0), static_cast<LONG>(y0),
                         static_cast<LONG>(x1), static_cast<LONG>(y1)};

        if (stroke.radius > 0.0F) {
            rounded.push_back(RoundedSeg{&stroke, local});
        } else {
            // 直角：直接填像素，最快的一条路。
            const unsigned color = Premultiply(stroke.color);
            for (int y = y0; y < y1; ++y) {
                unsigned* row =
                    pixels + static_cast<size_t>(y) * static_cast<size_t>(width);
                std::fill(row + x0, row + x1, color);
            }
        }

        if (anyPainted) {
            UnionRect(&painted, &painted, &local);
        } else {
            painted = local;
            anyPainted = true;
        }
    }

    if (!rounded.empty() && EnsureRenderTarget()) {
        const RECT bind{0, 0, width, height};
        if (SUCCEEDED(target_->BindDC(dc_, &bind))) {
            target_->BeginDraw();
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush);
            if (brush) {
                for (const RoundedSeg& seg : rounded) {
                    const BorderStroke& s = *seg.stroke;
                    brush->SetColor(ToD2DColor(s.color));

                    // 只画这一段：裁剪之外的部分不落笔。曲线是连续的，没法按段分别
                    // 构造路径，所以每段都画一次完整的环，靠裁剪挡掉其余部分。
                    // ALIASED 是必须的——裁剪边界要和像素对齐，否则相邻段的接缝上会
                    // 各留半个像素的半透明，拼起来是一条更淡的缝。
                    target_->PushAxisAlignedClip(
                        D2D1::RectF(static_cast<float>(seg.local.left),
                                    static_cast<float>(seg.local.top),
                                    static_cast<float>(seg.local.right),
                                    static_cast<float>(seg.local.bottom)),
                        D2D1_ANTIALIAS_MODE_ALIASED);

                    // 线沿路径居中，所以路径要从环的外沿往里收半个线宽。
                    const float inset = static_cast<float>(s.strokeWidth) * 0.5F;
                    const D2D1_RECT_F path = D2D1::RectF(
                        static_cast<float>(s.ringOuter.left - bounds_.left) + inset,
                        static_cast<float>(s.ringOuter.top - bounds_.top) + inset,
                        static_cast<float>(s.ringOuter.right - bounds_.left) - inset,
                        static_cast<float>(s.ringOuter.bottom - bounds_.top) - inset);
                    target_->DrawRoundedRectangle(
                        D2D1::RoundedRect(path, s.radius, s.radius), brush.Get(),
                        static_cast<float>(s.strokeWidth));

                    target_->PopAxisAlignedClip();
                }
            }
            if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
            // D2D 写完位图后必须冲一次，否则 UpdateLayeredWindow 可能读到旧内容。
            GdiFlush();
        }
    }

    RECT dirty{};
    bool anyDirty = false;
    if (hasLastPainted_) {
        dirty = lastPainted_;
        anyDirty = true;
    }
    if (anyPainted) {
        if (anyDirty) {
            UnionRect(&dirty, &dirty, &painted);
        } else {
            dirty = painted;
            anyDirty = true;
        }
    }

    lastPainted_ = painted;
    hasLastPainted_ = anyPainted;

    if (!anyDirty) return;   // 上一帧空、这一帧也空，没什么可提交的

    POINT src{0, 0};
    SIZE size{width, height};
    POINT dst{bounds_.left, bounds_.top};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.pptSrc = &src;
    info.psize = &size;
    info.hdcSrc = dc_;
    info.pptDst = &dst;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    // 只提交变化的那一块。整屏提交实测 2.09ms，脏矩形 0.91ms——白拿的三倍余量。
    info.prcDirty = &dirty;
    UpdateLayeredWindowIndirect(hwnd_, &info);
}

void MonitorOverlay::Destroy() noexcept {
    if (dc_ != nullptr && oldBitmap_ != nullptr) SelectObject(dc_, oldBitmap_);
    if (bitmap_ != nullptr) DeleteObject(bitmap_);
    if (dc_ != nullptr) DeleteDC(dc_);
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
    bitmap_ = nullptr;
    oldBitmap_ = nullptr;
    dc_ = nullptr;
    hwnd_ = nullptr;
    bits_ = nullptr;
    hasLastPainted_ = false;
    target_.Reset();
}

void OverlaySet::Sync() {
    std::vector<RECT> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi) != FALSE) {
                reinterpret_cast<std::vector<RECT>*>(param)->push_back(mi.rcMonitor);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    // 布局没变就什么都不做——这个函数每帧都会被调一次，代价必须接近零。
    const bool sameLayout =
        overlays_.size() == monitors.size() &&
        std::equal(overlays_.begin(), overlays_.end(), monitors.begin(),
                   [](const std::unique_ptr<MonitorOverlay>& overlay, const RECT& m) {
                       return EqualRect(&overlay->Bounds(), &m) != FALSE;
                   });
    if (sameLayout) return;

    overlays_.clear();
    for (const RECT& m : monitors) {
        auto overlay = std::make_unique<MonitorOverlay>();
        if (overlay->Create(m)) overlays_.push_back(std::move(overlay));
    }
}

void OverlaySet::Render(const std::vector<BorderStroke>& strokes) {
    for (auto& overlay : overlays_) overlay->Render(strokes);
}

void OverlaySet::Destroy() noexcept { overlays_.clear(); }

} // namespace windowmark::win
