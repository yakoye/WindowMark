#include "WinDonate.h"

#include "Resource.h"

#include <shellscalingapi.h>
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace windowmark::win {
namespace {

constexpr wchar_t kDonateClass[] = L"WindowMark.Donate";

// 96dpi 下的逻辑像素。
constexpr int kPad = 18;
// 收款码是竖图（上面一截平台标识、中间二维码、下面一行昵称），所以展示框也是竖的：
// 塞进方框的话宽度白白浪费，二维码本身反而被缩小——缩小到一定程度就扫不出来了。
constexpr int kCodeW = 300;
constexpr int kCodeH = 405;
constexpr int kGap = 18;
constexpr int kCaption = 20;
constexpr int kTitle = 24;
constexpr int kRowH = 26;

struct Code {
    int resourceId;
    const wchar_t* label;
};

constexpr std::array<Code, 2> kCodes{{
    {IDR_DONATE_WECHAT, L"微信"},
    {IDR_DONATE_ALIPAY, L"支付宝"},
}};

// 一张合图（两个码并排）时用的框：横版，不用标签——图里自带。
constexpr int kBothW = 520;
constexpr int kBothH = 300;

// 一张解好的图：32 位 BGRA，自上而下。
struct Bitmap {
    int width{};
    int height{};
    std::vector<unsigned> pixels;
    [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// 从 RCDATA 里把 PNG 解出来。用 WIC 而不是先转成 BMP 塞进资源：二维码是张彩图，24 位 BMP
// 要几百 KB，而整个 exe 才九百 KB；PNG 原样编进来是几十 KB。WIC 是系统自带的，不多一个依赖。
// 解码之后先缩一道的目标长边。260x350 的框在 200% 缩放下最多要 700px，600 够用而又不至于
// 让位图太大；最后那一点差距交给 GDI，差得少就看不出来。
constexpr UINT kDecodeLongSide = 600;

[[nodiscard]] Bitmap LoadPngResource(int id) {
    Bitmap out;
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC found = FindResourceW(self, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) return out;
    const DWORD size = SizeofResource(self, found);
    HGLOBAL handle = LoadResource(self, found);
    if (size == 0 || handle == nullptr) return out;
    const void* data = LockResource(handle);
    if (data == nullptr) return out;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return out;
    }

    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICBitmapSource* source = nullptr;
    do {
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(
                static_cast<BYTE*>(const_cast<void*>(data)), size))) {
            break;
        }
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr,
                                                    WICDecodeMetadataCacheOnLoad, &decoder))) {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) {
            break;
        }
        UINT w = 0;
        UINT h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) break;

        source = converter;
        source->AddRef();
        const UINT longSide = std::max(w, h);
        if (longSide > kDecodeLongSide) {
            // Fant 是带滤波的缩放。二维码缩小时用最近邻会丢模块、用抖动会糊边，两种都可能
            // 扫不出来；这一步在真正变小之前把灰阶算对。
            const double k = static_cast<double>(kDecodeLongSide) / longSide;
            const UINT tw = std::max<UINT>(1, static_cast<UINT>(w * k));
            const UINT th = std::max<UINT>(1, static_cast<UINT>(h * k));
            if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(converter, tw, th,
                                             WICBitmapInterpolationModeFant))) {
                source->Release();
                source = scaler;
                source->AddRef();
                w = tw;
                h = th;
            }
        }

        out.width = static_cast<int>(w);
        out.height = static_cast<int>(h);
        out.pixels.resize(static_cast<size_t>(w) * h);
        const UINT stride = w * 4;
        if (FAILED(source->CopyPixels(nullptr, stride, stride * h,
                                      reinterpret_cast<BYTE*>(out.pixels.data())))) {
            out = Bitmap{};
        }
    } while (false);

    if (source) source->Release();
    if (scaler) scaler->Release();
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return out;
}

// 画一颗心：两个圆加一个尖。先按 4 倍大小画成「白底黑字」那样的覆盖图，再 4x4 取平均当
// alpha——GDI 的填充没有抗锯齿，16px 直接画出来是锯齿块，降采样这一步就是在补抗锯齿。
void FillHeart(HDC dc, int side) {
    const int w = side;
    const int h = side;
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

    // 两个圆：左右各一个，圆心在上三分之一处。
    const int r = w * 28 / 100;
    const int cy = h * 34 / 100;
    Ellipse(dc, w / 2 - 2 * r, cy - r, w / 2, cy + r);
    Ellipse(dc, w / 2, cy - r, w / 2 + 2 * r, cy + r);
    // 下面的尖：从两圆外侧收到底部中点。
    const POINT tip[] = {
        {w / 2 - 2 * r, cy},
        {w / 2 + 2 * r, cy},
        {w / 2, h * 94 / 100},
    };
    Polygon(dc, tip, static_cast<int>(std::size(tip)));

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

class Donate {
public:
    void Run(HWND owner) {
        // 合图优先：放了 res\donate.png 就只显示它，省得一张图里已经有两个码还再摆两遍。
        both_ = LoadPngResource(IDR_DONATE_BOTH);
        if (!both_.valid()) {
            for (size_t i = 0; i < kCodes.size(); ++i) {
                images_[i] = LoadPngResource(kCodes[i].resourceId);
                if (images_[i].valid()) ++count_;
            }
        }
        if (!both_.valid() && count_ == 0) return;

        owner_ = owner;
        if (!Register() || !Create()) return;

        const bool ownerWasEnabled = owner_ && IsWindowEnabled(owner_);
        if (ownerWasEnabled) EnableWindow(owner_, FALSE);
        MSG msg{};
        while (hwnd_) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (hwnd_ && IsDialogMessageW(hwnd_, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ownerWasEnabled) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Donate*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Donate*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            Paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL || LOWORD(wParam) == IDOK) DestroyWindow(hwnd_);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    [[nodiscard]] int Scale(int v) const { return MulDiv(v, dpi_, 96); }

    bool Register() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kDonateClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        wc.hIcon = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool Create() {
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
            dpi_ = static_cast<int>(dpiX);
        }

        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                       static_cast<UINT>(dpi_))) {
            font_ = CreateFontIndirectW(&ncm.lfMessageFont);
            ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 12, 10);
            ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
            titleFont_ = CreateFontIndirectW(&ncm.lfMessageFont);
        }

        const int boxW = both_.valid() ? kBothW : kCodeW * count_ + kGap * (count_ - 1);
        const int boxH = both_.valid() ? kBothH : kCodeH + kCaption;
        const int width = kPad * 2 + boxW;
        const int height = kPad + kTitle + kGap + boxH + kGap + kRowH + kPad;
        RECT bounds{0, 0, Scale(width), Scale(height)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_DLGMODALFRAME);
        const int outerW = bounds.right - bounds.left;
        const int outerH = bounds.bottom - bounds.top;
        int x = static_cast<int>(work.left) +
                ((static_cast<int>(work.right - work.left)) - outerW) / 2;
        int y = static_cast<int>(work.top) +
                ((static_cast<int>(work.bottom - work.top)) - outerH) / 2;
        x = std::clamp(x, static_cast<int>(work.left),
                       std::max<int>(static_cast<int>(work.left),
                                     static_cast<int>(work.right) - outerW));
        y = std::clamp(y, static_cast<int>(work.top),
                       std::max<int>(static_cast<int>(work.top),
                                     static_cast<int>(work.bottom) - outerH));

        // owner 要传进去：关于框是 TaskDialog 而且被设成了 topmost，不认 owner 的话这个窗口
        // 就开在它**后面**——实测抓图看到的正是「只露出标题栏和底下一条」。有 owner 的窗口
        // 永远排在 owner 上面，不用再去碰 topmost。
        if (!CreateWindowExW(WS_EX_DLGMODALFRAME, kDonateClass, L"赞赏作者",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, outerW, outerH,
                             owner_, nullptr, GetModuleHandleW(nullptr), this)) {
            return false;
        }
        HWND close = CreateWindowExW(
            0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            Scale(width - kPad - 92), Scale(height - kPad - kRowH), Scale(92), Scale(kRowH),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            GetModuleHandleW(nullptr), nullptr);
        if (close && font_) SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return true;
    }

    void Paint(HDC dc) const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

        HGDIOBJ old = SelectObject(dc, titleFont_ ? titleFont_ : font_);
        RECT line{Scale(kPad), Scale(kPad), client.right - Scale(kPad), Scale(kPad + kTitle)};
        DrawTextW(dc, L"WindowMark 是免费的。觉得不错的话，赞赏一下作者", -1, &line,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(dc, font_);

        int x = Scale(kPad);
        const int top = Scale(kPad + kTitle + kGap);
        if (both_.valid()) {
            DrawImage(dc, both_, x, top, Scale(kBothW), Scale(kBothH));
            SelectObject(dc, old);
            return;
        }
        for (size_t i = 0; i < kCodes.size(); ++i) {
            if (!images_[i].valid()) continue;
            DrawImage(dc, images_[i], x, top, Scale(kCodeW), Scale(kCodeH));
            RECT caption{x, top + Scale(kCodeH), x + Scale(kCodeW),
                         top + Scale(kCodeH + kCaption)};
            DrawTextW(dc, kCodes[i].label, -1, &caption,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            x += Scale(kCodeW + kGap);
        }
        SelectObject(dc, old);
    }

    // 等比缩放塞进 boxW x boxH 里，居中。二维码一拉伸就可能扫不出来，所以不拉伸。
    void DrawImage(HDC dc, const Bitmap& image, int x, int y, int boxW, int boxH) const {
        const double scale =
            std::min(static_cast<double>(boxW) / image.width,
                     static_cast<double>(boxH) / image.height);
        const int w = std::max(1, static_cast<int>(image.width * scale));
        const int h = std::max(1, static_cast<int>(image.height * scale));
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = image.width;
        bi.bmiHeader.biHeight = -image.height;   // 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, x + (boxW - w) / 2, y + (boxH - h) / 2, w, h, 0, 0, image.width,
                      image.height, image.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }

    HWND owner_{};
    HWND hwnd_{};
    HFONT font_{};
    HFONT titleFont_{};
    int dpi_{96};
    int count_{0};
    Bitmap both_{};
    std::array<Bitmap, 2> images_{};
};

} // namespace

bool HasDonateCodes() {
    HMODULE self = GetModuleHandleW(nullptr);
    if (FindResourceW(self, MAKEINTRESOURCEW(IDR_DONATE_BOTH), RT_RCDATA) != nullptr) {
        return true;
    }
    for (const Code& code : kCodes) {
        if (FindResourceW(self, MAKEINTRESOURCEW(code.resourceId), RT_RCDATA) != nullptr) {
            return true;
        }
    }
    return false;
}

void ShowDonateWindow(HWND owner) {
    Donate donate;
    donate.Run(owner);
}

HICON CreateHeartIcon(int size) {
    if (size <= 0) return nullptr;
    constexpr int kSuper = 4;             // 超采样倍数
    const int big = size * kSuper;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = big;
    bi.bmiHeader.biHeight = -big;          // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bigBits = nullptr;
    HBITMAP bigBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bigBits, nullptr, 0);
    if (!bigBmp) {
        DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bigBmp);
    std::memset(bigBits, 0, static_cast<size_t>(big) * big * 4);   // 黑底
    FillHeart(dc, big);
    GdiFlush();

    // 降采样：白的地方就是心，取平均当 alpha；颜色固定成红。
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    void* smallBits = nullptr;
    HBITMAP colour = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &smallBits, nullptr, 0);
    if (!colour) {
        SelectObject(dc, oldBmp);
        DeleteObject(bigBmp);
        DeleteDC(dc);
        return nullptr;
    }
    const auto* src = static_cast<const unsigned char*>(bigBits);
    auto* dst = static_cast<unsigned*>(smallBits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            unsigned sum = 0;
            for (int sy = 0; sy < kSuper; ++sy) {
                for (int sx = 0; sx < kSuper; ++sx) {
                    const size_t o =
                        (static_cast<size_t>(y * kSuper + sy) * big + (x * kSuper + sx)) * 4;
                    sum += src[o];   // 蓝通道就够：画的是纯白
                }
            }
            const unsigned alpha = sum / (kSuper * kSuper);
            // 图标用的是直通 alpha（非预乘）。颜色挑得比正红深一点，小尺寸下更稳。
            dst[static_cast<size_t>(y) * size + x] =
                (alpha << 24) | (0xD4u << 16) | (0x27u << 8) | 0x3Du;
        }
    }

    SelectObject(dc, oldBmp);
    DeleteObject(bigBmp);
    DeleteDC(dc);

    // 32 位图标也必须带掩码位图，哪怕内容被 alpha 完全覆盖。
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

} // namespace windowmark::win
