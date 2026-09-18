#pragma once

#include "windowmark/core/Interfaces.h"

#include "WinLayeredSurface.h"
#include "WinPreviewTitle.h"

#include <windows.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;
struct IDWriteFactory;

namespace windowmark::win {

// 三段式预览栈的后两段：浮动标题和缩略图。第一段——书签本身——在书签条里。
//
// 书签条每一帧都调一次 Show，带着这一帧所有书签的矩形；这里量出标题有多长，用
// LayoutPreviewStack 排出标题和缩略图的位置，再把两个窗口挪过去、画上去。三段谁也不压谁，
// 这一条由 LayoutPreviewStack 保证，这里只负责照着画。
//
// 两个窗口都点不中（WS_EX_TRANSPARENT）：它们只是告诉你鼠标下是哪个窗口，鼠标该落在书签上。
class WinPreviewBackend final : public IPreviewBackend {
public:
    WinPreviewBackend();
    ~WinPreviewBackend() override;

    bool Start(const PreviewSettings& settings) override;
    void Show(const PreviewRequest& request) override;
    void UpdateSettings(const PreviewSettings& settings) override;
    void Hide() noexcept override;
    void Stop() noexcept override;

private:
    struct Thumb {
        WindowId source{};
        HTHUMBNAIL handle{};
    };

    static LRESULT CALLBACK TitleProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ThumbProc(HWND, UINT, WPARAM, LPARAM);
    bool EnsureWindows();
    bool EnsureDrawing();
    void PresentTitle(const RectF& rect, bool vertical, float opacity,
                      const std::vector<PreviewTitle::Layer>& layers);
    void PresentThumbnails(const RectF& rect, const PreviewRequest& request);
    void HideTitle() noexcept;
    void HideThumbnails() noexcept;
    // 缩略图窗口的底色，以及书签指向宿主自己时代替缩略图的「当前窗口」字样。
    void PaintThumbWindow(HWND hwnd);

    PreviewSettings settings_;
    HWND titleHwnd_{};
    HWND thumbHwnd_{};
    LayeredSurface titleSurface_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    std::unique_ptr<PreviewTitle> title_;
    std::vector<Thumb> thumbs_;
    // 「当前窗口」字样的不透明度：书签指向宿主自己时没有缩略图可放，缩略图的位置上改写这
    // 四个字。和缩略图一样跟着主标签交叉淡入淡出。
    float selfCard_{};
    float scale_{1.0F};
    HFONT cardFont_{};
    int cardFontPx_{};
    RECT thumbRect_{};
    bool titleShown_{false};
    bool thumbShown_{false};
    bool started_{false};
};

} // namespace windowmark::win
