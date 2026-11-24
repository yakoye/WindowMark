#pragma once
#include "windowmark/core/Settings.h"
#include <windows.h>
#include <filesystem>
#include <thread>
#include <string>
#include <atomic>
namespace windowmark::win {
class WinMouseControl {
public:
    explicit WinMouseControl(std::filesystem::path directory) : journal_(directory / L"mouse-speed.restore") {}
    ~WinMouseControl() { StopWheel(); RestorePointer(); }
    bool Apply(const MouseSettings& settings, std::wstring& error);
    bool Recover(std::wstring& error);
    bool WheelActive() const { return wheelActive_.load(); }
    bool PointerActive() const { return original_ != 0; }
private:
    bool RestorePointer();
    void StopWheel();
    std::filesystem::path journal_;
    int original_{}, applied_{};
    std::thread worker_;
    DWORD threadId_{};
    int interval_{};
    std::atomic<bool> wheelActive_{};
};
bool ShowMouseSettings(HWND owner, MouseSettings& settings);
}
