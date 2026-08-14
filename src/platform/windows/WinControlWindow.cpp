#include "WinControlWindow.h"

#include "AppIdentity.h"
#include "Resource.h"

#include <shellapi.h>
#include <utility>
#include <cwchar>

namespace windowmark::win {
namespace {

constexpr const wchar_t* kControlClass = app::kControlWindowClass;
constexpr UINT kTrayId = 1;

} // namespace

WinControlWindow::~WinControlWindow() { Stop(); }

bool WinControlWindow::Start(Handlers handlers) {
    handlers_ = std::move(handlers);

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

    // Deliberately no global hotkey. A process-wide RegisterHotKey claims the combination
    // for the whole session, so whichever app asks first wins and the other silently loses
    // it - not a trade worth making for a switch that is two clicks away in the tray menu.
    AddTrayIcon();
    return true;
}

void WinControlWindow::Stop() noexcept {
    if (!hwnd_) return;
    RemoveTrayIcon();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void WinControlWindow::SetEnabledState(bool enabled) {
    enabled_ = enabled;
}

void WinControlWindow::SetBorderState(bool enabled) {
    bordersEnabled_ = enabled;
}

void WinControlWindow::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    // LoadImage rather than LoadIcon, asking for the small-icon metric: the .ico carries
    // several sizes and this picks the 16px one outright instead of squashing the 256.
    data.hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"WindowMark - 右键打开菜单");
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
    // Bookmarks and borders are separate features with separate switches and separate
    // settings, so the menu keeps them in separate submenus rather than one flat list.
    HMENU bookmarks = CreatePopupMenu();
    if (bookmarks) {
        AppendMenuW(bookmarks, MF_STRING | (enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kToggleCommand, L"启用书签");
        AppendMenuW(bookmarks, MF_STRING, kSelectionCommand, L"选择参与的应用/窗口...");
        AppendMenuW(bookmarks, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(bookmarks, MF_STRING, kSettingsCommand, L"书签设置...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bookmarks), L"书签");
    }

    HMENU borders = CreatePopupMenu();
    if (borders) {
        AppendMenuW(borders, MF_STRING | (bordersEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kToggleBordersCommand, L"启用窗口边框");
        AppendMenuW(borders, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(borders, MF_STRING, kBorderSettingsCommand, L"边框设置...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(borders), L"窗口边框");
    }

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
        if (handlers_.onExit) handlers_.onExit();
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
    case WM_COMMAND: {
        const std::function<void()>* handler = nullptr;
        switch (LOWORD(wParam)) {
        case kToggleCommand:         handler = &handlers_.onToggleBookmarks; break;
        case kSelectionCommand:      handler = &handlers_.onSelection; break;
        case kSettingsCommand:       handler = &handlers_.onBookmarkSettings; break;
        case kToggleBordersCommand:  handler = &handlers_.onToggleBorders; break;
        case kBorderSettingsCommand: handler = &handlers_.onBorderSettings; break;
        case kAboutCommand:          handler = &handlers_.onAbout; break;
        case kExitCommand:           handler = &handlers_.onExit; break;
        default: break;
        }
        if (handler) {
            if (*handler) (*handler)();
            return 0;
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace windowmark::win
