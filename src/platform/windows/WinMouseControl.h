#pragma once
#include "windowmark/core/Settings.h"
#include <windows.h>
#include <filesystem>
#include <thread>
#include <string>
#include <atomic>
#include <memory>
namespace windowmark::win {
struct MouseWheelState {
    std::atomic<bool> active{}, stop{}, failed{};
    std::atomic<HHOOK> hook{};
};
class WinMouseControl {
public:
    explicit WinMouseControl(std::filesystem::path directory) : journal_(directory / L"mouse-speed.restore") {}
    ~WinMouseControl() { StopWheel(); RestorePointer(); }
    bool Apply(const MouseSettings& settings, std::wstring& error);
    bool Recover(std::wstring& error);
    bool WheelActive() const { return wheel_ && wheel_->active.load(); }
    bool PointerActive() const { return original_ != 0; }
    bool WheelFailed() const { return wheel_ && wheel_->failed.load(); }
private:
    friend struct MouseControlTestAccess;
    bool RestorePointer();
    void StopWheel();
    std::filesystem::path journal_;
    int original_{}, applied_{};
    std::thread worker_;
    DWORD threadId_{};
    int interval_{};
    std::shared_ptr<MouseWheelState> wheel_;
};
bool ShowMouseSettings(HWND owner, MouseSettings& settings);
}
