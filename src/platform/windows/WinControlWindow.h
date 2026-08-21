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
        // Crosshair grab. Preview fires as the cursor crosses windows, commit when the
        // user settles on one, cancel on right-click or if capture is taken away.
        std::function<void(WindowId)> onGrabPreview;
        std::function<void(WindowId)> onGrabCommit;
        std::function<void()> onGrabCancel;
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
    static constexpr UINT kPinSettingsCommand = 1013;
    static constexpr UINT kUnpinAllCommand = 1014;
    // Dynamic block: one command per currently pinned window, allocated when the menu is
    // built. Kept well clear of the fixed ids above so adding a fixed item never collides.
    static constexpr UINT kPinnedWindowCommandBase = 1100;
    static constexpr UINT kPinnedWindowCommandLimit = 64;
    // Safety net for the crosshair grab. While it is active this window holds the mouse
    // capture, so if it ever failed to end the user would be left clicking into nothing
    // with no obvious way out. Fifteen seconds is far longer than aiming at a window takes
    // and far shorter than anyone would spend wondering what broke.
    static constexpr UINT_PTR kGrabTimeoutTimer = 90;
    static constexpr UINT kGrabTimeoutMs = 15000;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void ShowMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowAlreadyRunningHint();

    // Crosshair grab, DeskPins style: point at a window and it gets pinned.
    //
    // Two ways in, one state machine. Dragging off the tray icon is the quick one; the
    // menu item is the reliable one, because a tray icon folded into the overflow flyout
    // is awkward to drag out of.
    enum class GrabState { None, PendingDrag, Grabbing };

    void BeginGrabFromMenu();
    void EndGrab(bool commit);
    void UpdateGrabTarget();
    [[nodiscard]] WindowId WindowUnderCursor() const;

    GrabState grabState_{GrabState::None};
    POINT grabStart_{};
    // The menu path arrives with no button held, so the click that selects a window has to
    // start with a press. Without this, the button-up that dismissed the menu would land
    // here and instantly pin whatever happened to be under the cursor.
    bool grabNeedsPress_{false};
    WindowId grabTarget_{};
    HCURSOR grabCursor_{};

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
