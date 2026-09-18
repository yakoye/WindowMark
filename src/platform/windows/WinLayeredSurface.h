#pragma once

#include <windows.h>

namespace windowmark::win {

// Backing store for UpdateLayeredWindow: a top-down 32bpp DIB that Direct2D renders
// into with premultiplied alpha, which is exactly the format ULW_ALPHA expects.
// 书签条和浮动标题共用。
class LayeredSurface {
public:
    LayeredSurface() = default;
    LayeredSurface(const LayeredSurface&) = delete;
    LayeredSurface& operator=(const LayeredSurface&) = delete;
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

} // namespace windowmark::win
