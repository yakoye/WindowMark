#include "WinPreviewBackend.h"

#include "WinUtil.h"
#include "windowmark/core/PreviewStack.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace windowmark::win {
namespace {

constexpr wchar_t kThumbClass[] = L"WindowMark.WindowPreview";
constexpr wchar_t kTitleClass[] = L"WindowMark.PreviewTitle";

// 交叉轴放不下时缩略图按比例缩小；缩到比这还小就不显示——再小认不出是哪个窗口，而且绝不能
// 为了塞下它去压标题或书签。
constexpr float kMinThumbWidth = 120.0F;
constexpr float kMinThumbHeight = 80.0F;
constexpr int kThumbPad = 8;

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

[[nodiscard]] bool IsSide(Placement placement) {
    return placement == Placement::Left || placement == Placement::Right;
}

// 按预览栈所在的显示器问缩放，而不是按宿主窗口问：宿主要是不感知 DPI 的老程序，
// GetDpiForWindow 对它永远报 96，标题就小一号。
[[nodiscard]] float ScaleFor(const Rect& workArea) {
    const RECT r{workArea.left, workArea.top, workArea.right, workArea.bottom};
    HMONITOR monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
        dpiX > 0) {
        return static_cast<float>(dpiX) / 96.0F;
    }
    return 1.0F;
}

[[nodiscard]] RECT ToRect(const RectF& r) {
    return RECT{static_cast<LONG>(std::lround(r.left)), static_cast<LONG>(std::lround(r.top)),
                static_cast<LONG>(std::lround(r.right)), static_cast<LONG>(std::lround(r.bottom))};
}

} // namespace

WinPreviewBackend::WinPreviewBackend() = default;
WinPreviewBackend::~WinPreviewBackend() { Stop(); }

bool WinPreviewBackend::Start(const PreviewSettings& settings) {
    settings_ = settings;
    started_ = true;
    return true;
}

bool WinPreviewBackend::EnsureWindows() {
    if (titleHwnd_ && thumbHwnd_) return true;
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW thumbClass{};
    thumbClass.cbSize = sizeof(thumbClass);
    thumbClass.hInstance = instance;
    thumbClass.lpfnWndProc = ThumbProc;
    thumbClass.lpszClassName = kThumbClass;
    thumbClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassExW(&thumbClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    WNDCLASSEXW titleClass = thumbClass;
    titleClass.lpfnWndProc = TitleProc;
    titleClass.lpszClassName = kTitleClass;
    if (RegisterClassExW(&titleClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Topmost, and deliberately not owned by the host window: Windows does not keep
    // cross-process owner/owned z-order in sync, which is what once left the bookmark
    // strips stuck behind unrelated windows. Both only exist while the pointer is on a
    // bookmark, so "above everything, briefly" is the honest arrangement.
    if (!thumbHwnd_) {
        thumbHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                     kThumbClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                     instance, this);
    }
    // 标题是逐像素 alpha 的分层窗口（圆角底要抗锯齿，还要整体淡入淡出），并且点不中。
    if (!titleHwnd_) {
        titleHwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                WS_EX_TOPMOST,
            kTitleClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    }
    return titleHwnd_ && thumbHwnd_;
}

bool WinPreviewBackend::EnsureDrawing() {
    if (!d2dFactory_ &&
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
        return false;
    }
    if (!dwriteFactory_ &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
        return false;
    }
    if (!renderTarget_) {
        // 96 DPI：和书签条一样，所有尺寸已经是物理像素。
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F, 96.0F);
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, &renderTarget_))) {
            return false;
        }
    }
    if (!title_) title_ = std::make_unique<PreviewTitle>(dwriteFactory_.Get());
    return true;
}

void WinPreviewBackend::Show(const PreviewRequest& request) {
    if (!started_) return;
    if (request.layers.empty() || request.opacity <= 0.0F) {
        Hide();
        return;
    }
    if (!EnsureWindows() || !EnsureDrawing()) return;

    const bool side = IsSide(request.placement);
    scale_ = ScaleFor(request.workArea);
    title_->SetScale(scale_);

    // 标题的长度：各层按不透明度加权。主标签换人时，底从旧标题的长度连续变到新标题的长度，
    // 和位置的滑动同步，不会一帧跳过去。横排不超过缩略图宽，竖排不超过缩略图高。
    const float maxMain = static_cast<float>(side ? settings_.height : settings_.width);
    std::vector<PreviewTitle::Layer> layers;
    layers.reserve(request.layers.size());
    float weighted = 0.0F;
    float weights = 0.0F;
    for (const PreviewLayer& layer : request.layers) {
        PreviewTitle::Layer drawn{Utf8ToWide(layer.title), layer.opacity};
        const float length = std::min(title_->NaturalLength(drawn.text), maxMain);
        weighted += length * layer.opacity;
        weights += layer.opacity;
        layers.push_back(std::move(drawn));
    }
    const float titleMain = weights > 0.0F
        ? weighted / weights
        : std::min(title_->NaturalLength(layers.back().text), maxMain);

    // 缩略图的位置：preview.enabled 关掉、或者首次延迟还没过，就不要。书签指向宿主自己时
    // 这个位置照样占着，改放「当前窗口」字样——从别的书签滑到自己身上时版面不跳。
    bool wantsThumb = false;
    if (settings_.enabled && request.thumbnailArmed) {
        for (const PreviewLayer& layer : request.layers) {
            if (layer.opacity > 0.0F) wantsThumb = true;
        }
    }

    PreviewStackInput in;
    in.placement = request.placement;
    in.workArea = RectF{static_cast<float>(request.workArea.left),
                        static_cast<float>(request.workArea.top),
                        static_cast<float>(request.workArea.right),
                        static_cast<float>(request.workArea.bottom)};
    in.rootEdge = request.rootEdge;
    in.tabs = request.tabs;
    in.anchorMain = request.anchorMain;
    in.titleMain = titleMain;
    in.titleCross = title_->Thickness();
    // 缩略图保持它横放的样子：侧边时主轴（竖直）方向是它的高。
    in.thumbnailMain =
        wantsThumb ? static_cast<float>(side ? settings_.height : settings_.width) : 0.0F;
    in.thumbnailCross = static_cast<float>(side ? settings_.width : settings_.height);
    in.titleGap = static_cast<float>(settings_.titleGap);
    in.thumbnailGap = static_cast<float>(settings_.thumbnailGap);
    in.minThumbnailMain = side ? kMinThumbHeight : kMinThumbWidth;
    in.minThumbnailCross = side ? kMinThumbWidth : kMinThumbHeight;
    const PreviewStackLayout stack = LayoutPreviewStack(in);

    PresentTitle(stack.title, side, request.opacity, layers);
    if (stack.thumbnailVisible) {
        PresentThumbnails(stack.thumbnail, request);
    } else {
        HideThumbnails();
    }
}

void WinPreviewBackend::PresentTitle(const RectF& rectF, bool vertical, float opacity,
                                     const std::vector<PreviewTitle::Layer>& layers) {
    const RECT r = ToRect(rectF);
    const int width = std::max<LONG>(1, r.right - r.left);
    const int height = std::max<LONG>(1, r.bottom - r.top);
    if (!titleSurface_.Ensure(width, height)) return;

    const RECT bind{0, 0, width, height};
    if (FAILED(renderTarget_->BindDC(titleSurface_.dc(), &bind))) return;
    renderTarget_->BeginDraw();
    // 分层窗口的逐像素 alpha 配不了 ClearType 的彩色边缘，统一用灰度抗锯齿。
    renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    renderTarget_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
    title_->Draw(*renderTarget_.Get(),
                 D2D1::RectF(0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)),
                 vertical, layers);
    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderTarget_.Reset();
        return;
    }
    if (FAILED(hr)) return;
    GdiFlush();

    POINT destination{r.left, r.top};
    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha =
        static_cast<BYTE>(std::clamp(std::lround(opacity * 255.0F), 0L, 255L));
    blend.AlphaFormat = AC_SRC_ALPHA;
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(titleHwnd_, screen, &destination, &size, titleSurface_.dc(), &source, 0,
                        &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);

    if (!titleShown_) {
        SetWindowPos(titleHwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        titleShown_ = true;
    }
}

void WinPreviewBackend::PresentThumbnails(const RectF& rectF, const PreviewRequest& request) {
    // 每一层一个 DWM thumbnail：切换时新旧两个同时注册，按各自那层的不透明度交叉淡入淡出，
    // 淡完的那层从请求里消失，这里就注销它。
    const auto wanted = [&](WindowId source) {
        return std::any_of(request.layers.begin(), request.layers.end(),
                           [&](const PreviewLayer& layer) {
                               return layer.thumbnail && layer.sourceWindowId == source;
                           });
    };
    for (auto it = thumbs_.begin(); it != thumbs_.end();) {
        if (wanted(it->source)) {
            ++it;
        } else {
            DwmUnregisterThumbnail(it->handle);
            it = thumbs_.erase(it);
        }
    }
    for (const PreviewLayer& layer : request.layers) {
        if (!layer.thumbnail) continue;
        const bool known = std::any_of(thumbs_.begin(), thumbs_.end(), [&](const Thumb& t) {
            return t.source == layer.sourceWindowId;
        });
        if (known) continue;
        HWND source = HwndFromId(layer.sourceWindowId);
        if (!IsWindow(source)) continue;
        HTHUMBNAIL handle{};
        if (SUCCEEDED(DwmRegisterThumbnail(thumbHwnd_, source, &handle))) {
            thumbs_.push_back(Thumb{layer.sourceWindowId, handle});
        }
    }
    // 指向宿主自己的那几层没有缩略图，换成「当前窗口」字样，不透明度取它们里最大的。
    float selfCard = 0.0F;
    for (const PreviewLayer& layer : request.layers) {
        if (!layer.thumbnail) selfCard = std::max(selfCard, layer.opacity);
    }
    if (thumbs_.empty() && selfCard <= 0.0F) {
        HideThumbnails();
        return;
    }

    const RECT r = ToRect(rectF);
    const int width = std::max<LONG>(1, r.right - r.left);
    const int height = std::max<LONG>(1, r.bottom - r.top);
    const bool resized = width != thumbRect_.right - thumbRect_.left ||
                         height != thumbRect_.bottom - thumbRect_.top;
    const bool appearing = !thumbShown_;
    // 字样画在窗口自己的内容里，先定下这一帧的深浅，窗口出现时第一次绘制就是对的。
    const bool cardChanged = std::fabs(selfCard - selfCard_) >= 0.004F;
    selfCard_ = selfCard;
    if (!thumbShown_ || !EqualRect(&r, &thumbRect_)) {
        SetWindowPos(thumbHwnd_, HWND_TOPMOST, r.left, r.top, width, height,
                     SWP_NOACTIVATE | (thumbShown_ ? SWP_NOZORDER : SWP_SHOWWINDOW));
        if (resized || !thumbShown_) {
            const int diameter = std::max(0, settings_.cornerRadius * 2);
            HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
            if (region) SetWindowRgn(thumbHwnd_, region, TRUE);
        }
        thumbRect_ = r;
        thumbShown_ = true;
    }
    // DWM 缩略图叠在窗口内容上面，所以切换时是缩略图盖过字样、或者字样从淡出的缩略图下面
    // 露出来——和两张缩略图之间的交叉淡入淡出是同一个效果。
    if (cardChanged || resized || appearing) {
        InvalidateRect(thumbHwnd_, nullptr, FALSE);
        UpdateWindow(thumbHwnd_);
    }

    const int availableW = std::max(1, width - kThumbPad * 2);
    const int availableH = std::max(1, height - kThumbPad * 2);
    for (const Thumb& thumb : thumbs_) {
        float opacity = 0.0F;
        for (const PreviewLayer& layer : request.layers) {
            if (layer.thumbnail && layer.sourceWindowId == thumb.source) opacity = layer.opacity;
        }
        SIZE source{};
        if (FAILED(DwmQueryThumbnailSourceSize(thumb.handle, &source)) || source.cx <= 0 ||
            source.cy <= 0) {
            source = SIZE{availableW, availableH};
        }
        const double scale = std::min(static_cast<double>(availableW) / source.cx,
                                      static_cast<double>(availableH) / source.cy);
        const int drawW = std::max(1, static_cast<int>(std::lround(source.cx * scale)));
        const int drawH = std::max(1, static_cast<int>(std::lround(source.cy * scale)));
        const int left = kThumbPad + (availableW - drawW) / 2;
        const int top = kThumbPad + (availableH - drawH) / 2;

        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_RECTDESTINATION |
                        DWM_TNP_SOURCECLIENTAREAONLY;
        props.fVisible = opacity > 0.0F ? TRUE : FALSE;
        props.opacity = static_cast<BYTE>(std::clamp(std::lround(opacity * 255.0F), 0L, 255L));
        props.fSourceClientAreaOnly = FALSE;
        props.rcDestination = RECT{left, top, left + drawW, top + drawH};
        DwmUpdateThumbnailProperties(thumb.handle, &props);
    }
}

void WinPreviewBackend::HideTitle() noexcept {
    if (titleHwnd_ && titleShown_) ShowWindow(titleHwnd_, SW_HIDE);
    titleShown_ = false;
}

void WinPreviewBackend::HideThumbnails() noexcept {
    for (const Thumb& thumb : thumbs_) DwmUnregisterThumbnail(thumb.handle);
    thumbs_.clear();
    if (thumbHwnd_ && thumbShown_) ShowWindow(thumbHwnd_, SW_HIDE);
    thumbShown_ = false;
    thumbRect_ = RECT{};
    selfCard_ = 0.0F;
}

void WinPreviewBackend::PaintThumbWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    // 先画到内存里再一次贴上去：交叉淡入淡出时每帧都重画，直接画在窗口上会闪。
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, std::max(1, width), std::max(1, height))
                            : nullptr;
    HDC target = bitmap ? memory : dc;
    HGDIOBJ previousBitmap = bitmap ? SelectObject(memory, bitmap) : nullptr;

    constexpr COLORREF kBackground = RGB(246, 247, 249);
    HBRUSH brush = CreateSolidBrush(kBackground);
    FillRect(target, &rc, brush);
    DeleteObject(brush);

    if (selfCard_ > 0.0F) {
        const int px = static_cast<int>(std::lround(34.0F * scale_));
        if (!cardFont_ || cardFontPx_ != px) {
            if (cardFont_) DeleteObject(cardFont_);
            cardFont_ = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            cardFontPx_ = px;
        }
        // 淡入淡出：字的颜色从底色往深灰按不透明度插值。
        const float a = std::clamp(selfCard_, 0.0F, 1.0F);
        const auto mix = [a](int from, int to) {
            return static_cast<BYTE>(std::lround(static_cast<float>(from) +
                                                 (static_cast<float>(to) - from) * a));
        };
        SetTextColor(target, RGB(mix(246, 70), mix(247, 74), mix(249, 82)));
        SetBkMode(target, TRANSPARENT);
        HGDIOBJ previousFont = cardFont_ ? SelectObject(target, cardFont_) : nullptr;
        RECT textRect = rc;
        DrawTextW(target, L"当前窗口", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (previousFont) SelectObject(target, previousFont);
    }

    if (bitmap) {
        BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, previousBitmap);
        DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

void WinPreviewBackend::UpdateSettings(const PreviewSettings& settings) {
    settings_ = settings;
    Hide();
}

void WinPreviewBackend::Hide() noexcept {
    HideTitle();
    HideThumbnails();
}

void WinPreviewBackend::Stop() noexcept {
    Hide();
    if (titleHwnd_) {
        DestroyWindow(titleHwnd_);
        titleHwnd_ = nullptr;
    }
    if (thumbHwnd_) {
        DestroyWindow(thumbHwnd_);
        thumbHwnd_ = nullptr;
    }
    titleSurface_.Reset();
    title_.reset();
    if (cardFont_) {
        DeleteObject(cardFont_);
        cardFont_ = nullptr;
        cardFontPx_ = 0;
    }
    renderTarget_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    started_ = false;
}

LRESULT CALLBACK WinPreviewBackend::TitleProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
        // 内容走 UpdateLayeredWindow，这里只清掉更新区域。
        ValidateRect(hwnd, nullptr);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WinPreviewBackend::ThumbProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        auto* self = reinterpret_cast<WinPreviewBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            self->PaintThumbWindow(hwnd);
        } else {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace windowmark::win
