#pragma once

#include "windowmark/core/Interfaces.h"
#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <windows.h>

#include <optional>
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
    void OnMenuOpened(HWND window);
    void OnMenuItemInvoked(HWND eventWindow, LONG commandId);
    void UpdateSystemMenuItem(HWND window) const;
    void RemoveSystemMenuItem(HWND window) const;
    [[nodiscard]] bool IsPinned(WindowId id) const;
    void InstallHooks();
    void RemoveHooks() noexcept;

    Settings settings_;
    PinCallbacks callbacks_;
    std::vector<PinRecord> pinned_;
    std::vector<HWINEVENTHOOK> hooks_;
    // The window whose system menu is open right now. EVENT_OBJECT_INVOKED does not always
    // name the window the menu belongs to, so this is what the click is resolved against.
    HWND menuWindow_{};
    bool started_{false};
};

} // namespace windowmark::win
