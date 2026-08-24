#pragma once

#include "windowmark/core/Interfaces.h"
#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <windows.h>

#include <optional>
#include <vector>
#include <vector>

namespace windowmark::win {

// Always-on-top for other processes' windows, plus the title-bar entry point.
//
// Deliberately does not render anything. The outline a pinned window gets comes from the
// border backend, which is already the one thing on this desktop that draws outlines - a
// second renderer would mean deciding, every frame, which of the two owns a window that is
// both active and pinned.
class WinPinBackend final : public IPinBackend {
public:
    ~WinPinBackend() override;

    bool Start(const Settings& settings, PinCallbacks callbacks) override;
    std::optional<bool> SetTopmost(WindowId id, bool topmost) override;
    void Apply(const std::vector<PinRecord>& pinned) override;
    void UpdateSettings(const Settings& settings) override;
    void Stop() noexcept override;

private:
    // Adding an item to another process's system menu is allowed; receiving the click is
    // the hard half, because WM_SYSCOMMAND goes to that process's own window procedure and
    // never comes near this one. The way through without injecting anything is
    // EVENT_OBJECT_INVOKED, whose idChild carries the menu command id.
    static void CALLBACK HookProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                  LONG idChild, DWORD, DWORD);
    // Called both when a window becomes foreground (early, so the item is in place before
    // any right-click) and when a menu opens (to refresh the tick).
    // remember=false for the startup sweep: it must not overwrite lastTouchedWindow_, which
    // is the first candidate a menu click is resolved against.
    void EnsureSystemMenuItem(HWND window, bool remember = true);
    void SeedExistingWindows();
    void OnMenuItemInvoked(HWND eventWindow, LONG commandId);
    void UpdateSystemMenuItem(HWND window);
    void RemoveSystemMenuItem(HWND window) const;
    [[nodiscard]] bool IsPinned(WindowId id) const;
    void InstallHooks();
    void RemoveHooks() noexcept;

    Settings settings_;
    PinCallbacks callbacks_;
    std::vector<PinRecord> pinned_;
    std::vector<HWINEVENTHOOK> hooks_;
    // The window this last put the item on. EVENT_OBJECT_INVOKED does not reliably name
    // the window whose menu it was, so this is the first candidate the click is resolved
    // against. Never cleared: the best guess for "whose menu was that" is the window most
    // recently touched, and a stale value costs nothing - the resolver verifies each
    // candidate actually carries our item before acting on it.
    HWND lastTouchedWindow_{};
    // Every window this has put the item into, so they can all be cleaned up on the way
    // out. A menu item left behind after the process is gone still looks clickable and
    // does nothing - which is exactly the confusion a stale PowerToys entry caused here.
    std::vector<HWND> touchedWindows_;
    bool started_{false};
};

} // namespace windowmark::win
