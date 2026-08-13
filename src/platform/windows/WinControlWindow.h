#pragma once

#include <functional>
#include <windows.h>

namespace windowmark::win {

class WinControlWindow {
public:
    ~WinControlWindow();

    bool Start(
        std::function<void()> onToggle,
        std::function<void()> onSelection,
        std::function<void()> onSettings,
        std::function<void()> onAbout,
        std::function<void()> onExit);
    void Stop() noexcept;
    void SetEnabledState(bool enabled);
    [[nodiscard]] HWND NativeHandle() const noexcept { return hwnd_; }

private:
    static constexpr UINT kTrayMessage = WM_APP + 70;
    static constexpr UINT kToggleCommand = 1001;
    static constexpr UINT kSelectionCommand = 1002;
    static constexpr UINT kSettingsCommand = 1003;
    static constexpr UINT kAboutCommand = 1004;
    static constexpr UINT kExitCommand = 1005;
    static constexpr int kHotkeyId = 1;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void ShowMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowAlreadyRunningHint();

    HWND hwnd_{};
    UINT requestQuitMessage_{};
    UINT secondInstanceMessage_{};
    std::function<void()> onToggle_;
    std::function<void()> onSelection_;
    std::function<void()> onSettings_;
    std::function<void()> onAbout_;
    std::function<void()> onExit_;
    bool enabled_{true};
};

} // namespace windowmark::win
