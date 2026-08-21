#include "WinControlWindow.h"

#include "AppIdentity.h"
#include "AutoStart.h"
#include "Resource.h"

#include <shellapi.h>
#include <utility>
#include <cwchar>
#include <cstddef>
#include <iterator>

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

void WinControlWindow::SetPinState(bool enabled) {
    pinningEnabled_ = enabled;
}

void WinControlWindow::SetPinnedProvider(PinnedProvider provider) {
    pinnedProvider_ = std::move(provider);
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
    // Disabled header. It carries the app name so 关于 and 退出 do not have to repeat it,
    // which is what was making the menu wide - a Win32 menu is exactly as wide as its
    // longest label, and 「关于 WindowMark...」 was that label.
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, app::kProductName);
    // Bookmarks and borders are separate features with separate switches and separate
    // settings, so the menu keeps them in separate submenus rather than one flat list.
    HMENU bookmarks = CreatePopupMenu();
    if (bookmarks) {
        AppendMenuW(bookmarks, MF_STRING | (enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kToggleCommand, L"启用书签");
        // Shortened from 「选择参与的应用/窗口...」: at twelve glyphs it was the widest item
        // anywhere in the menu, and a submenu is right-aligned to the parent's left edge,
        // so it alone decided how far left the 书签 panel reached - visibly further than
        // the 窗口边框 panel next to it.
        AppendMenuW(bookmarks, MF_STRING, kSelectionCommand, L"选择应用/窗口...");
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

    HMENU pinning = CreatePopupMenu();
    if (pinning) {
        AppendMenuW(pinning, MF_STRING | (pinningEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kTogglePinningCommand, L"启用窗口置顶");
        if (pinningEnabled_) {
            AppendMenuW(pinning, MF_STRING, kPinForegroundCommand, L"置顶当前窗口");
        }

        // The pinned list is built fresh here, not cached: it changes whenever a window is
        // pinned, unpinned or closed, and the menu is the only thing that reads it.
        pinnedMenuWindows_.clear();
        if (pinningEnabled_ && pinnedProvider_) {
            const auto pinned = pinnedProvider_();
            if (!pinned.empty()) {
                AppendMenuW(pinning, MF_SEPARATOR, 0, nullptr);
                for (const auto& [id, title] : pinned) {
                    if (pinnedMenuWindows_.size() >= kPinnedWindowCommandLimit) break;
                    const UINT command = kPinnedWindowCommandBase +
                                         static_cast<UINT>(pinnedMenuWindows_.size());
                    AppendMenuW(pinning, MF_STRING | MF_CHECKED, command, title.c_str());
                    pinnedMenuWindows_.push_back(id);
                }
                AppendMenuW(pinning, MF_STRING, kUnpinAllCommand, L"全部取消置顶");
            }
        }

        AppendMenuW(pinning, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pinning, MF_STRING, kPinSettingsCommand, L"置顶设置...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pinning), L"窗口置顶");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Master switch above the per-feature ones: one click silences the whole app without
    // having to visit both submenus. The label names what the click will do rather than
    // what the state is - no tick to interpret, and two glyphs instead of five.
    const bool anythingOn = enabled_ || bordersEnabled_ || pinningEnabled_;
    AppendMenuW(menu, MF_STRING, kToggleAllCommand, anythingOn ? L"暂停所有" : L"启用所有");
    // Top level rather than inside either submenu: it switches the program, not a feature.
    // The tick is read from the registry every time the menu opens instead of being cached,
    // because Windows lets the user turn a startup entry off from Task Manager and from
    // 设置 - 应用 - 启动, and a cached copy would keep claiming the opposite.
    // 「开机启动」 rather than 「开机自启动」: with the app name moved into the header, this
    // was the widest label left, and a Win32 menu is exactly as wide as its widest label.
    AppendMenuW(menu, MF_STRING | (app::IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                kAutoStartCommand, L"开机启动");
    AppendMenuW(menu, MF_STRING, kAboutCommand, L"关于");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");
    SetForegroundWindow(hwnd_);
    // TPM_RIGHTALIGN puts the menu's *right* edge on the cursor, so it grows leftwards into
    // the free desktop instead of rightwards towards the screen edge. The tray sits at the
    // bottom right, so growing right meant the menu was always shoved back by the clamp and
    // ended up hard against the edge.
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
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
        // Handled here rather than routed to a handler like the rest: there is no
        // application state behind it and nothing to persist - the registry value *is* the
        // setting, so a trip out to WinMain and back would only add indirection.
        if (LOWORD(wParam) == kAutoStartCommand) {
            const bool turningOn = !app::IsAutoStartEnabled();
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe))) != 0) {
                if (turningOn) app::ClearAutoStartVeto();
                app::SetAutoStart(exe, turningOn);
            }
            return 0;
        }

        // Dynamic block: unpin one particular window. Handled before the fixed table
        // because these ids are allocated per menu build rather than being constants.
        {
            const UINT command = LOWORD(wParam);
            if (command >= kPinnedWindowCommandBase &&
                command < kPinnedWindowCommandBase + kPinnedWindowCommandLimit) {
                const std::size_t index = command - kPinnedWindowCommandBase;
                if (index < pinnedMenuWindows_.size() && handlers_.onTogglePinWindow) {
                    handlers_.onTogglePinWindow(pinnedMenuWindows_[index]);
                }
                return 0;
            }
        }

        const std::function<void()>* handler = nullptr;
        switch (LOWORD(wParam)) {
        case kToggleAllCommand:      handler = &handlers_.onToggleAll; break;
        case kToggleCommand:         handler = &handlers_.onToggleBookmarks; break;
        case kSelectionCommand:      handler = &handlers_.onSelection; break;
        case kSettingsCommand:       handler = &handlers_.onBookmarkSettings; break;
        case kToggleBordersCommand:  handler = &handlers_.onToggleBorders; break;
        case kBorderSettingsCommand: handler = &handlers_.onBorderSettings; break;
        case kTogglePinningCommand:  handler = &handlers_.onTogglePinning; break;
        case kPinForegroundCommand:  handler = &handlers_.onPinForeground; break;
        case kUnpinAllCommand:       handler = &handlers_.onUnpinAll; break;
        case kPinSettingsCommand:    handler = &handlers_.onPinSettings; break;
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
