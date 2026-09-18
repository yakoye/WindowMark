// 离屏检查三段式预览栈：四个方向、几个典型场景，把 书签 / 浮动标题 / 缩略图 画成 PNG，
// 同时逐个断言三段互不重叠、都在工作区里。
//
// 不开窗口、不动鼠标、不截屏：标题用的是书签条真正用的 PreviewTitle（同一份量法和画法），
// 书签位置用的是真正的 LayoutEngine + MagneticDock，三段位置用的是真正的 LayoutPreviewStack。
// 缩略图只画成一块灰色占位——它是 DWM 画的，离屏画不出来，这里要看的是它放在哪、多大。
//
// 用法：StackCheck.exe <输出目录>        退出码 0 = 所有断言通过
#include "WinPreviewTitle.h"
#include "windowmark/core/LayoutEngine.h"
#include "windowmark/core/MagneticDock.h"
#include "windowmark/core/PreviewStack.h"
#include "windowmark/core/Settings.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace windowmark;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kCanvasW = 1600;
constexpr int kCanvasH = 1000;
constexpr float kScale = 1.25F;   // 按 125% 缩放的显示器排标题

int g_failures = 0;

void Expect(bool ok, const wchar_t* scene, const wchar_t* what) {
    if (ok) return;
    ++g_failures;
    std::fwprintf(stderr, L"  !! %ls: %ls\n", scene, what);
}

struct TitleLayer {
    std::wstring text;
    int item;
    float opacity;
};

struct Scene {
    const wchar_t* name;
    Placement placement;
    Rect host;
    float pointerItems;   // 鼠标位置，以标签为单位：3 = 第 3 个标签正中，3.5 = 它和下一个之间的间隙正中
    std::vector<TitleLayer> layers;
    bool thumbnail;
};

ComPtr<IWICImagingFactory> g_wic;
ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;

bool SavePng(IWICBitmapSource* source, const std::wstring& path) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(g_wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
        return false;
    }
    UINT w = 0;
    UINT h = 0;
    source->GetSize(&w, &h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    frame->SetSize(w, h);
    frame->SetPixelFormat(&format);
    return SUCCEEDED(frame->WriteSource(source, nullptr)) && SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(encoder->Commit());
}

// 把 rect 周围一圈放大 zoom 倍单独存一张，看字用。
bool SaveCrop(IWICBitmap* bitmap, const RectF& rect, int zoom, const std::wstring& path) {
    const int margin = 24;
    WICRect crop{};
    crop.X = std::max(0, static_cast<int>(rect.left) - margin);
    crop.Y = std::max(0, static_cast<int>(rect.top) - margin);
    crop.Width = std::min(kCanvasW - crop.X, static_cast<int>(rect.width()) + margin * 2);
    crop.Height = std::min(kCanvasH - crop.Y, static_cast<int>(rect.height()) + margin * 2);
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(g_wic->CreateBitmapClipper(&clipper)) ||
        FAILED(clipper->Initialize(bitmap, &crop)) || FAILED(g_wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(clipper.Get(), static_cast<UINT>(crop.Width * zoom),
                                  static_cast<UINT>(crop.Height * zoom),
                                  WICBitmapInterpolationModeNearestNeighbor))) {
        return false;
    }
    return SavePng(scaler.Get(), path);
}

D2D1_RECT_F ToD2D(const RectF& r) { return D2D1::RectF(r.left, r.top, r.right, r.bottom); }

bool Inside(const RectF& inner, const RectF& outer) {
    return inner.left >= outer.left - 0.01F && inner.top >= outer.top - 0.01F &&
           inner.right <= outer.right + 0.01F && inner.bottom <= outer.bottom + 0.01F;
}

void RunScene(const Scene& scene, const std::wstring& outDir) {
    const Settings settings;
    const bool side = scene.placement == Placement::Left || scene.placement == Placement::Right;
    const int count = 7;
    const int active = 2;

    WindowInfo host;
    host.frame = scene.host;
    host.workArea = Rect{0, 0, kCanvasW, kCanvasH};
    const DockSpec spec = LayoutEngine::DockSpecFor(scene.placement, count, active, settings.drawer);
    const float growth = DockMaxGrowth(spec.items, spec.params);
    const DockBounds bounds =
        LayoutEngine::ComputeOverlayBounds(host, spec, growth, scene.placement, settings.drawer);
    const std::vector<float> starts =
        DockBaseStarts(spec.items, spec.params.gap, bounds.baseOrigin);

    // 鼠标位置：第 k 个标签正中，或它和下一个之间的间隙正中
    const int k = static_cast<int>(scene.pointerItems);
    const bool inGap = scene.pointerItems - static_cast<float>(k) > 0.25F;
    const float pointer = inGap
        ? starts[k] + spec.items[k].main + spec.params.gap * 0.5F
        : starts[k] + spec.items[k].main * 0.5F;
    std::vector<DockItemVisual> visual;
    LayoutDock(spec.items, starts, spec.params, pointer, 1.0F, visual);

    const Rect& b = bounds.bounds;
    const float w = static_cast<float>(b.width());
    const float h = static_cast<float>(b.height());
    std::vector<RectF> tabs;
    for (const DockItemVisual& v : visual) {
        RectF r{};
        switch (scene.placement) {
        case Placement::Top: r = RectF{v.start, 0.0F, v.start + v.main, v.cross}; break;
        case Placement::Left: r = RectF{0.0F, v.start, v.cross, v.start + v.main}; break;
        case Placement::Right: r = RectF{w - v.cross, v.start, w, v.start + v.main}; break;
        default: r = RectF{v.start, h - v.cross, v.start + v.main, h}; break;
        }
        tabs.push_back(RectF{r.left + static_cast<float>(b.left), r.top + static_cast<float>(b.top),
                             r.right + static_cast<float>(b.left),
                             r.bottom + static_cast<float>(b.top)});
    }

    // 标题：和预览端一样按不透明度加权长度、加权中心
    windowmark::win::PreviewTitle title(g_dwrite.Get());
    title.SetScale(kScale);
    const float maxMain = static_cast<float>(side ? settings.preview.height : settings.preview.width);
    float lengthSum = 0.0F;
    float anchorSum = 0.0F;
    float weight = 0.0F;
    std::vector<windowmark::win::PreviewTitle::Layer> drawn;
    for (const TitleLayer& layer : scene.layers) {
        const float length = std::min(title.NaturalLength(layer.text), maxMain);
        const DockItemVisual& v = visual[static_cast<std::size_t>(layer.item)];
        const float center = (side ? static_cast<float>(b.top) : static_cast<float>(b.left)) +
                             v.start + v.main * 0.5F;
        lengthSum += length * layer.opacity;
        anchorSum += center * layer.opacity;
        weight += layer.opacity;
        drawn.push_back({layer.text, layer.opacity});
    }

    PreviewStackInput in;
    in.placement = scene.placement;
    in.workArea = RectF{0.0F, 0.0F, static_cast<float>(kCanvasW), static_cast<float>(kCanvasH)};
    switch (scene.placement) {
    case Placement::Top: in.rootEdge = static_cast<float>(b.top); break;
    case Placement::Left: in.rootEdge = static_cast<float>(b.left); break;
    case Placement::Right: in.rootEdge = static_cast<float>(b.right); break;
    default: in.rootEdge = static_cast<float>(b.bottom); break;
    }
    in.tabs = tabs;
    in.anchorMain = anchorSum / weight;
    in.titleMain = lengthSum / weight;
    in.titleCross = title.Thickness();
    in.thumbnailMain =
        scene.thumbnail ? static_cast<float>(side ? settings.preview.height : settings.preview.width)
                        : 0.0F;
    in.thumbnailCross = static_cast<float>(side ? settings.preview.width : settings.preview.height);
    in.titleGap = static_cast<float>(settings.preview.titleGap);
    in.thumbnailGap = static_cast<float>(settings.preview.thumbnailGap);
    in.minThumbnailMain = side ? 80.0F : 120.0F;
    in.minThumbnailCross = side ? 120.0F : 80.0F;
    const PreviewStackLayout stack = LayoutPreviewStack(in);

    // --- 断言 ---
    for (const RectF& tab : tabs) {
        Expect(!RectsOverlap(stack.title, tab), scene.name, L"标题压到了书签");
        if (stack.thumbnailVisible) {
            Expect(!RectsOverlap(stack.thumbnail, tab), scene.name, L"缩略图压到了书签");
        }
        Expect(Inside(tab, RectF{static_cast<float>(b.left), static_cast<float>(b.top),
                                 static_cast<float>(b.right), static_cast<float>(b.bottom)}),
               scene.name, L"书签超出了书签条窗口（会被裁掉）");
    }
    if (stack.thumbnailVisible) {
        Expect(!RectsOverlap(stack.title, stack.thumbnail), scene.name, L"标题和缩略图重叠");
        Expect(Inside(stack.thumbnail, in.workArea), scene.name, L"缩略图出了工作区");
    }
    Expect(Inside(stack.title, in.workArea), scene.name, L"标题出了工作区");
    Expect(scene.thumbnail == stack.thumbnailVisible || scene.name[0] == L'4', scene.name,
           L"缩略图该显示却没显示");

    // --- 画 ---
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    if (FAILED(g_wic->CreateBitmap(kCanvasW, kCanvasH, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(g_d2d->CreateWicBitmapRenderTarget(bitmap.Get(), &props, &target))) {
        Expect(false, scene.name, L"建不出离屏画布");
        return;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F), &brush);
    target->BeginDraw();
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->Clear(D2D1::ColorF(0.23F, 0.26F, 0.31F, 1.0F));   // 桌面
    // 宿主窗口：随便画点「内容」，看标题压在内容上是否读得清
    const RectF hostF{static_cast<float>(host.frame.left), static_cast<float>(host.frame.top),
                      static_cast<float>(host.frame.right), static_cast<float>(host.frame.bottom)};
    brush->SetColor(D2D1::ColorF(0.96F, 0.96F, 0.97F, 1.0F));
    target->FillRectangle(ToD2D(hostF), brush.Get());
    brush->SetColor(D2D1::ColorF(0.80F, 0.82F, 0.86F, 1.0F));
    for (float y = hostF.top + 40.0F; y < hostF.bottom - 20.0F; y += 22.0F) {
        target->FillRectangle(D2D1::RectF(hostF.left + 90.0F, y, hostF.right - 90.0F, y + 8.0F),
                              brush.Get());
    }
    // 书签条窗口的范围（虚线框），和书签
    brush->SetColor(D2D1::ColorF(1.0F, 0.0F, 0.6F, 0.6F));
    target->DrawRectangle(D2D1::RectF(static_cast<float>(b.left) + 0.5F,
                                      static_cast<float>(b.top) + 0.5F,
                                      static_cast<float>(b.right) - 0.5F,
                                      static_cast<float>(b.bottom) - 0.5F),
                          brush.Get(), 1.0F);
    const D2D1_COLOR_F palette[] = {
        D2D1::ColorF(0.98F, 0.72F, 0.72F), D2D1::ColorF(0.98F, 0.84F, 0.66F),
        D2D1::ColorF(0.97F, 0.93F, 0.64F), D2D1::ColorF(0.74F, 0.92F, 0.72F),
        D2D1::ColorF(0.68F, 0.87F, 0.97F), D2D1::ColorF(0.78F, 0.76F, 0.98F),
        D2D1::ColorF(0.95F, 0.76F, 0.93F),
    };
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        brush->SetColor(palette[i % std::size(palette)]);
        target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(tabs[i]), 6.0F, 6.0F), brush.Get());
    }
    // 标题
    title.Draw(*target.Get(), ToD2D(stack.title), side, drawn);
    // 缩略图占位
    if (stack.thumbnailVisible) {
        brush->SetColor(D2D1::ColorF(0.965F, 0.969F, 0.976F, 1.0F));
        target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(stack.thumbnail), 12.0F, 12.0F),
                                     brush.Get());
        brush->SetColor(D2D1::ColorF(0.55F, 0.58F, 0.63F, 1.0F));
        const RectF t = stack.thumbnail;
        target->FillRectangle(D2D1::RectF(t.left + 8.0F, t.top + 8.0F, t.right - 8.0F,
                                          t.bottom - 8.0F),
                              brush.Get());
    }
    target->EndDraw();

    const std::wstring base = outDir + L"\\" + scene.name;
    Expect(SavePng(bitmap.Get(), base + L".png"), scene.name, L"存不了 PNG");
    Expect(SaveCrop(bitmap.Get(), stack.title, 3, base + L"-title.png"), scene.name,
           L"存不了标题特写");

    std::fwprintf(stdout,
                  L"%-22ls 书签条 %4d,%4d %4dx%-3d  标题 %6.1f,%6.1f %5.1fx%-5.1f  缩略图 %ls "
                  L"%6.1f,%6.1f %5.1fx%-5.1f\n",
                  scene.name, b.left, b.top, b.width(), b.height(), stack.title.left,
                  stack.title.top, stack.title.width(), stack.title.height(),
                  stack.thumbnailVisible ? L"显示" : L"隐藏", stack.thumbnail.left,
                  stack.thumbnail.top, stack.thumbnail.width(), stack.thumbnail.height());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"用法：StackCheck.exe <输出目录>\n");
        return 2;
    }
    // 宽字符按 UTF-8 输出，管道另一头才读得到中文。
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    const std::wstring outDir = argv[1];
    CreateDirectoryW(outDir.c_str(), nullptr);

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_wic))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf())))) {
        std::fwprintf(stderr, L"初始化 WIC / D2D / DWrite 失败\n");
        return 2;
    }

    const Rect maximized{0, 0, kCanvasW, kCanvasH};
    const Rect normal{240, 60, 1360, 940};
    const std::wstring code = L"MagneticDock.cpp - WindowMark - Visual Studio Code";
    const std::wstring shortTitle = L"终端";
    const std::wstring longTitle =
        L"一个非常非常长的窗口标题，用来检查省略号是否正常工作 — Microsoft Edge 浏览器 Beta 2026";
    const std::wstring mixed = L"设计文档 Design Doc.md";

    std::vector<Scene> scenes;
    const struct {
        const wchar_t* tag;
        Placement placement;
        Rect host;
    } directions[] = {
        {L"bottom", Placement::Bottom, maximized},
        {L"top", Placement::Top, maximized},
        {L"left", Placement::Left, normal},
        {L"right", Placement::Right, normal},
    };
    static std::vector<std::wstring> names;
    names.reserve(32);
    for (const auto& d : directions) {
        names.push_back(std::wstring(L"1-center-") + d.tag);
        scenes.push_back({names.back().c_str(), d.placement, d.host, 3.0F, {{code, 3, 1.0F}}, true});
        names.push_back(std::wstring(L"2-gap-") + d.tag);
        scenes.push_back({names.back().c_str(), d.placement, d.host, 3.5F, {{mixed, 3, 1.0F}}, true});
        names.push_back(std::wstring(L"3-crossfade-") + d.tag);
        scenes.push_back({names.back().c_str(), d.placement, d.host, 4.0F,
                          {{code, 3, 0.5F}, {shortTitle, 4, 0.5F}}, true});
        names.push_back(std::wstring(L"5-long-") + d.tag);
        scenes.push_back({names.back().c_str(), d.placement, d.host, 0.0F,
                          {{longTitle, 0, 1.0F}}, true});
    }
    // 缩略图放不下的场景：宿主矮、书签栏又在朝屏幕边缘长的那一侧
    names.push_back(L"4-tight-bottom");
    scenes.push_back({names.back().c_str(), Placement::Bottom, Rect{0, 0, kCanvasW, 260}, 3.0F,
                      {{code, 3, 1.0F}}, true});
    names.push_back(L"4-tight-right");
    scenes.push_back({names.back().c_str(), Placement::Right, Rect{0, 60, 330, 940}, 3.0F,
                      {{code, 3, 1.0F}}, true});

    for (const Scene& scene : scenes) RunScene(scene, outDir);

    if (g_failures > 0) {
        std::fwprintf(stderr, L"%d 项检查没通过\n", g_failures);
        return 1;
    }
    std::fwprintf(stdout, L"全部通过\n");
    return 0;
}
