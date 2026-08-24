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
    // Raised for every geometry change, bypassing the throttling EventSink is subject to.
    // A border hugs the window edge, so a throttled position visibly lags behind a drag;
    // bookmarks sit outside the window and do not care. Handlers run on the UI thread
    // inside event dispatch and must stay cheap - move a window, do not repaint it.
    using GeometrySink = std::function<void(WindowId, const Rect& frame)>;

    virtual ~IWindowBackend() = default;
    virtual bool Start(EventSink sink) = 0;
    virtual void SetGeometrySink(GeometrySink sink) = 0;
    // Extra window classes never to report, on top of whatever the backend excludes by
    // itself. Not pure: a backend with nothing shell-specific to hide needs no opinion.
    virtual void SetExcludedClasses(const std::vector<std::string>& classes) { (void)classes; }
    // Entries of the form "class:left,top,right,bottom". See TrackingSettings::shadowInsets.
    virtual void SetShadowInsets(const std::vector<std::string>& entries) { (void)entries; }
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual std::vector<WindowInfo> EnumerateWindows() = 0;
    [[nodiscard]] virtual std::optional<WindowInfo> QueryWindow(WindowId id) = 0;
    // Just the frame, for a window already known to be tracked. A drag fires hundreds of
    // location events and the only thing that changed is where the window is - re-running
    // the whole QueryWindow costs about 0.7ms of cross-process calls each time (measured),
    // to re-derive a class name, a process path and a title that cannot have changed.
    // Returns nothing if the window is gone, which is the caller's cue to re-enumerate.
    [[nodiscard]] virtual std::optional<Rect> QueryFrame(WindowId id) = 0;
    virtual bool ActivateWindow(WindowId id) = 0;
};

class IBorderBackend {
public:
    virtual ~IBorderBackend() = default;
    virtual bool Start(const Settings& settings) = 0;
    virtual void Apply(const std::vector<BorderModel>& models) = 0;
    // Cheap path for a window that only moved: reposition without re-rendering.
    virtual void MoveBorder(WindowId id, const Rect& frame) = 0;
    virtual void UpdateSettings(const Settings& settings) = 0;
    virtual void Stop() noexcept = 0;
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


// Window pinning: always-on-top, plus the ways a user asks for it.
//
// The backend owns no state of its own. Every entry point - the title bar system menu, the
// crosshair grab, the tray submenu, the shortcut - funnels into the same callback, and the
// Coordinator is the only place that decides whether a window ends up pinned. That keeps
// four different input paths from developing four different ideas of the truth.
struct PinCallbacks {
    std::function<void(WindowId)> onTogglePin;
    std::function<void()> onUnpinAll;
};

class IPinBackend {
public:
    virtual ~IPinBackend() = default;
    virtual bool Start(const Settings& settings, PinCallbacks callbacks) = 0;
    // Applies the always-on-top style and reports what it was beforehand, which is what
    // makes unpinning restore rather than clear. Empty when the window is gone.
    virtual std::optional<bool> SetTopmost(WindowId id, bool topmost) = 0;
    // The pinned set changed: refresh whatever reflects it - the tray submenu, the tick in
    // a system menu that is currently open.
    virtual void Apply(const std::vector<PinRecord>& pinned) = 0;
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
