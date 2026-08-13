#include "WinControlWindow.h"

#include "AppIdentity.h"

#include <shellapi.h>
#include <utility>
#include <cwchar>

namespace windowmark::win {
namespace {

constexpr const wchar_t* kControlClass = app::kControlWindowClass;
constexpr UINT kTrayId = 1;

} // namespace

WinControlWindow::~WinControlWindow() { Stop(); }

bool WinControlWindow::Start(
    std::function<void()> onToggle,
    std::function<void()> onSelection,
    std::function<void()> onSettings,
    std::function<void()> onAbout,
    std::function<void()> onExit) {
    onToggle_ = std::move(onToggle);
    onSelection_ = std::move(onSelection);
    onSettings_ = std::move(onSettings);
    onAbout_ = std::move(onAbout);
    onExit_ = std::move(onExit);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kControlClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    hwnd_ = CreateWindowExW(0, kControlClass, L"WindowMark", WS_OVERLAPPED,
                            0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;

    // Registered messages let the installer/uninstaller and a second launch reach this
    // instance without either side treating "already running" as a failure.
    requestQuitMessage_ = RegisterWindowMessageW(app::kRequestQuitMessage);
    secondInstanceMessage_ = RegisterWindowMessageW(app::kSecondInstanceMessage);

    AddTrayIcon();
    RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    return true;
}

void WinControlWindow::Stop() noexcept {
    if (!hwnd_) return;
    UnregisterHotKey(hwnd_, kHotkeyId);
    RemoveTrayIcon();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void WinControlWindow::SetEnabledState(bool enabled) {
    enabled_ = enabled;
}

void WinControlWindow::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"WindowMark - Ctrl+Alt+W 显示/隐藏书签");
    Shell_NotifyIconW(NIM_ADD, &data);
}

void WinControlWindow::ShowAlreadyRunningHint() {
    if (!hwnd_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcscpy_s(data.szInfoTitle, L"WindowMark 已在运行");
    wcscpy_s(data.szInfo, L"书签层已经启用。右键此图标可以隐藏书签或选择参与的应用。");
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void WinControlWindow::RemoveTrayIcon() {
    if (!hwnd_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void WinControlWindow::ShowMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kToggleCommand, enabled_ ? L"隐藏书签\tCtrl+Alt+W" : L"显示书签\tCtrl+Alt+W");
    AppendMenuW(menu, MF_STRING, kSelectionCommand, L"选择需要书签的应用/窗口...");
    AppendMenuW(menu, MF_STRING, kSettingsCommand, L"设置...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAboutCommand, L"关于 WindowMark...");
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出 WindowMark");
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WinControlWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<WinControlWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WinControlWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT WinControlWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // Registered message ids are runtime values, so they cannot be switch labels.
    if (requestQuitMessage_ != 0 && msg == requestQuitMessage_) {
        if (onExit_) onExit_();
        return 0;
    }
    if (secondInstanceMessage_ != 0 && msg == secondInstanceMessage_) {
        ShowAlreadyRunningHint();
        return 0;
    }

    switch (msg) {
    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowMenu();
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wParam == kHotkeyId && onToggle_) {
            onToggle_();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == kToggleCommand && onToggle_) {
            onToggle_();
            return 0;
        }
        if (LOWORD(wParam) == kSelectionCommand && onSelection_) {
            onSelection_();
            return 0;
        }
        if (LOWORD(wParam) == kSettingsCommand && onSettings_) {
            onSettings_();
            return 0;
        }
        if (LOWORD(wParam) == kAboutCommand && onAbout_) {
            onAbout_();
            return 0;
        }
        if (LOWORD(wParam) == kExitCommand && onExit_) {
            onExit_();
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace windowmark::win
