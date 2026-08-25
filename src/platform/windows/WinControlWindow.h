#pragma once

#include "windowmark/core/Hotkey.h"
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
        // 窗口边框 -> 排除应用. Lives in the menu next to the on/off switch rather than
        // inside the settings window, matching where 书签 keeps its own app picker: both
        // answer "which windows does this feature apply to", which is a different question
        // from the numbers on the settings page.
        std::function<void()> onBorderExcludeApps;
        std::function<void()> onTogglePinning;
        std::function<void(WindowId)> onTogglePinWindow;
        // Crosshair grab. Preview fires as the cursor crosses windows, commit when the
        // user settles on one, cancel on right-click or if capture is taken away.
        std::function<void(WindowId)> onGrabPreview;
        std::function<void(WindowId)> onGrabCommit;
        std::function<void()> onGrabCancel;
        // Whether a window can be pinned at all. Without this the crosshair would commit
        // whatever WindowFromPoint returned, and anything that appears under the cursor
        // mid-aim - a tooltip, a floating toolbar, the taskbar sliding up - silently
        // becomes the target and the pin does nothing.
        std::function<bool(WindowId)> isPinnable;
        std::function<void()> onUnpinAll;
        // The global shortcut. Unlike the tray menu this does not steal the foreground,
        // so the window the user is looking at is still the foreground window when it
        // fires - which is the whole reason a shortcut can pin "the current window" and
        // a menu item cannot.
        std::function<void()> onPinHotkey;
        std::function<void()> onPinSettings;
        std::function<void()> onAbout;
        std::function<void()> onExit;
    };

    bool Start(Handlers handlers);
    void Stop() noexcept;
    void SetEnabledState(bool enabled);
    void SetBorderState(bool enabled);
    void SetPinState(bool enabled);
    // Registers `hotkey`, replacing whatever was registered before. An empty Hotkey just
    // unregisters. Returns false when Windows refused the combination - always because
    // another process already owns it, since RegisterHotKey is first-come, first-served
    // for the whole session. The caller is expected to say so out loud: a shortcut that
    // silently does nothing is worse than not offering one.
    bool SetPinHotkey(const Hotkey& hotkey);
    [[nodiscard]] Hotkey CurrentPinHotkey() const noexcept { return pinHotkey_; }
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
    static constexpr UINT kBorderExcludeCommand = 1015;
    static constexpr UINT kAutoStartCommand = 1008;
    static constexpr UINT kToggleAllCommand = 1009;
    static constexpr UINT kTogglePinningCommand = 1010;
    static constexpr UINT kGrabToPinCommand = 1011;
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
    // Only one hotkey, so a fixed id is enough. Ids are per-window, not global.
    static constexpr int kPinHotkeyId = 1;
    static constexpr UINT_PTR kGrabTimeoutTimer = 90;
    static constexpr UINT kGrabTimeoutMs = 15000;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void ShowMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowAlreadyRunningHint();

    // Crosshair grab, Spy++ style: the menu puts up a draggable crosshair, the user drags
    // it onto a window, and whatever it is released over gets pinned.
    //
    // One way in, on purpose. Dragging off the tray icon was tried and dropped: the icon
    // is often folded into the overflow flyout, and starting a drag there meant an
    // ordinary left click on the icon had to be told apart from the beginning of a
    // gesture. The handle also fixes the real defect - it is a visible window of ours, so
    // pressing on it makes this process foreground and hands it the mouse, and SetCapture
    // cannot fail. The old menu path called SetForegroundWindow on the hidden 0x0 control
    // window instead, which a background process is allowed to lose.
    enum class GrabState { None, Grabbing };

    void BeginGrabFromMenu();
    void ShowGrabHandle();
    void DestroyGrabHandle() noexcept;
    void StartGrabFromHandle();
    static LRESULT CALLBACK GrabHandleProc(HWND, UINT, WPARAM, LPARAM);
    void PaintGrabHandle(HWND) const;
    void EndGrab(bool commit);
    void UpdateGrabTarget();
    [[nodiscard]] WindowId WindowUnderCursor() const;

    GrabState grabState_{GrabState::None};
    WindowId grabTarget_{};
    HCURSOR grabCursor_{};
    // The draggable crosshair. Only exists between the menu item and the release.
    HWND grabHandle_{};
    // Whichever window holds the mouse capture for the grab in progress. Stored rather
    // than assumed, so releasing it cannot get out of step with taking it.
    HWND grabCapture_{};

    HWND hwnd_{};
    UINT requestQuitMessage_{};
    UINT secondInstanceMessage_{};
    Handlers handlers_;
    bool enabled_{true};
    bool bordersEnabled_{false};
    bool pinningEnabled_{true};
    PinnedProvider pinnedProvider_;
    // What is currently registered, so a settings change can unregister the old one before
    // claiming the new one. Empty means nothing is registered.
    Hotkey pinHotkey_{};
    // Index -> WindowId for the dynamic block, rebuilt every time the menu opens.
    std::vector<WindowId> pinnedMenuWindows_;
};

} // namespace windowmark::win
