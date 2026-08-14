#pragma once

#include <functional>
#include <windows.h>

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
        std::function<void()> onAbout;
        std::function<void()> onExit;
    };

    bool Start(Handlers handlers);
    void Stop() noexcept;
    void SetEnabledState(bool enabled);
    void SetBorderState(bool enabled);
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
};

} // namespace windowmark::win
