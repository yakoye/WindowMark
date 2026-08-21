#pragma once

#include "windowmark/core/Interfaces.h"
#include "windowmark/core/PinRegistry.h"
#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace windowmark {

class Coordinator {
public:
    Coordinator(
        Settings settings,
        IWindowBackend& windowBackend,
        IOverlayBackend& overlayBackend,
        IPreviewBackend& previewBackend,
        IBorderBackend* borderBackend = nullptr,
        IPinBackend* pinBackend = nullptr);

    bool Start();
    void Stop() noexcept;
    // Backed by the setting rather than a separate runtime flag, so the tray toggle and
    // the settings checkbox cannot disagree and the choice survives a restart.
    void SetOverlayEnabled(bool enabled);
    [[nodiscard]] bool OverlayEnabled() const noexcept { return settings_.drawer.enabled; }

    // Pinning. Every entry point in the platform layer routes here, so there is exactly
    // one place that decides whether a window is pinned.
    void TogglePin(WindowId id);
    void UnpinAll();
    [[nodiscard]] std::vector<PinRecord> PinnedWindows() const { return pins_.Snapshot(); }
    [[nodiscard]] bool IsPinned(WindowId id) const { return pins_.Contains(id); }
    // The window the user last worked in. Not GetForegroundWindow() at click time: opening
    // the tray menu makes the tray window itself the foreground one.
    // Rendered exactly like a real pin - same colour, same width - because that is the
    // question being answered: what will this look like if I let go here. Pass 0 to clear.
    void SetPinPreview(WindowId id);
    // Title for the tray submenu. Empty when the window is no longer tracked.
    [[nodiscard]] std::string PinnedTitle(WindowId id) const;

    // Selection is intentionally exposed as platform-neutral data so each OS can
    // build its own native settings UI without pulling platform types into Core.
    [[nodiscard]] std::vector<AppSelectionModel> SelectionSnapshot() const;
    void ApplySelection(const std::vector<AppSelectionModel>& selection);
    [[nodiscard]] const Settings& CurrentSettings() const noexcept { return settings_; }

    // Pushes edited settings to the backends and redraws, so the settings UI does not
    // have to restart anything.
    void UpdateSettings(Settings settings);

    // Custom bookmark labels. Deliberately session-only, for the same reason per-window
    // selection is: a generic OS window has no reliable cross-session identity, so a
    // persisted name would eventually attach itself to the wrong window.
    void SetCustomLabel(WindowId id, std::string label);
    [[nodiscard]] std::string CustomLabel(WindowId id) const;
    [[nodiscard]] std::string DefaultLabel(WindowId id) const;

    // Actions raised from a bookmark's context menu. The platform layer owns the input
    // and settings UI; set these before Start().
    void SetMenuHandlers(std::function<void(WindowId)> onRename, std::function<void()> onOpenSettings);

private:
    void OnWindowEvent(const WindowEvent& event);
    void RefreshAll();
    void RefreshOne(WindowId id);
    // Cheap path for location events: updates the frame and nothing else.
    void RefreshGeometry(WindowId id);
    void ApplyModels();
    void ApplyBorders();
    void ApplyPins();
    // Drops pins for windows that are gone, restoring nothing - they took their state
    // with them.
    void PrunePins();
    [[nodiscard]] std::vector<OverlayModel> BuildModels();
    // Borders cover every tracked top-level window, with no grouping: a single-window app
    // gets a border even though it never gets a bookmark strip.
    [[nodiscard]] std::vector<BorderModel> BuildBorderModels() const;
    [[nodiscard]] Color ColorFor(WindowId id);
    [[nodiscard]] bool IsAppEnabled(const std::string& groupKey) const;
    [[nodiscard]] bool IsWindowEnabled(WindowId id) const;
    void PruneTransientState();

    Settings settings_;
    IWindowBackend& windowsBackend_;
    IOverlayBackend& overlaysBackend_;
    IPreviewBackend& previewBackend_;
    IBorderBackend* borderBackend_{};
    IPinBackend* pinBackend_{};

    std::unordered_map<WindowId, WindowInfo> windows_;
    std::unordered_map<WindowId, std::size_t> stableOrder_;
    std::unordered_map<WindowId, std::size_t> colorSlots_;
    std::unordered_set<WindowId> disabledWindowIds_;
    std::unordered_map<WindowId, std::string> customLabels_;
    PinRegistry pins_;
    std::function<void(WindowId)> onRename_;
    std::function<void()> onOpenSettings_;
    std::size_t nextStableOrder_{0};
    std::size_t nextColorSlot_{0};
    WindowId activeWindow_{0};
    WindowId pinPreview_{0};
    bool started_{false};
};

} // namespace windowmark
