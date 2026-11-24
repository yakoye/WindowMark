#include "WinMouseControl.h"
#include "windowmark/core/WheelThrottle.h"
#include <future>
#include <fstream>
#include <algorithm>

namespace windowmark::win {
namespace {
constexpr ULONG_PTR kOwnWheel = 0x574D5748;
constexpr UINT kDeferredWheel = WM_APP + 513;
thread_local WheelThrottle gate;
thread_local unsigned wheelInterval{};
thread_local MouseWheelState* wheelContext{};
LRESULT CALLBACK WheelHook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && wheelContext && !wheelContext->stop && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        const auto delta = static_cast<short>(HIWORD(event.mouseData));
        const auto action = gate.Route(message == WM_MOUSEHWHEEL, delta, GetTickCount64(), wheelInterval, event.dwExtraInfo == kOwnWheel);
        if (action == WheelAction::Drop) return 1;
        if (action == WheelAction::NormalizeLater) {
            // 回调只排队后返回，绝不在钩子内部同步 SendInput。
            const auto flags = message == WM_MOUSEHWHEEL ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            const auto packed = MAKELPARAM(flags, static_cast<WORD>(delta > 0 ? WHEEL_DELTA : -WHEEL_DELTA));
            if (PostThreadMessageW(GetCurrentThreadId(), kDeferredWheel, packed, static_cast<LPARAM>(GetTickCount64()))) return 1;
            gate.Reset(); // 队列不可用则让原始输入继续。
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
    if (worker_.joinable()) {
        wheel_->stop = true;
        wheel_->active = false;
        if (const auto hook = wheel_->hook.exchange(nullptr)) UnhookWindowsHookEx(hook);
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        const auto handle = static_cast<HANDLE>(worker_.native_handle());
        const auto deadline = GetTickCount64() + 1500;
        while (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
            // 退出时仍处理同步线程调用，避免钩子链与 UI 互相等待。
            MsgWaitForMultipleObjectsEx(1, &handle, 25, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            MSG sent{}; PeekMessageW(&sent, nullptr, 0, 0, PM_NOREMOVE | (QS_SENDMESSAGE << 16));
        }
        if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) worker_.join();
        else worker_.detach(); // 独立共享状态保证迟退出线程不引用已销毁对象。
        wheel_.reset();
    }
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
            wheel_ = std::make_shared<MouseWheelState>();
            auto state = wheel_;
            try {
            worker_ = std::thread([state, next, ready = std::move(ready)]() mutable {
                MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                wheelContext = state.get(); wheelInterval = static_cast<unsigned>(next); gate.Reset();
                const auto hook = SetWindowsHookExW(WH_MOUSE_LL, WheelHook, GetModuleHandleW(nullptr), 0);
                state->hook = hook; state->active = hook != nullptr;
                ready.set_value(hook != nullptr);
                if (!hook) return;
                while (!state->stop && GetMessageW(&message, nullptr, 0, 0) > 0) {
                    if (message.message == kDeferredWheel) {
                        if (state->stop || GetTickCount64() - static_cast<ULONGLONG>(message.lParam) > 250) continue;
                        INPUT input{}; input.type = INPUT_MOUSE;
                        input.mi.dwFlags = LOWORD(message.wParam);
                        input.mi.mouseData = static_cast<DWORD>(static_cast<short>(HIWORD(message.wParam)));
                        input.mi.dwExtraInfo = kOwnWheel;
                        if (SendInput(1, &input, sizeof(input)) != 1) {
                            state->failed = true; state->stop = true;
                        }
                    } else { TranslateMessage(&message); DispatchMessageW(&message); }
                }
                if (const auto installed = state->hook.exchange(nullptr)) UnhookWindowsHookEx(installed);
                state->active = false; wheelContext = nullptr;
            });
            } catch (...) { error = L"滚轮工作线程未能启动。"; return false; }
            threadId_ = GetThreadId(static_cast<HANDLE>(worker_.native_handle()));
            const auto deadline = GetTickCount64() + 1500;
            while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && GetTickCount64() < deadline) {
                const auto handle = static_cast<HANDLE>(worker_.native_handle());
                MsgWaitForMultipleObjectsEx(1, &handle, 20, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
                MSG sent{}; PeekMessageW(&sent, nullptr, 0, 0, PM_NOREMOVE | (QS_SENDMESSAGE << 16));
            }
            if (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready || !result.get()) {
                StopWheel(); error = L"滚轮监听启动失败或超时，未启用滚轮调节。"; return false;
            }
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
