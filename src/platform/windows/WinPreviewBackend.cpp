#include "WinPreviewBackend.h"

#include "WinUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace windowmark::win {
namespace {

constexpr wchar_t kPreviewClass[] = L"WindowMark.WindowPreview";

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

int ClampOrigin(int desired, int extent, int minValue, int maxValue) {
    if (extent >= maxValue - minValue) return minValue;
    return std::clamp(desired, minValue, maxValue - extent);
}

} // namespace

WinPreviewBackend::WinPreviewBackend() = default;
WinPreviewBackend::~WinPreviewBackend() { Stop(); }

bool WinPreviewBackend::Start(const PreviewSettings& settings) {
    settings_ = settings;
    started_ = true;
    return true;
}

bool WinPreviewBackend::EnsureWindow() {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kPreviewClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kPreviewClass,
        L"",
        WS_POPUP,
        0, 0,
        settings_.width,
        settings_.height,
        nullptr,
        nullptr,
        wc.hInstance,
        this);
    return hwnd_ != nullptr;
}

void WinPreviewBackend::Show(const PreviewRequest& request) {
    if (!started_ || !settings_.enabled) return;
    if (!EnsureWindow()) return;

    HWND source = HwndFromId(request.sourceWindowId);
    HWND host = HwndFromId(request.hostWindowId);
    if (!IsWindow(source) || !IsWindow(host)) {
        Hide();
        return;
    }

    if (thumbnail_) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }

    const RECT previewRect = ComputePreviewRect(request);
    const int width = previewRect.right - previewRect.left;
    const int height = previewRect.bottom - previewRect.top;

    // Topmost, and deliberately not owned by the host window. Making it an owned popup
    // of a window in another process is exactly the arrangement that left the bookmark
    // overlays stuck behind unrelated windows - Windows does not keep cross-process
    // owner/owned z-order in sync. A preview is transient and only ever appears because
    // the user is pointing at a bookmark, so topmost is the honest way to say "above
    // everything, briefly"; Hide() takes it away again.
    SetWindowPos(
        hwnd_, HWND_TOPMOST,
        previewRect.left,
        previewRect.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const int diameter = std::max(0, settings_.cornerRadius * 2);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region) SetWindowRgn(hwnd_, region, TRUE);

    if (FAILED(DwmRegisterThumbnail(hwnd_, source, &thumbnail_))) {
        thumbnail_ = nullptr;
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    UpdateThumbnail(request);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void WinPreviewBackend::UpdateThumbnail(const PreviewRequest&) {
    if (!thumbnail_ || !hwnd_) return;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int pad = 8;
    const int clientW = static_cast<int>(client.right - client.left);
    const int clientH = static_cast<int>(client.bottom - client.top);
    const int availableW = std::max(1, clientW - pad * 2);
    const int availableH = std::max(1, clientH - pad * 2);

    SIZE source{};
    if (FAILED(DwmQueryThumbnailSourceSize(thumbnail_, &source)) || source.cx <= 0 || source.cy <= 0) {
        source = SIZE{availableW, availableH};
    }

    const double scale = std::min(
        static_cast<double>(availableW) / static_cast<double>(source.cx),
        static_cast<double>(availableH) / static_cast<double>(source.cy));
    const int drawW = std::max(1, static_cast<int>(std::lround(source.cx * scale)));
    const int drawH = std::max(1, static_cast<int>(std::lround(source.cy * scale)));
    const int left = pad + (availableW - drawW) / 2;
    const int top = pad + (availableH - drawH) / 2;

    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY;
    props.fVisible = TRUE;
    props.opacity = 255;
    props.fSourceClientAreaOnly = FALSE;
    props.rcDestination = RECT{left, top, left + drawW, top + drawH};
    DwmUpdateThumbnailProperties(thumbnail_, &props);
}

RECT WinPreviewBackend::ComputePreviewRect(const PreviewRequest& request) const {
    const int width = settings_.width;
    const int height = settings_.height;
    const int gap = 12;
    int x = request.anchorScreenRect.left;
    int y = request.anchorScreenRect.top;

    switch (request.placement) {
    case Placement::Left:
        x = request.anchorScreenRect.left - width - gap;
        if (x < request.workArea.left) x = request.hostFrame.left + gap;
        y = request.anchorScreenRect.top - (height - request.anchorScreenRect.height()) / 2;
        break;
    case Placement::Right:
        x = request.anchorScreenRect.right + gap;
        if (x + width > request.workArea.right) x = request.hostFrame.right - width - gap;
        y = request.anchorScreenRect.top - (height - request.anchorScreenRect.height()) / 2;
        break;
    case Placement::Top:
        x = request.anchorScreenRect.left + (request.anchorScreenRect.width() - width) / 2;
        y = request.anchorScreenRect.top - height - gap;
        if (y < request.workArea.top) y = request.hostFrame.top + gap;
        break;
    case Placement::Bottom:
    case Placement::Auto:
    default:
        x = request.anchorScreenRect.left + (request.anchorScreenRect.width() - width) / 2;
        y = request.anchorScreenRect.top - height - gap;
        if (y < request.workArea.top) y = request.hostFrame.bottom - height - gap;
        break;
    }

    x = ClampOrigin(x, width, request.workArea.left, request.workArea.right);
    y = ClampOrigin(y, height, request.workArea.top, request.workArea.bottom);
    return RECT{x, y, x + width, y + height};
}

void WinPreviewBackend::UpdateSettings(const PreviewSettings& settings) {
    settings_ = settings;
    // Width, height and corner radius are baked into the window when it is created, so
    // drop it and let the next Show() rebuild it with the new numbers.
    Hide();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void WinPreviewBackend::Hide() noexcept {
    if (thumbnail_) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void WinPreviewBackend::Stop() noexcept {
    Hide();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    started_ = false;
}

LRESULT CALLBACK WinPreviewBackend::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(246, 247, 249));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace windowmark::win
