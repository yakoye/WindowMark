#pragma once

#include "windowmark/core/Interfaces.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace windowmark::win {

class WinOverlayBackend final : public IOverlayBackend {
public:
    WinOverlayBackend();
    ~WinOverlayBackend() override;

    bool Start(const Settings& settings, OverlayCallbacks callbacks) override;
    void Apply(const std::vector<OverlayModel>& models) override;
    void UpdateSettings(const Settings& settings) override;
    void Stop() noexcept override;

private:
    class OverlayWindow;

    bool EnsureFactories();
    bool EnsureWindowClass();
    // One DC render target and one text format are enough: overlays are drawn one at
    // a time on the UI thread, and the target is re-bound to each window's memory DC.
    bool EnsureDrawingResources();

    Settings settings_;
    OverlayCallbacks callbacks_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;

    // Text formats are keyed by (pixel size, centred). Size has to vary because a tab is
    // only as tall as the drawer settings make it - a 13px font needs 17.3px of line
    // height, so on a 16px collapsed tab the text is clipped away entirely. Expanded tabs
    // read better left-aligned; a narrow collapsed tab only fits a glyph or two, which
    // looks wrong anywhere but centred.
    [[nodiscard]] IDWriteTextFormat* FormatFor(int pixelSize, bool centred);
    // Largest size whose line height still fits `height`, within sane bounds.
    [[nodiscard]] static int FontSizeFor(float height);

    std::map<std::pair<int, bool>, Microsoft::WRL::ComPtr<IDWriteTextFormat>> textFormats_;
    std::unordered_map<WindowId, std::unique_ptr<OverlayWindow>> windows_;
    bool started_{false};
};

} // namespace windowmark::win
