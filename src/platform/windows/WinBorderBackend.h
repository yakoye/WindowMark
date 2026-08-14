#pragma once

#include "windowmark/core/Interfaces.h"

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;

namespace windowmark::win {

// Draws a coloured outline hugging each tracked window. Deliberately independent of the
// bookmark overlay: it has its own enable switch and covers every top-level window,
// including apps that never get a bookmark strip. The two only share window tracking.
class WinBorderBackend final : public IBorderBackend {
public:
    WinBorderBackend();
    ~WinBorderBackend() override;

    bool Start(const Settings& settings) override;
    void Apply(const std::vector<BorderModel>& models) override;
    void MoveBorder(WindowId id, const Rect& frame) override;
    void UpdateSettings(const Settings& settings) override;
    void Stop() noexcept override;

private:
    class BorderWindow;
    class LayeredSurface;

    bool EnsureFactory();
    bool EnsureWindowClass();
    // One render target for every border. Borders are drawn one at a time on the UI
    // thread and the target is re-bound to each window's memory DC, so a target per
    // window would just multiply GPU-side resources by the number of open windows.
    bool EnsureRenderTarget();

    // One scratch bitmap for the whole process, grown to the largest outline seen.
    // UpdateLayeredWindow copies what it is handed, so every border can render into the
    // same buffer in turn - which is what makes a per-window bitmap affordable and lets
    // each outline stay a single window (one SetWindowPos per move).
    std::unique_ptr<LayeredSurface> surface_;

    Settings settings_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    std::unordered_map<WindowId, std::unique_ptr<BorderWindow>> windows_;
    bool started_{false};
};

} // namespace windowmark::win
