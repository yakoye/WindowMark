#pragma once

#include "windowmark/core/Interfaces.h"

#include <windows.h>
#include <dwmapi.h>

namespace windowmark::win {

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
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool EnsureWindow();
    void UpdateThumbnail(const PreviewRequest& request);
    [[nodiscard]] RECT ComputePreviewRect(const PreviewRequest& request) const;

    PreviewSettings settings_;
    HWND hwnd_{};
    HTHUMBNAIL thumbnail_{};
    bool started_{false};
};

} // namespace windowmark::win
