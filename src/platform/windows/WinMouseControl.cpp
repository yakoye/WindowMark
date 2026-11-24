#include "WinMouseControl.h"
#include "windowmark/core/WheelThrottle.h"
#include <future>
#include <fstream>
#include <algorithm>

namespace windowmark::win {
namespace {
constexpr ULONG_PTR kOwnWheel = 0x574D5748;
thread_local WheelThrottle gate;
thread_local unsigned wheelInterval{};
LRESULT CALLBACK WheelHook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        // 远程输入可能也是 injected，只有本模块重发的事件跳过。
        if (event.dwExtraInfo != kOwnWheel) {
            const auto delta = static_cast<short>(HIWORD(event.mouseData));
            if (!gate.Accept(message == WM_MOUSEHWHEEL, delta, GetTickCount64(), wheelInterval)) return 1;
            INPUT input{}; input.type = INPUT_MOUSE;
            input.mi.dwFlags = message == WM_MOUSEHWHEEL ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(delta > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
            input.mi.dwExtraInfo = kOwnWheel;
            // 保留按键状态；发送失败时放过原始事件，不吞掉用户输入。
            if (SendInput(1, &input, sizeof(input)) == 1) return 1;
            gate.Reset();
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
bool ReadSpeed(int& speed) { return SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0) != FALSE; }
bool WriteSpeed(int speed) {
    return SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<INT_PTR>(speed)), 0) != FALSE;
}
}
void WinMouseControl::StopWheel() {
    if (worker_.joinable()) { PostThreadMessageW(threadId_, WM_QUIT, 0, 0); worker_.join(); }
    threadId_ = 0; interval_ = 0;
}
bool WinMouseControl::RestorePointer() {
    if (!original_) return true;
    int current{};
    if (!ReadSpeed(current)) return false;
    // 用户或其他程序已改变速度时不覆盖它。
    if (current == applied_ && !WriteSpeed(original_)) return false;
    std::error_code ec;
    std::filesystem::remove(journal_, ec);
    if (ec) return false;
    original_ = applied_ = 0;
    return true;
}
bool WinMouseControl::Recover(std::wstring& error) {
    if (original_) return true;
    std::error_code ec;
    if (!std::filesystem::exists(journal_, ec)) {
        if (ec) error = L"无法读取指针恢复记录目录。";
        return !ec;
    }
    std::ifstream input(journal_);
    if (!(input >> original_ >> applied_) || original_ < 1 || original_ > 20 || applied_ < 1 || applied_ > 20) {
        original_ = applied_ = 0;
        error = L"指针恢复记录不可读，请检查程序目录的 mouse-speed.restore。"; return false;
    }
    input.close(); // Windows 读取句柄必须先关闭，才能删除已完成的恢复记录。
    if (!RestorePointer()) { error = L"原指针速度未能恢复，请重试。"; return false; }
    return true;
}
bool WinMouseControl::Apply(const MouseSettings& settings, std::wstring& error) {
    if (!RestorePointer()) { error = L"恢复原指针速度失败。"; return false; }
    const int next = settings.wheelEnabled ? std::clamp(settings.wheelIntervalMs, 1, 1000) : 0;
    if (next != interval_ || (next && !WheelActive())) {
        StopWheel();
        if (next) {
            std::promise<bool> ready; auto result = ready.get_future();
            try {
            worker_ = std::thread([this, next, ready = std::move(ready)]() mutable {
                MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                threadId_ = GetCurrentThreadId(); wheelInterval = static_cast<unsigned>(next); gate.Reset();
                const auto hook = SetWindowsHookExW(WH_MOUSE_LL, WheelHook, GetModuleHandleW(nullptr), 0);
                wheelActive_ = hook != nullptr;
                ready.set_value(hook != nullptr);
                if (!hook) return;
                while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
                UnhookWindowsHookEx(hook);
                wheelActive_ = false;
            });
            } catch (...) { error = L"滚轮工作线程未能启动。"; return false; }
            if (!result.get()) { StopWheel(); error = L"滚轮监听启动失败，未启用滚轮调节。"; return false; }
            interval_ = next;
        }
    }
    if (settings.pointerEnabled) {
        if (!ReadSpeed(original_)) { error = L"读取系统指针速度失败。"; return false; }
        applied_ = std::clamp(settings.pointerSpeed, 1, 20);
        const auto pending = journal_.wstring() + L".tmp";
        std::ofstream record(pending, std::ios::trunc);
        record << original_ << ' ' << applied_; record.close();
        if (!record || !MoveFileExW(pending.c_str(), journal_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) || !WriteSpeed(applied_)) {
            DeleteFileW(pending.c_str());
            RestorePointer(); error = L"保存恢复记录或设置指针速度失败。"; return false;
        }
    }
    return true;
}
}
