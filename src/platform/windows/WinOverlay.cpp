#include "WinOverlay.h"

#include "windowmark/core/BorderOcclusion.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace windowmark::win {

namespace {
RenderTrace g_render;

[[nodiscard]] LONGLONG RenderTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

[[nodiscard]] double RenderMsSince(LONGLONG start) {
    static const double perMs = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(freq.QuadPart) / 1000.0;
    }();
    return static_cast<double>(RenderTicks() - start) / perMs;
}
} // namespace

RenderTrace TakeRenderTrace() {
    const RenderTrace copy = g_render;
    g_render = RenderTrace{};
    return copy;
}

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

// from 减去 hole，最多切出四块，互不重叠。
//
// 圆角只发生在环那一圈带子上，中间那一大块空洞里一个像素都不会亮。挖掉它，逐像素的
// 循环就从「整个窗口」缩到「一圈带子」——一个 1223x724 的窗口从 88 万像素降到 3 万。
void SubtractInto(const RECT& from, const RECT& hole, std::vector<RECT>& out) {
    if (from.right <= from.left || from.bottom <= from.top) return;
    if (hole.right <= hole.left || hole.bottom <= hole.top ||
        hole.right <= from.left || hole.left >= from.right ||
        hole.bottom <= from.top || hole.top >= from.bottom) {
        out.push_back(from);
        return;
    }
    const LONG midTop = std::max(from.top, hole.top);
    const LONG midBottom = std::min(from.bottom, hole.bottom);
    if (from.top < midTop) out.push_back(RECT{from.left, from.top, from.right, midTop});
    if (midBottom < from.bottom) {
        out.push_back(RECT{from.left, midBottom, from.right, from.bottom});
    }
    if (midTop < midBottom) {
        if (from.left < hole.left) {
            out.push_back(RECT{from.left, midTop, std::min(from.right, hole.left),
                               midBottom});
        }
        if (hole.right < from.right) {
            out.push_back(RECT{std::max(from.left, hole.right), midTop, from.right,
                               midBottom});
        }
    }
}

// 把一个预乘过的颜色按 source-over 混到目标像素上。
//
// 不能直接覆盖：抗锯齿的边缘 alpha 不满，两个边框在角上叠着的时候直接写会把先画的
// 那条挖出一圈半透明的缺口。
void BlendPixel(unsigned& dst, unsigned srcR, unsigned srcG, unsigned srcB, unsigned a) {
    if (a == 0U) return;
    const unsigned pr = srcR * a / 255U;
    const unsigned pg = srcG * a / 255U;
    const unsigned pb = srcB * a / 255U;
    if (a >= 255U) {
        dst = (255U << 24) | (pr << 16) | (pg << 8) | pb;
        return;
    }
    const unsigned inv = 255U - a;
    const unsigned da = (dst >> 24) & 0xFFU;
    const unsigned dr = (dst >> 16) & 0xFFU;
    const unsigned dg = (dst >> 8) & 0xFFU;
    const unsigned db = dst & 0xFFU;
    dst = (((a + da * inv / 255U) & 0xFFU) << 24) |
          (((pr + dr * inv / 255U) & 0xFFU) << 16) |
          (((pg + dg * inv / 255U) & 0xFFU) << 8) |
          ((pb + db * inv / 255U) & 0xFFU);
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

    ++g_render.frames;
    const LONGLONG fillStart = RenderTicks();

    // 先挑出落在这块屏上的段。
    std::vector<BorderStroke> current;
    current.reserve(strokes.size());
    for (const BorderStroke& stroke : strokes) {
        const RECT r{stroke.rect.left, stroke.rect.top, stroke.rect.right,
                     stroke.rect.bottom};
        RECT probe{};
        if (IntersectRect(&probe, &r, &bounds_) == FALSE) continue;
        current.push_back(stroke);
    }

    // 脏区只包含增删的段。
    //
    // 拖一个窗口时其他窗口的边框一个像素都没动，把它们也算进脏区就等于每帧重贴大半个
    // 屏幕——那正是提交和擦除的成本所在。段数只有几十个，两两比一遍比省下的那点面积
    // 便宜得多。
    const auto has = [](const std::vector<BorderStroke>& list, const BorderStroke& one) {
        return std::any_of(list.begin(), list.end(),
                           [&one](const BorderStroke& other) { return other == one; });
    };
    RECT dirty{};
    bool anyDirty = false;
    const auto growDirty = [&](const Rect& r) {
        const RECT local{r.left - bounds_.left, r.top - bounds_.top,
                         r.right - bounds_.left, r.bottom - bounds_.top};
        if (anyDirty) {
            UnionRect(&dirty, &dirty, &local);
        } else {
            dirty = local;
            anyDirty = true;
        }
    };
    for (const BorderStroke& stroke : current) {
        if (!has(lastSegments_, stroke)) growDirty(stroke.rect);
    }
    for (const BorderStroke& stroke : lastSegments_) {
        if (!has(current, stroke)) growDirty(stroke.rect);
    }
    lastSegments_ = current;

    if (!anyDirty) {
        g_render.fillMs += RenderMsSince(fillStart);
        return;   // 这块屏上什么都没变
    }
    dirty.left = std::max<LONG>(0, dirty.left);
    dirty.top = std::max<LONG>(0, dirty.top);
    dirty.right = std::min<LONG>(width, dirty.right);
    dirty.bottom = std::min<LONG>(height, dirty.bottom);
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
        g_render.fillMs += RenderMsSince(fillStart);
        return;
    }

    // 擦掉脏区，再把**所有**和它相交的段重画一遍——只重画变了的那些不行，擦除会连带
    // 抹掉压在同一块地方的其他段。
    clearRect(dirty);

    // 圆角段先攒着，等像素填充做完再一次性交给 D2D——中途来回切换会让 GDI 和 D2D
    // 对同一块位图的写入次序变得难以推理。
    struct RoundedSeg {
        const BorderStroke* stroke;
        RECT local;
    };
    std::vector<RoundedSeg> rounded;

    for (const BorderStroke& stroke : current) {
        const RECT r{stroke.rect.left - bounds_.left, stroke.rect.top - bounds_.top,
                     stroke.rect.right - bounds_.left, stroke.rect.bottom - bounds_.top};
        // 脏区之外的部分位图上还是好的，不必重画。
        RECT local{};
        if (IntersectRect(&local, &r, &dirty) == FALSE) continue;

        if (stroke.roundWidth > 0.0F) {
            rounded.push_back(RoundedSeg{&stroke, local});
        } else {
            // 直角：直接填像素，最快的一条路。
            const unsigned color = Premultiply(stroke.color);
            for (int y = local.top; y < local.bottom; ++y) {
                unsigned* row =
                    pixels + static_cast<size_t>(y) * static_cast<size_t>(width);
                std::fill(row + local.left, row + local.right, color);
            }
        }
    }

    g_render.fillMs += RenderMsSince(fillStart);
    g_render.arcSegments += static_cast<int>(rounded.size());
    const LONGLONG arcStart = RenderTicks();

    // 圆角：自己栅格化。
    //
    // 圆角矩形的有符号距离有闭式解——把点折到第一象限，减去「直边半长」，负的部分说明
    // 还在直边段上，正的部分才走到角的弧上。一次 sqrt 就得到这个点离环中线多远，再和
    // 半个线宽比，就知道该涂多满。
    //
    // 之前这里是 D2D：每一段可见部分都要把整个环的路径栅格化一遍，一个窗口每帧六段
    // 就是六遍，实测每段 2.15ms。现在按段只碰那一圈带子上的像素。
    std::vector<RECT> pieces;
    for (const RoundedSeg& seg : rounded) {
        const BorderStroke& s = *seg.stroke;
        const float ol = static_cast<float>(s.ringOuter.left - bounds_.left);
        const float ot = static_cast<float>(s.ringOuter.top - bounds_.top);
        const float orr = static_cast<float>(s.ringOuter.right - bounds_.left);
        const float ob = static_cast<float>(s.ringOuter.bottom - bounds_.top);
        const float in = s.roundInset;
        const float pathL = ol + in;
        const float pathT = ot + in;
        const float pathR = orr - in;
        const float pathB = ob - in;
        if (pathR <= pathL || pathB <= pathT) continue;

        const float cx = (pathL + pathR) * 0.5F;
        const float cy = (pathT + pathB) * 0.5F;
        const float hx = (pathR - pathL) * 0.5F;
        const float hy = (pathB - pathT) * 0.5F;
        // 半径大过半边长就画成胶囊，别让它把角互相吃穿。
        const float radius = std::max(0.0F, std::min(s.radius, std::min(hx, hy)));
        const float half = s.roundWidth * 0.5F;

        const unsigned srcA = (s.color >> 24) & 0xFFU;
        const unsigned srcR = (s.color >> 16) & 0xFFU;
        const unsigned srcG = (s.color >> 8) & 0xFFU;
        const unsigned srcB = s.color & 0xFFU;

        // 中间那一大块空洞里不可能有笔迹，挖掉它再逐像素跑。
        //
        // 收多少不能只看线宽：洞是直角的，环的内沿在角上是圆弧，收少了洞的角就顶进
        // 环带、把圆角内侧削掉一块。RingHoleInset 把这件事算清楚了。
        const float holeInset = RingHoleInset(radius, half);
        const RECT hole{
            static_cast<LONG>(std::ceil(pathL + holeInset)),
            static_cast<LONG>(std::ceil(pathT + holeInset)),
            static_cast<LONG>(std::floor(pathR - holeInset)),
            static_cast<LONG>(std::floor(pathB - holeInset))};
        pieces.clear();
        SubtractInto(seg.local, hole, pieces);

        for (const RECT& piece : pieces) {
            const int px0 = std::max<LONG>(0, piece.left);
            const int py0 = std::max<LONG>(0, piece.top);
            const int px1 = std::min<LONG>(width, piece.right);
            const int py1 = std::min<LONG>(height, piece.bottom);
            if (px1 <= px0 || py1 <= py0) continue;
            g_render.arcPixels += static_cast<double>(px1 - px0) *
                                  static_cast<double>(py1 - py0);
            for (int y = py0; y < py1; ++y) {
                const float py = static_cast<float>(y) + 0.5F - cy;
                unsigned* row =
                    pixels + static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = px0; x < px1; ++x) {
                    const float px = static_cast<float>(x) + 0.5F - cx;
                    const float dist = RoundedRectDistance(px, py, hx, hy, radius);
                    // 覆盖率：离环中线越近越满，边上留一个像素的过渡。
                    const float cover = half - std::fabs(dist) + 0.5F;
                    if (cover <= 0.0F) continue;
                    const unsigned a = cover >= 1.0F
                        ? srcA
                        : static_cast<unsigned>(static_cast<float>(srcA) * cover + 0.5F);
                    BlendPixel(row[x], srcR, srcG, srcB, a);
                }
            }
        }
    }

    g_render.arcMs += RenderMsSince(arcStart);
    const LONGLONG commitStart = RenderTicks();

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

    g_render.commitMs += RenderMsSince(commitStart);
    g_render.dirtyMegapixels +=
        static_cast<double>(dirty.right - dirty.left) *
        static_cast<double>(dirty.bottom - dirty.top) / 1000000.0;
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
    lastSegments_.clear();
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
