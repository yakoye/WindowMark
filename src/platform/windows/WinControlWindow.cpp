#include "WinControlWindow.h"

#include "AppIdentity.h"
#include "AutoStart.h"
#include "PinDiag.h"
#include "Resource.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <utility>
#include <tuple>
#include <cwchar>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <iterator>

namespace windowmark::win {
namespace {

constexpr const wchar_t* kControlClass = app::kControlWindowClass;
// The "WindowMark." prefix matters: IsOwnWindowClass below keys off it, and that is what
// keeps the handle from ever becoming its own grab target.
constexpr wchar_t kGrabHandleClass[] = L"WindowMark.GrabHandle";

// Unscaled layout for the handle, at 96 dpi.
constexpr int kHandleW = 196;
constexpr int kHandleH = 58;
constexpr int kHandlePad = 10;
constexpr int kHandleCross = 38;
constexpr UINT kTrayId = 1;

// Anything of ours is not a pin target: the outlines and bookmark strips sit right on top
// of the windows being aimed at, and WindowFromPoint would happily return one of them.
bool IsOwnWindowClass(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0) return false;
    return wcsncmp(cls, L"WindowMark.", 11) == 0;
}

} // namespace

WinControlWindow::~WinControlWindow() { Stop(); }

bool WinControlWindow::Start(Handlers handlers) {
    handlers_ = std::move(handlers);

    // Named one by one on purpose. A missing handler is invisible from the outside - the
    // menu item is there, the click dispatches, nothing happens - so the only way to catch
    // it is to say so at startup rather than wait for someone to notice.
    const std::pair<const wchar_t*, bool> wired[] = {
        {L"onToggleAll", static_cast<bool>(handlers_.onToggleAll)},
        {L"onToggleBookmarks", static_cast<bool>(handlers_.onToggleBookmarks)},
        {L"onSelection", static_cast<bool>(handlers_.onSelection)},
        {L"onBookmarkSettings", static_cast<bool>(handlers_.onBookmarkSettings)},
        {L"onToggleBorders", static_cast<bool>(handlers_.onToggleBorders)},
        {L"onBorderSettings", static_cast<bool>(handlers_.onBorderSettings)},
        {L"onBorderExcludeApps", static_cast<bool>(handlers_.onBorderExcludeApps)},
        {L"onTogglePinning", static_cast<bool>(handlers_.onTogglePinning)},
        {L"onTogglePinWindow", static_cast<bool>(handlers_.onTogglePinWindow)},
        {L"onGrabPreview", static_cast<bool>(handlers_.onGrabPreview)},
        {L"onGrabCommit", static_cast<bool>(handlers_.onGrabCommit)},
        {L"onGrabCancel", static_cast<bool>(handlers_.onGrabCancel)},
        {L"isPinnable", static_cast<bool>(handlers_.isPinnable)},
        {L"onUnpinAll", static_cast<bool>(handlers_.onUnpinAll)},
        {L"onPinSettings", static_cast<bool>(handlers_.onPinSettings)},
        {L"onPinHotkey", static_cast<bool>(handlers_.onPinHotkey)},
        {L"onConfigPath", static_cast<bool>(handlers_.onConfigPath)},
        {L"onAbout", static_cast<bool>(handlers_.onAbout)},
        {L"onExit", static_cast<bool>(handlers_.onExit)},
    };
    for (const auto& [name, set] : wired) {
        if (!set) PinDiag(L"处理器未接上: %s", name);
    }

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

    // No hotkey is claimed here. RegisterHotKey takes a combination away from every other
    // program for the whole session and the loser is not told, so this app only asks once
    // the user has actually chosen one - see SetPinHotkey.
    AddTrayIcon();
    return true;
}

void WinControlWindow::Stop() noexcept {
    if (!hwnd_) return;
    DestroyGrabHandle();
    if (!pinHotkey_.Empty()) {
        UnregisterHotKey(hwnd_, kPinHotkeyId);
        pinHotkey_ = {};
    }
    RemoveTrayIcon();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

bool WinControlWindow::SetPinHotkey(const Hotkey& hotkey) {
    if (!hwnd_) return false;
    if (hotkey == pinHotkey_) return true;   // nothing to do; re-registering would fail

    if (!pinHotkey_.Empty()) {
        UnregisterHotKey(hwnd_, kPinHotkeyId);
        pinHotkey_ = {};
    }
    if (!hotkey.Valid()) return true;   // clearing the shortcut is not a failure

    // MOD_NOREPEAT: without it, holding the keys down toggles the pin over and over.
    constexpr UINT kNoRepeat = 0x4000;
    if (!RegisterHotKey(hwnd_, kPinHotkeyId, hotkey.mods | kNoRepeat, hotkey.key)) {
        PinDiag(L"快捷键注册失败: %s (错误 %lu)",
                FormatHotkeyWide(hotkey).c_str(), GetLastError());
        return false;
    }
    pinHotkey_ = hotkey;
    PinDiag(L"快捷键已注册: %s", FormatHotkeyWide(hotkey).c_str());
    return true;
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


WindowId WinControlWindow::WindowUnderCursor() const {
    POINT pt{};
    if (!GetCursorPos(&pt)) return 0;
    HWND hit = WindowFromPoint(pt);
    if (!hit) return 0;
    HWND root = GetAncestor(hit, GA_ROOT);
    if (!root || root == hwnd_ || IsOwnWindowClass(root)) return 0;
    return static_cast<WindowId>(reinterpret_cast<std::uintptr_t>(root));
}

void WinControlWindow::UpdateGrabTarget() {
    // The cursor is redrawn here rather than from WM_SETCURSOR: while a window holds the
    // mouse capture, the system stops asking it what cursor to use.
    if (grabCursor_) SetCursor(grabCursor_);

    const WindowId candidate = WindowUnderCursor();
    // Anything that cannot be pinned leaves the current target alone rather than clearing
    // it. Transient windows appear over the one being aimed at all the time - PDF-XChange
    // floats a toolbar on hover, tooltips arrive after a second of stillness, an auto-hide
    // taskbar slides up - and clearing on those made the click land on nothing. Keeping the
    // last valid target also makes the highlight honest: whatever is outlined is what gets
    // pinned.
    if (candidate == 0) return;
    if (handlers_.isPinnable && !handlers_.isPinnable(candidate)) return;
    if (candidate == grabTarget_) return;

    grabTarget_ = candidate;
    PinDiag(L"准星指向 %llu", static_cast<unsigned long long>(candidate));
    if (handlers_.onGrabPreview) handlers_.onGrabPreview(candidate);
}

void WinControlWindow::BeginGrabFromMenu() {
    if (grabState_ != GrabState::None) return;
    ShowGrabHandle();
}

void WinControlWindow::ShowGrabHandle() {
    if (grabHandle_) {
        SetForegroundWindow(grabHandle_);
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = GrabHandleProc;
    wc.lpszClassName = kGrabHandleClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    POINT cursor{};
    GetCursorPos(&cursor);

    UINT dpi = 96;
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (monitor) {
        UINT dx = 96;
        UINT dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) dpi = dx;
    }
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int width = scale(kHandleW);
    const int height = scale(kHandleH);

    // The crosshair lands directly under the cursor, so the gesture is press-and-drag with
    // no repositioning first - which is the whole point of having a handle.
    int x = cursor.x - scale(kHandlePad + kHandleCross / 2);
    int y = cursor.y - scale(kHandlePad + kHandleCross / 2);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        x = std::min(std::max(x, static_cast<int>(info.rcWork.left)),
                     static_cast<int>(info.rcWork.right) - width);
        y = std::min(std::max(y, static_cast<int>(info.rcWork.top)),
                     static_cast<int>(info.rcWork.bottom) - height);
    }

    grabHandle_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kGrabHandleClass, L"", WS_POPUP | WS_BORDER,
        x, y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!grabHandle_) return;
    ShowWindow(grabHandle_, SW_SHOWNOACTIVATE);
    // Activated after showing rather than through the style: if the foreground lock
    // refuses the activation the handle is still on screen and still draggable, because
    // the drag needs the mouse, not the keyboard.
    SetForegroundWindow(grabHandle_);
    // WS_EX_TOPMOST only puts it in the topmost band, not at the front of it. Without this
    // nudge another topmost window - a screenshot tool's overlay, a floating palette - can
    // sit on top of the handle, and the press meant for the crosshair lands there instead.
    // A background process is not allowed to raise a window, so this can be ignored; it
    // costs nothing when it is, and the menu path that leads here normally leaves this
    // process holding the foreground anyway.
    SetWindowPos(grabHandle_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Left alone, it should go away by itself rather than sit there forever.
    SetTimer(grabHandle_, kGrabTimeoutTimer, kGrabTimeoutMs, nullptr);
    PinDiag(L"抓取句柄已弹出 (%d,%d) %dx%d dpi=%u", x, y, width, height, dpi);
}

void WinControlWindow::DestroyGrabHandle() noexcept {
    HWND handle = grabHandle_;
    grabHandle_ = nullptr;   // cleared first: DestroyWindow re-enters through WM_DESTROY
    if (handle) {
        KillTimer(handle, kGrabTimeoutTimer);
        DestroyWindow(handle);
    }
}

void WinControlWindow::StartGrabFromHandle() {
    if (grabState_ != GrabState::None || !grabHandle_) return;
    grabCapture_ = grabHandle_;
    SetCapture(grabHandle_);
    grabState_ = GrabState::Grabbing;
    grabTarget_ = 0;
    KillTimer(grabHandle_, kGrabTimeoutTimer);
    SetTimer(hwnd_, kGrabTimeoutTimer, kGrabTimeoutMs, nullptr);
    if (!grabCursor_) grabCursor_ = LoadCursorW(nullptr, IDC_CROSS);
    PinDiag(L"抓取开始: 捕获成功=%d", GetCapture() == grabHandle_ ? 1 : 0);
    UpdateGrabTarget();
}

void WinControlWindow::PaintGrabHandle(HWND handle) const {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(handle, &ps);
    RECT client{};
    GetClientRect(handle, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

    const int dpi = static_cast<int>(GetDpiForWindow(handle));
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };

    // A ring with four ticks pushing past it - what every window-finder tool on Windows
    // looks like, so it needs no caption of its own to be understood.
    const int pad = scale(kHandlePad);
    const int size = scale(kHandleCross);
    const int cx = pad + size / 2;
    const int cy = client.bottom / 2;
    const int radius = size / 3;
    const int reach = size / 2;

    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(2)), GetSysColor(COLOR_WINDOWTEXT));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    MoveToEx(dc, cx - reach, cy, nullptr); LineTo(dc, cx - radius, cy);
    MoveToEx(dc, cx + radius, cy, nullptr); LineTo(dc, cx + reach, cy);
    MoveToEx(dc, cx, cy - reach, nullptr); LineTo(dc, cx, cy - radius);
    MoveToEx(dc, cx, cy + radius, nullptr); LineTo(dc, cx, cy + reach);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    HFONT font = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                   static_cast<UINT>(dpi))) {
        font = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT text{pad + size + pad, 0, client.right - pad, client.bottom};
    DrawTextW(dc, L"按住准星拖到\n目标窗口上松开", -1, &text,
              DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_WORDBREAK);
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);

    EndPaint(handle, &ps);
}

LRESULT CALLBACK WinControlWindow::GrabHandleProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                  LPARAM lParam) {
    auto* self = reinterpret_cast<WinControlWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KILLFOCUS:
    case WM_SETFOCUS:
    case WM_DESTROY:
    case WM_CAPTURECHANGED:
    case WM_MOUSEACTIVATE:
        PinDiag(L"句柄消息 0x%04X wParam=%llu 状态=%d", msg,
                static_cast<unsigned long long>(wParam),
                self->grabState_ == GrabState::None ? 0 : 1);
        break;
    default:
        break;
    }

    switch (msg) {
    case WM_NCHITTEST:
        // While dragging, the handle has to be invisible to WindowFromPoint or it would be
        // the only thing the crosshair ever finds. Capture is unaffected: captured mouse
        // messages bypass hit-testing entirely.
        if (self->grabState_ != GrabState::None) return HTTRANSPARENT;
        return HTCLIENT;
    case WM_PAINT:
        self->PaintGrabHandle(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        self->StartGrabFromHandle();
        return 0;
    case WM_RBUTTONDOWN:
        // Before the drag starts there is no grab for HandleMessage to cancel, so the
        // right-click has to close the handle here.
        if (self->grabState_ == GrabState::None) {
            self->DestroyGrabHandle();
            return 0;
        }
        return self->HandleMessage(msg, wParam, lParam);
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        // One state machine, one implementation. Forwarding rather than duplicating is
        // what keeps the handle's behaviour and the control window's from drifting apart.
        return self->HandleMessage(msg, wParam, lParam);
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (self->grabState_ != GrabState::None) self->EndGrab(false);
            else self->DestroyGrabHandle();
            return 0;
        }
        break;
    case WM_TIMER:
        // Only armed while the handle is waiting to be picked up; once the drag starts the
        // timeout moves to the control window.
        if (wParam == kGrabTimeoutTimer && self->grabState_ == GrabState::None) {
            self->DestroyGrabHandle();
            return 0;
        }
        break;
    // No WM_KILLFOCUS handling. Destroying the handle on focus loss was tried and it ate
    // the handle before the user could press on it: if some other topmost window is
    // covering the crosshair, the press lands there, that window takes the focus, and the
    // handle deletes itself - so the gesture failed with nothing on screen to explain why.
    // Losing focus is not a decision; Esc, a right-click, and the timeout are.
    case WM_DESTROY:
        if (self->grabHandle_ == hwnd) self->grabHandle_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WinControlWindow::EndGrab(bool commit) {
    if (grabState_ == GrabState::None) return;
    const WindowId target = grabTarget_;
    grabState_ = GrabState::None;
    KillTimer(hwnd_, kGrabTimeoutTimer);
    grabTarget_ = 0;
    if (grabCapture_ && GetCapture() == grabCapture_) ReleaseCapture();
    grabCapture_ = nullptr;
    DestroyGrabHandle();
    // Clear the preview before committing, so the window ends up drawn from the real
    // pinned set rather than briefly from both.
    if (handlers_.onGrabPreview) handlers_.onGrabPreview(0);
    PinDiag(L"EndGrab: commit=%d target=%llu", commit ? 1 : 0, (unsigned long long)target);
    if (commit && target != 0) {
        if (handlers_.onGrabCommit) handlers_.onGrabCommit(target);
    } else if (handlers_.onGrabCancel) {
        handlers_.onGrabCancel();
    }
}

void WinControlWindow::ShowMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // 这里曾经有一行灰色的「WindowMark」标题，用来让「关于」不必写成「关于 WindowMark...」。
    // 那一步确实减了宽，但标题本身随后成了新的瓶颈：实测它撑到 212px，而去掉之后是 187px
    // ——十个拉丁字符比任何一个中文标签都宽。菜单是从 WindowMark 的托盘图标上右击出来的，
    // 图标和 tooltip 已经说明了身份，标题行没有再留的理由。
    //
    // 顺带记一笔，同一次测量还否掉了一个直觉：给带子菜单的项加 MF_CHECKED 完全不影响
    // 菜单宽度（带勾不带勾都是 212px）。宽度只由最宽的那个标签决定。
    //
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
        AppendMenuW(menu, MF_POPUP | (enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    reinterpret_cast<UINT_PTR>(bookmarks), L"书签");
    }

    HMENU borders = CreatePopupMenu();
    if (borders) {
        AppendMenuW(borders, MF_STRING | (bordersEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kToggleBordersCommand, L"启用窗口边框");
        AppendMenuW(borders, MF_STRING, kBorderExcludeCommand, L"排除应用...");
        AppendMenuW(borders, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(borders, MF_STRING, kBorderSettingsCommand, L"边框设置...");
        AppendMenuW(menu, MF_POPUP | (bordersEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    reinterpret_cast<UINT_PTR>(borders), L"窗口边框");
    }

    HMENU pinning = CreatePopupMenu();
    if (pinning) {
        AppendMenuW(pinning, MF_STRING | (pinningEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kTogglePinningCommand, L"启用窗口置顶");
        if (pinningEnabled_) {
            // The glyph is the affordance: it says "this one hands you a crosshair",
            // the way 「❏置于顶层」 marks the system-menu item as ours.
            AppendMenuW(pinning, MF_STRING, kGrabToPinCommand, L"\u2295抓取窗口置顶...");
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
        AppendMenuW(menu, MF_POPUP | (pinningEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    reinterpret_cast<UINT_PTR>(pinning), L"窗口置顶");
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
    // 同样是程序级而非功能级的设置。带省略号是「点了会开对话框」的标准约定，代价是它
    // 比「开机启动」宽了三个点——顶层菜单的宽度由最宽的标签决定，这里是有意付的。
    AppendMenuW(menu, MF_STRING, kConfigPathCommand, L"配置文件...");
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
    case WM_HOTKEY:
        // Deliberately not gated on pinningEnabled_. If the user pressed the shortcut they
        // want the window pinned; silently doing nothing because a submenu switch is off
        // is the failure mode this whole feature exists to avoid.
        PinDiag(L"收到 WM_HOTKEY id=%d 有处理器=%d", static_cast<int>(wParam),
                handlers_.onPinHotkey ? 1 : 0);
        if (static_cast<int>(wParam) == kPinHotkeyId && handlers_.onPinHotkey) {
            handlers_.onPinHotkey();
        }
        return 0;
    case WM_TIMER:
        if (wParam == kGrabTimeoutTimer) {
            EndGrab(false);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (grabState_ == GrabState::Grabbing) {
            UpdateGrabTarget();
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        // The button went down on the handle, so the release is the confirmation - there
        // is no stray button-up from a dismissed menu to guard against any more.
        if (grabState_ == GrabState::Grabbing) {
            EndGrab(true);
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
    case WM_KILLFOCUS:
        if (grabState_ != GrabState::None) {
            EndGrab(false);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        // Something else took the mouse. Whatever it was, this grab is over.
        if (grabState_ != GrabState::None) EndGrab(false);
        return 0;
    case kTrayMessage:
        // No left-button gesture here on purpose. Dragging off the tray icon was tried and
        // dropped: the icon is usually folded into the overflow flyout, and claiming the
        // left button meant an ordinary click on it had to be told apart from the start of
        // a drag. The crosshair handle in the menu does the same job without either
        // problem.
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
            const bool havePath = !turningOn ||
                GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe))) != 0;
            if (!havePath || !app::SetAutoStart(exe, turningOn)) {
                MessageBoxW(hwnd_,
                            turningOn
                                ? L"写入 Windows 开机启动设置失败。请检查当前用户的注册表权限。"
                                : L"删除 Windows 开机启动设置失败。请检查当前用户的注册表权限。",
                            L"WindowMark", MB_OK | MB_ICONERROR);
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
        case kBorderExcludeCommand:  handler = &handlers_.onBorderExcludeApps; break;
        case kTogglePinningCommand:  handler = &handlers_.onTogglePinning; break;

        case kUnpinAllCommand:       handler = &handlers_.onUnpinAll; break;
        case kPinSettingsCommand:    handler = &handlers_.onPinSettings; break;
        case kGrabToPinCommand:
            // Entered after the menu closes, so the menu's own mouse messages are gone.
            BeginGrabFromMenu();
            return 0;
        case kConfigPathCommand:     handler = &handlers_.onConfigPath; break;
        case kAboutCommand:          handler = &handlers_.onAbout; break;
        case kExitCommand:           handler = &handlers_.onExit; break;
        default: break;
        }
        if (handler) {
            // An unset handler used to be silent, and 「全部取消置顶」 spent its whole life
            // that way: the menu item was there, the click dispatched, and nothing at all
            // happened - no error, no log, nothing to notice.
            if (*handler) (*handler)();
            else PinDiag(L"菜单命令 %u 没有接上处理器", LOWORD(wParam));
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
