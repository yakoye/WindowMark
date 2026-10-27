#include "WinImeTrayGuard.h"
#include "windowmark/core/ImeGuardPolicy.h"
#include <shellapi.h>
#include <wtsapi32.h>
#include <fstream>

namespace windowmark::win {
namespace {
constexpr wchar_t kClass[] = L"WindowMark.ImeTrayGuard.Backend";
constexpr wchar_t kVersionKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
}
WinImeTrayGuard::~WinImeTrayGuard() { Shutdown(); }
bool WinImeTrayGuard::Initialize(std::filesystem::path tools, std::filesystem::path data) {
    tools_ = std::move(tools); data_ = std::move(data); marker_ = data_ / L"active-session.lock";
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = WndProc;
    cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    // 隐藏顶层窗口接收 Explorer/显示器广播；message-only 窗口收不到广播。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP,
                           0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this);
    if (!hwnd_) return false;
    if (!SetTimer(hwnd_, 1, 250, nullptr) ||
        !WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION)) {
        message_ = L"无法建立恢复监视器，未启用保护。";
        DestroyWindow(hwnd_); hwnd_ = nullptr; return false;
    }
    return true;
}
std::wstring WinImeTrayGuard::Compatibility() const {
#ifdef WINDOWMARK_IME_GUARD_TESTING
    if (fixturePid_) return {};
#endif
    wchar_t buildText[32]{}; DWORD size = sizeof(buildText), revision = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"CurrentBuildNumber", RRF_RT_REG_SZ,
                     nullptr, buildText, &size) != ERROR_SUCCESS) return L"无法读取系统版本。";
    size = sizeof(revision);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"UBR", RRF_RT_REG_DWORD,
                     nullptr, &revision, &size) != ERROR_SUCCESS) return L"无法读取系统补丁版本。";
    const auto build = static_cast<unsigned>(wcstoul(buildText, nullptr, 10));
    if (!ImeGuardPlatformEligible(build)) {
        return std::wstring(L"Windows 10 适配已暂停，当前接口不支持该任务栏。系统：") +
               std::to_wstring(build) + L"." + std::to_wstring(revision);
    }
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return L"尚未找到原生任务栏。";
    return {};
}
void WinImeTrayGuard::SetRequested(bool value) {
    if (requested_ == value) return;
    requested_ = value;
    if (onRequestedChanged) onRequestedChanged(value);
}
bool WinImeTrayGuard::Enable(bool automatic) {
    if (!hwnd_ || shuttingDown_) { message_ = L"恢复监视器不可用，未启用。"; return false; }
    if (client_.Current().running) { message_ = L"等待上一会话恢复完成后再启用。"; return false; }
    recovering_ = false;
    const auto reason = Compatibility();
    if (!reason.empty()) { message_ = reason; SetRequested(false); return false; }
    std::error_code ec;
    if (automatic && (std::filesystem::exists(marker_, ec) || ec)) {
        message_ = L"上次恢复未确认，已阻止自动启用；请检查诊断后手动重试。";
        SetRequested(false); return false;
    }
    try {
        // Explorer 加载独立运行副本，避免安装目录 DLL 锁定阻塞升级/卸载。
        const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto runtime = data_ / L"runtime" / id;
        std::filesystem::create_directories(runtime);
        for (const auto* name : {L"WindowMarkXamlContainerTrial.exe", L"WindowMarkXamlContainerTap.dll"})
            std::filesystem::copy_file(tools_ / name, runtime / name);
        log_ = data_ / (id + L".jsonl");
        std::ofstream marker(marker_, std::ios::trunc);
        marker << "pid=" << GetCurrentProcessId() << "\n"; marker.close();
        if (!marker) throw std::runtime_error("marker");
        DWORD fixturePid = 0;
#ifdef WINDOWMARK_IME_GUARD_TESTING
        fixturePid = fixturePid_;
#endif
        if (!client_.Start(runtime, log_, fixturePid)) {
            message_ = client_.Current().error;
            // 不能确认失败发生在哪一步，保留标记供下次启动诊断。
            SetRequested(false); return false;
        }
        hadLease_ = true; message_ = L"已请求启用，正在核验原生容器；尚未生效。";
        taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
        taskbarDpi_ = taskbar_ ? GetDpiForWindow(taskbar_) : 0;
        taskbarMonitor_ = taskbar_ ? MonitorFromWindow(taskbar_, MONITOR_DEFAULTTONULL) : nullptr;
        SetRequested(true); return true;
    } catch (...) {
        message_ = L"准备原生组件或恢复标记失败，未启用。"; SetRequested(false); return false;
    }
}
bool WinImeTrayGuard::Active() const noexcept {
    const auto& s = client_.Current();
    const auto now = GetTickCount64();
    return requested_ && s.targetHeartbeat && s.targetHeartbeat <= now && now - s.targetHeartbeat <= 3000 &&
        ImeGuardActuallyActive(s.running, s.stopping, s.state, s.restoreFailures);
}
std::wstring WinImeTrayGuard::StatusText() const {
    const auto& s = client_.Current();
    if (s.running && s.stopping) return L"正在恢复系统默认布局；请稍候。";
    if (recovering_) return message_;
    if (Active()) return L"原生占位保护已生效：" + std::to_wstring(static_cast<int>(s.width)) +
        L" DIP；未添加替代图标。";
    if (requested_ && s.running && s.state == 2 && s.width == 0)
        return L"正在学习完整宽度；请让原生输入法图标显示一次，随后自动生效。";
    return message_;
}
void WinImeTrayGuard::Restore() {
    recovering_ = false;
    SetRequested(false);
    if (client_.Current().running) { client_.Stop(); message_ = L"正在恢复系统默认布局。"; }
    else {
        std::error_code ec;
        if (std::filesystem::exists(marker_, ec) || ec)
            message_ = L"没有活动控制会话，但上次恢复未确认；请查看日志，不重复连接 Explorer。";
        else message_ = L"已关闭；没有本进程正在修改的布局。";
    }
}
void WinImeTrayGuard::Pause(const wchar_t* reason) {
    recovering_ = false;
    if (!requested_ && !client_.Current().running) return;
    SetRequested(false); client_.Stop(); message_ = reason;
}
void WinImeTrayGuard::RecoverLayout(const wchar_t* reason) {
    if (!requested_ || shuttingDown_) return;
    recovering_ = true;
    recoverAt_ = GetTickCount64() + 2000;
    recoverDeadline_ = recoverAt_ + 30000;
    client_.Stop();
    message_ = reason;
}
void WinImeTrayGuard::Tick() {
    client_.Tick();
    const auto& s = client_.Current();
    if (s.running && s.restoreFailures) Pause(L"恢复校验异常；已停止续租，请查看日志。");
    if (!s.running && hadLease_) {
        hadLease_ = false;
        if (ImeGuardRestored(s.running, s.exitCode, s.state, s.writes, s.restores, s.restoreFailures)) {
            std::error_code ec; std::filesystem::remove(marker_, ec);
            if (ec) { recovering_ = false; message_ = L"布局已恢复，但恢复标记清理失败；请手动检查。"; }
            else if (!recovering_) message_ = L"已关闭，原生布局已确认恢复。";
        } else { recovering_ = false; message_ = L"保护已停止，恢复结果未确认；不自动重试。请查看诊断日志。"; }
        if (!recovering_) SetRequested(false);
    }
    if (recovering_ && requested_ && !s.running && !suspended_) {
        const auto now = GetTickCount64();
        if (now >= recoverDeadline_) {
            recovering_ = false; SetRequested(false);
            message_ = L"任务栏持续未就绪，自动恢复已停止；可手动重新启用。";
        } else if (now >= recoverAt_ && Compatibility().empty()) {
            // 只在原布局已确认恢复后重连一次；连接失败不循环重试。
            Enable(true);
        }
    }
    if (requested_ && s.running && !s.stopping && !recovering_) {
        const HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (bar && bar == taskbar_ &&
            (GetDpiForWindow(bar) != taskbarDpi_ || MonitorFromWindow(bar, MONITOR_DEFAULTTONULL) != taskbarMonitor_))
            RecoverLayout(L"任务栏缩放或所在显示器变化，正在自动重新学习宽度。");
    }
}
void WinImeTrayGuard::Check() {
    const auto reason = Compatibility();
    if (!client_.Current().running)
        message_ = reason.empty() ? L"已开放各种缩放与多屏下的主任务栏测试；启用时校验原生组件兼容性。" : reason;
}
void WinImeTrayGuard::OpenDiagnostics(HWND owner) const {
    std::error_code ec; std::filesystem::create_directories(data_, ec);
    if (!ec) ShellExecuteW(owner, L"open", data_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void WinImeTrayGuard::Shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    // 正常退出保留用户启用意图；只有失败或系统事件才自动清除配置。
    onRequestedChanged = {};
    Restore();
    const auto deadline = GetTickCount64() + 6500;
    while (client_.Current().running && GetTickCount64() < deadline) { Tick(); Sleep(20); }
    if (hwnd_) { WTSUnRegisterSessionNotification(hwnd_); DestroyWindow(hwnd_); hwnd_ = nullptr; }
}
LRESULT CALLBACK WinImeTrayGuard::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WinImeTrayGuard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<WinImeTrayGuard*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        if (msg == WM_TIMER) { self->Tick(); return 0; }
        if (msg == WM_DISPLAYCHANGE || msg == WM_DPICHANGED)
            self->RecoverLayout(L"显示布局变化，等待稳定后自动恢复保护。");
        if (msg == self->taskbarCreated_ && self->taskbarCreated_)
            self->Pause(L"Explorer 已重建任务栏，已暂停保护。");
        if (msg == WM_POWERBROADCAST && wp == PBT_APMSUSPEND) {
            self->suspended_ |= 4; self->RecoverLayout(L"系统休眠，唤醒后自动恢复保护。");
        }
        if (msg == WM_POWERBROADCAST && (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)) {
            self->suspended_ &= ~4U; self->RecoverLayout(L"系统已唤醒，等待布局稳定后恢复保护。");
        }
        if (msg == WM_WTSSESSION_CHANGE && (wp == WTS_SESSION_LOCK ||
            wp == WTS_REMOTE_DISCONNECT || wp == WTS_CONSOLE_DISCONNECT))
        {
            self->suspended_ |= wp == WTS_SESSION_LOCK ? 1U : 2U;
            self->RecoverLayout(L"会话锁定或断开，解锁或重连后自动恢复保护。");
        }
        if (msg == WM_WTSSESSION_CHANGE && (wp == WTS_SESSION_UNLOCK ||
            wp == WTS_REMOTE_CONNECT || wp == WTS_CONSOLE_CONNECT)) {
            self->suspended_ &= wp == WTS_SESSION_UNLOCK ? ~1U : ~2U;
            self->RecoverLayout(L"会话已恢复，等待布局稳定后恢复保护。");
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}
