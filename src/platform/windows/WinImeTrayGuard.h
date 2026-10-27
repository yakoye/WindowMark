#pragma once
#include "ImeGuardClient.h"
#include <functional>

namespace windowmark::win {
// 自己的窗口定时器在主面板的模态循环中同样运行；UI 卡住会停止续租。
class WinImeTrayGuard {
public:
    ~WinImeTrayGuard();
    bool Initialize(std::filesystem::path tools, std::filesystem::path data);
    bool Enable(bool automatic = false);
    void Restore();
    void Shutdown();
    void Check();
    void SetHints(bool enabled) { client_.SetHints(enabled); }
    void OpenDiagnostics(HWND owner) const;
    [[nodiscard]] bool Requested() const noexcept { return requested_; }
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] std::wstring StatusText() const;
    std::function<void(bool)> onRequestedChanged;
#ifdef WINDOWMARK_IME_GUARD_TESTING
    void UseFixture(DWORD pid) { fixturePid_ = pid; }
    HWND TestWindow() const { return hwnd_; }
#endif
private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Tick();
    void Pause(const wchar_t* reason);
    void RecoverLayout(const wchar_t* reason);
    void SetRequested(bool value);
    [[nodiscard]] std::wstring Compatibility() const;
    ime_guard::Client client_;
    HWND hwnd_{};
    UINT taskbarCreated_{};
    std::filesystem::path tools_, data_, marker_, log_;
    std::wstring message_{L"已关闭；不改变原生输入法及任务栏布局。"};
    bool requested_{}, hadLease_{}, shuttingDown_{};
    bool recovering_{};
    unsigned suspended_{};
    ULONGLONG recoverAt_{}, recoverDeadline_{};
    HWND taskbar_{};
    UINT taskbarDpi_{};
    HMONITOR taskbarMonitor_{};
#ifdef WINDOWMARK_IME_GUARD_TESTING
    DWORD fixturePid_{};
#endif
};
}
