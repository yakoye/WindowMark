#pragma once

#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <functional>
#include <optional>
#include <vector>

namespace windowmark {

class IWindowBackend {
public:
    using EventSink = std::function<void(const WindowEvent&)>;

    virtual ~IWindowBackend() = default;
    virtual bool Start(EventSink sink) = 0;
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual std::vector<WindowInfo> EnumerateWindows() = 0;
    [[nodiscard]] virtual std::optional<WindowInfo> QueryWindow(WindowId id) = 0;
    virtual bool ActivateWindow(WindowId id) = 0;
};

class IOverlayBackend {
public:
    virtual ~IOverlayBackend() = default;
    virtual bool Start(const Settings& settings, OverlayCallbacks callbacks) = 0;
    virtual void Apply(const std::vector<OverlayModel>& models) = 0;
    // Applied while running, so edits in the settings UI take effect without a restart.
    virtual void UpdateSettings(const Settings& settings) = 0;
    virtual void Stop() noexcept = 0;
};

class IPreviewBackend {
public:
    virtual ~IPreviewBackend() = default;
    virtual bool Start(const PreviewSettings& settings) = 0;
    virtual void Show(const PreviewRequest& request) = 0;
    virtual void UpdateSettings(const PreviewSettings& settings) = 0;
    virtual void Hide() noexcept = 0;
    virtual void Stop() noexcept = 0;
};

} // namespace windowmark
