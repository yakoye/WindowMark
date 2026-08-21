#pragma once

#include "windowmark/core/Types.h"

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace windowmark::win {

class WinControlWindow {
public:
    ~WinControlWindow();

    struct Handlers {
        // Master switch: turns both features off, or both back on.
        std::function<void()> onToggleAll;
        std::function<void()> onToggleBookmarks;
        std::function<void()> onSelection;
        std::function<void()> onBookmarkSettings;
        std::function<void()> onToggleBorders;
        std::function<void()> onBorderSettings;
        std::function<void()> onTogglePinning;
        std::function<void(WindowId)> onTogglePinWindow;
        std::function<void()> onPinForeground;
        std::function<void()> onUnpinAll;
        std::function<void()> onPinSettings;
        std::function<void()> onAbout;
        std::function<void()> onExit;
    };

    bool Start(Handlers handlers);
    void Stop() noexcept;
    void SetEnabledState(bool enabled);
    void SetBorderState(bool enabled);
    void SetPinState(bool enabled);
    // The pinned list is read when the menu opens rather than pushed on every change:
    // it changes far more often than the menu is looked at, and a stale copy here would
    // be a second source of truth for something the Coordinator already owns.
    using PinnedProvider = std::function<std::vector<std::pair<WindowId, std::wstring>>()>;
    void SetPinnedProvider(PinnedProvider provider);
    [[nodiscard]] HWND NativeHandle() const noexcept { return hwnd_; }

private:
    static constexpr UINT kTrayMessage = WM_APP + 70;
    static constexpr UINT kToggleCommand = 1001;
    static constexpr UINT kSelectionCommand = 1002;
    static constexpr UINT kSettingsCommand = 1003;
    static constexpr UINT kAboutCommand = 1004;
    static constexpr UINT kExitCommand = 1005;
    static constexpr UINT kToggleBordersCommand = 1006;
    static constexpr UINT kBorderSettingsCommand = 1007;
    static constexpr UINT kAutoStartCommand = 1008;
    static constexpr UINT kToggleAllCommand = 1009;
    static constexpr UINT kTogglePinningCommand = 1010;
    static constexpr UINT kGrabToPinCommand = 1011;
    static constexpr UINT kPinForegroundCommand = 1012;
    static constexpr UINT kUnpinAllCommand = 1014;
    static constexpr UINT kPinSettingsCommand = 1013;
    // Dynamic block: one command per currently pinned window, allocated when the menu is
    // built. Kept well clear of the fixed ids above so adding a fixed item never collides.
    static constexpr UINT kPinnedWindowCommandBase = 1100;
    static constexpr UINT kPinnedWindowCommandLimit = 64;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void ShowMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowAlreadyRunningHint();

    HWND hwnd_{};
    UINT requestQuitMessage_{};
    UINT secondInstanceMessage_{};
    Handlers handlers_;
    bool enabled_{true};
    bool bordersEnabled_{false};
    bool pinningEnabled_{true};
    PinnedProvider pinnedProvider_;
    // Index -> WindowId for the dynamic block, rebuilt every time the menu opens.
    std::vector<WindowId> pinnedMenuWindows_;
};

} // namespace windowmark::win
