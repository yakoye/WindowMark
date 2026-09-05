// Overlay 方案的性能关卡：一个覆盖整屏的分层窗口，反复只更新四条细带，量每次提交
// 要多久。
//
// 要回答的问题只有一个：用 UpdateLayeredWindowIndirect 的脏矩形，把一圈边框的变化
// 提交上去，能不能压到和现在「一次 SetWindowPos 移动一个小窗口」同量级（中位数
// 3.0ms / p99 15.7ms / 一帧内 100%）。压不到，Overlay 方案就得换渲染策略——那时候
// 代码还没铺开，改起来便宜。
// windows.h 会定义 min / max 宏，把 std::min(...) 展开成 std::(...) 这种鬼东西。
// 在包含它之前关掉。
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

LARGE_INTEGER g_freq{};

double NowMs() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 32bpp 自上而下的 DIB。ULW_ALPHA 要求源位图已经预乘好 alpha。
struct Surface {
    HDC dc{};
    HBITMAP bitmap{};
    HBITMAP old{};
    void* bits{};
    int width{};
    int height{};

    bool Create(int w, int h) {
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (dc == nullptr) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;  // 负数 = 自上而下
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr) return false;
        old = static_cast<HBITMAP>(SelectObject(dc, bitmap));
        width = w;
        height = h;
        return true;
    }

    void Destroy() {
        if (dc != nullptr && old != nullptr) SelectObject(dc, old);
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (dc != nullptr) DeleteDC(dc);
    }

    // 直接写像素：这个基准量的是**提交**开销，不是绘制质量，所以不引入 D2D。
    void Fill(const RECT& r, unsigned argb) {
        auto* px = static_cast<unsigned*>(bits);
        const int x0 = std::max<int>(0, r.left);
        const int y0 = std::max<int>(0, r.top);
        const int x1 = std::min<int>(width, r.right);
        const int y1 = std::min<int>(height, r.bottom);
        for (int y = y0; y < y1; ++y) {
            unsigned* row = px + static_cast<size_t>(y) * static_cast<size_t>(width);
            std::fill(row + x0, row + x1, argb);
        }
    }
};

void Ring(const RECT& frame, int stroke, RECT (&out)[4]) {
    out[0] = RECT{frame.left, frame.top, frame.right, frame.top + stroke};
    out[1] = RECT{frame.left, frame.bottom - stroke, frame.right, frame.bottom};
    out[2] = RECT{frame.left, frame.top, frame.left + stroke, frame.bottom};
    out[3] = RECT{frame.right - stroke, frame.top, frame.right, frame.bottom};
}

void Report(const char* name, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    auto pct = [&v](double p) {
        const size_t i =
            std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * p / 100.0));
        return v[i];
    };
    const auto oneFrame = static_cast<size_t>(
        std::count_if(v.begin(), v.end(), [](double d) { return d <= 16.7; }));
    std::printf("%-12s 中位数 %6.2f ms   p90 %6.2f ms   p99 %6.2f ms   最慢 %6.2f ms   "
                "一帧内 %zu/%zu (%.0f%%)\n",
                name, pct(50), pct(90), pct(99), v.back(), oneFrame, v.size(),
                100.0 * static_cast<double>(oneFrame) / static_cast<double>(v.size()));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_freq);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int rounds = argc > 1 ? _wtoi(argv[1]) : 300;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindowMark.OverlayBench";
    RegisterClassExW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST,
        L"WindowMark.OverlayBench", L"", WS_POPUP, 0, 0, sw, sh, nullptr, nullptr,
        wc.hInstance, nullptr);
    if (overlay == nullptr) {
        std::printf("创建 overlay 失败: %lu\n", GetLastError());
        return 1;
    }
    ShowWindow(overlay, SW_SHOWNOACTIVATE);

    Surface surface;
    if (!surface.Create(sw, sh)) {
        std::printf("创建 DIB 失败: %lu\n", GetLastError());
        DestroyWindow(overlay);
        return 1;
    }

    std::vector<double> dirtyTimes;
    std::vector<double> fullTimes;
    dirtyTimes.reserve(static_cast<size_t>(rounds));
    fullTimes.reserve(static_cast<size_t>(rounds));

    constexpr int kStroke = 4;
    constexpr unsigned kColor = 0xFF6274E7;  // 已经是不透明，无需预乘
    RECT prev{};

    for (int i = 0; i < rounds; ++i) {
        // 模拟一个 1200x800 的窗口在屏幕上来回移动
        const int x = 100 + (i % 200) * 3;
        const int y = 80 + (i % 100) * 2;
        const RECT frame{x, y, std::min(x + 1200, sw), std::min(y + 800, sh)};

        RECT strips[4]{};
        if (i > 0) {
            Ring(prev, kStroke, strips);
            for (const RECT& s : strips) surface.Fill(s, 0x00000000);
        }
        Ring(frame, kStroke, strips);
        for (const RECT& s : strips) surface.Fill(s, kColor);

        POINT src{0, 0};
        SIZE size{sw, sh};
        POINT dst{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        // 变化区域 = 旧圈与新圈的并集，外扩 stroke 保证盖住
        RECT dirty = frame;
        if (i > 0) {
            UnionRect(&dirty, &frame, &prev);
        }
        InflateRect(&dirty, kStroke, kStroke);

        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.pptSrc = &src;
        info.psize = &size;
        info.hdcSrc = surface.dc;
        info.pptDst = &dst;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;

        info.prcDirty = &dirty;
        const double t0 = NowMs();
        UpdateLayeredWindowIndirect(overlay, &info);
        dirtyTimes.push_back(NowMs() - t0);

        info.prcDirty = nullptr;
        const double t1 = NowMs();
        UpdateLayeredWindowIndirect(overlay, &info);
        fullTimes.push_back(NowMs() - t1);

        prev = frame;
        Sleep(8);
    }

    surface.Destroy();
    DestroyWindow(overlay);

    std::printf("Overlay 提交开销（%d 轮，全屏 %dx%d = %.1f MB/帧）\n\n", rounds, sw, sh,
                static_cast<double>(sw) * sh * 4.0 / 1024.0 / 1024.0);
    Report("脏矩形", dirtyTimes);
    Report("整屏", fullTimes);
    std::printf("\n基线（现有实现，纯移动不重绘）："
                "中位数   3.00 ms                    p99  15.70 ms"
                "                    一帧内 100%%\n");
    return 0;
}
