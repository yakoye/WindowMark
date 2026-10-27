#include "WinControlWindow.h"

#include "AppIdentity.h"
#include "AutoStart.h"
#include "ClipKeeperIdentity.h"
#include "PinDiag.h"
#include "Resource.h"
#include "WinUtil.h"

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
        {L"onToggleDrag", static_cast<bool>(handlers_.onToggleDrag)},
        {L"onDragSettings", static_cast<bool>(handlers_.onDragSettings)},
        {L"onDragExcludeApps", static_cast<bool>(handlers_.onDragExcludeApps)},
        {L"onToggleBookmarks", static_cast<bool>(handlers_.onToggleBookmarks)},
        {L"onSelection", static_cast<bool>(handlers_.onSelection)},
        {L"onBookmarkSettings", static_cast<bool>(handlers_.onBookmarkSettings)},
        {L"onToggleBorders", static_cast<bool>(handlers_.onToggleBorders)},
        {L"onBorderSettings", static_cast<bool>(handlers_.onBorderSettings)},
        {L"onBorderExcludeApps", static_cast<bool>(handlers_.onBorderExcludeApps)},
        {L"onTogglePinning", static_cast<bool>(handlers_.onTogglePinning)},
        {L"onTogglePinWindow", static_cast<bool>(handlers_.onTogglePinWindow)},
        {L"onGrabPreview", static_cast<bool>(handlers_.onGrabPreview)},
        {L"onGrabJoinGroup", static_cast<bool>(handlers_.onGrabJoinGroup)},
        {L"onGrabCommit", static_cast<bool>(handlers_.onGrabCommit)},
        {L"onGrabCancel", static_cast<bool>(handlers_.onGrabCancel)},
        {L"canGrab", static_cast<bool>(handlers_.canGrab)},
        {L"manualGroups", static_cast<bool>(handlers_.manualGroups)},
        {L"manualGroupOf", static_cast<bool>(handlers_.manualGroupOf)},
        {L"onDisbandAllGroups", static_cast<bool>(handlers_.onDisbandAllGroups)},
        {L"onUnpinAll", static_cast<bool>(handlers_.onUnpinAll)},
        {L"onPinSettings", static_cast<bool>(handlers_.onPinSettings)},
        {L"onPinHotkey", static_cast<bool>(handlers_.onPinHotkey)},
        {L"onClipKeeper", static_cast<bool>(handlers_.onClipKeeper)},
        {L"onConfigPath", static_cast<bool>(handlers_.onConfigPath)},
        {L"onDesktopShortcut", static_cast<bool>(handlers_.onDesktopShortcut)},
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
    // explorer 一重启，系统就把所有托盘图标销毁重来，然后广播这条消息让大家重新加。
    // 不接它，图标就再也回不来了——而程序还好好跑着，用户只会看到「托盘里没了」，
    // 再双击 exe 又被单例挡下，什么都不发生，看起来就是「启动不了」。
    //
    // explorer 重启比想象中常见：改显示配置、远程桌面/ToDesk 接入、它自己崩一次都会。
    // 实测一次就是这么来的：WindowMark 15:15 启动，explorer 18:46 重启，图标随之消失。
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

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
    SetSwitchHotkeys(0);
    RemoveTrayIcon();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

std::wstring WinControlWindow::SetSwitchHotkeys(UINT mods) {
    if (!hwnd_) return {};
    if (mods == switchMods_) return {};

    if (switchMods_ != 0) {
        for (int i = 1; i <= 9; ++i) UnregisterHotKey(hwnd_, kSwitchHotkeyBase + i);
        UnregisterHotKey(hwnd_, kSwitchPreviousId);
        switchMods_ = 0;
    }
    if (mods == 0) return {};

    // MOD_NOREPEAT：按住不放不该连续切换。
    constexpr UINT kNoRepeat = 0x4000;
    std::wstring failed;
    for (int i = 1; i <= 9; ++i) {
        const UINT vk = static_cast<UINT>('0' + i);
        if (!RegisterHotKey(hwnd_, kSwitchHotkeyBase + i, mods | kNoRepeat, vk)) {
            if (!failed.empty()) failed += L"、";
            failed += static_cast<wchar_t>(L'0' + i);
        }
    }
    if (!RegisterHotKey(hwnd_, kSwitchPreviousId, mods | kNoRepeat, VK_OEM_3)) {
        if (!failed.empty()) failed += L"、";
        failed += L"`";
    }
    switchMods_ = mods;
    PinDiag(L"切换快捷键已注册 mods=0x%X，没注册上的: %s", mods,
            failed.empty() ? L"(无)" : failed.c_str());
    return failed;
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

void WinControlWindow::SetDragState(bool enabled) { dragEnabled_ = enabled; }

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
    wcscpy_s(data.szTip, L"WindowMark - 左键打开面板，右键打开菜单");
    Shell_NotifyIconW(NIM_ADD, &data);
}

void WinControlWindow::ShowBalloon(const wchar_t* title, const std::wstring& text) {
    if (!hwnd_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(data.szInfoTitle, title, std::size(data.szInfoTitle) - 1);
    wcsncpy_s(data.szInfo, text.c_str(), std::size(data.szInfo) - 1);
    // NIM_MODIFY 往一个不存在的图标上发会失败，而且是静默失败。图标没了的时候用户
    // 恰恰最可能双击 exe——那正是这条路唯一被走到的时候，不能在这里哑掉。
    if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
        AddTrayIcon();
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}

void WinControlWindow::ShowAlreadyRunningHint() {
    ShowBalloon(L"WindowMark 已在运行",
                L"窗口书签已经启用。右键此图标可以隐藏窗口书签或选择参与的应用。");
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
    if (handlers_.canGrab && !handlers_.canGrab(candidate)) return;
    if (candidate == grabTarget_) return;

    grabTarget_ = candidate;
    PinDiag(L"准星指向 %llu", static_cast<unsigned long long>(candidate));
    if (handlers_.onGrabPreview) handlers_.onGrabPreview(candidate);
}

void WinControlWindow::BeginGrabFromMenu(GrabPurpose purpose, int groupId) {
    if (grabState_ != GrabState::None) return;
    grabPurpose_ = purpose;
    grabGroupId_ = groupId;
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
    // 说明里带上这次抓来干什么：准星长得一样，松手之后的结果不一样，光一个准星说不清。
    const wchar_t* hint = L"按住准星拖到\n目标窗口上松开";
    std::wstring joinHint;
    if (grabPurpose_ == GrabPurpose::JoinGroup) {
        joinHint = grabGroupId_ == 0 ? L"拖到窗口上松开\n→ 新建分组"
                                     : L"拖到窗口上松开\n→ 分组 " +
                                           std::to_wstring(grabGroupId_);
        hint = joinHint.c_str();
    }
    DrawTextW(dc, hint, -1, &text,
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
        if (grabPurpose_ == GrabPurpose::JoinGroup) {
            CommitGrabToGroup(target);
        } else if (handlers_.onGrabCommit) {
            handlers_.onGrabCommit(target);
        }
    } else if (handlers_.onGrabCancel) {
        handlers_.onGrabCancel();
    }
}

void WinControlWindow::CommitGrabToGroup(WindowId target) {
    if (!handlers_.onGrabJoinGroup) return;

    wchar_t title[96]{};
    GetWindowTextW(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(target)), title, 96);
    std::wstring name = title[0] ? title : L"这个窗口";
    if (name.size() > 28) name = name.substr(0, 27) + L"…";

    // 它本来就在目标组里：加也是加，但屏幕上什么都不会变，得单独说一句。
    if (grabGroupId_ != 0 && handlers_.manualGroupOf &&
        handlers_.manualGroupOf(target) == grabGroupId_) {
        ShowBalloon(L"它已经在这个组里了",
                    L"「" + name + L"」本来就在「分组 " + std::to_wstring(grabGroupId_) +
                        L"」里，什么都没变。");
        return;
    }

    const int before = grabGroupId_;
    const int group = handlers_.onGrabJoinGroup(target, grabGroupId_);
    if (group == 0) {
        // 只会是「这个窗口没被跟踪」。canGrab 已经挡过一道，还是走到这儿就说出来，
        // 别让用户对着一个什么都没发生的准星猜。
        ShowBalloon(L"加入分组没成功", L"「" + name + L"」现在不在 WindowMark 的跟踪范围里。");
        return;
    }

    // 组名和人数现查：加入之后才知道这个组变成几个人，而「新建分组」连 id 都是刚分配的。
    std::wstring label = L"分组 " + std::to_wstring(group);
    std::size_t count = 0;
    if (handlers_.manualGroups) {
        for (const ManualGroupInfo& info : handlers_.manualGroups()) {
            if (info.id != group) continue;
            label = Utf8ToWide(info.name);
            count = info.count;
            break;
        }
    }

    if (count >= 2) {
        ShowBalloon(L"已加入分组",
                    L"「" + name + L"」进了「" + label + L"」，现在 " +
                        std::to_wstring(count) + L" 个窗口。");
    } else {
        // 一个成员的组按设计不生效（不然新建分组的那一刻书签条会先闪一下），所以这时候
        // 屏幕上什么都不会变——不说一声，用户只会以为点了没用。
        ShowBalloon(before == 0 ? L"分组建好了" : L"已加入分组",
                    L"「" + name + L"」进了「" + label +
                        L"」。再抓一个窗口进来，这条书签条就会出现。");
    }
}

void WinControlWindow::ShowMenu() {
    // 对勾以当前配置为准，不信缓存。见 Handlers::onMenuOpening。
    if (handlers_.onMenuOpening) handlers_.onMenuOpening();
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
                    kToggleCommand, L"启用窗口书签");
        // Shortened from 「选择参与的应用/窗口...」: at twelve glyphs it was the widest item
        // anywhere in the menu, and a submenu is right-aligned to the parent's left edge,
        // so it alone decided how far left the 书签 panel reached - visibly further than
        // the 窗口边框 panel next to it.
        AppendMenuW(bookmarks, MF_STRING, kSelectionCommand, L"选择应用/窗口...");

        // 手动分组的入口。书签上右键也能加入分组，但那要求那个窗口**已经有**书签条
        // ——也就是它那个应用至少开着两个窗口。落单的窗口只能从这儿进来。
        groupMenuIds_.clear();
        if (enabled_) {
            std::vector<ManualGroupInfo> groups;
            if (handlers_.manualGroups) groups = handlers_.manualGroups();
            HMENU grabGroup = CreatePopupMenu();
            if (grabGroup) {
                AppendMenuW(grabGroup, MF_STRING, kGrabToGroupCommand, L"新建分组");
                if (!groups.empty()) AppendMenuW(grabGroup, MF_SEPARATOR, 0, nullptr);
                for (const ManualGroupInfo& group : groups) {
                    if (groupMenuIds_.size() >= kGroupCommandLimit) break;
                    const std::wstring label = Utf8ToWide(group.name) + L"（" +
                                               std::to_wstring(group.count) + L" 个窗口）";
                    AppendMenuW(grabGroup, MF_STRING,
                                kGroupCommandBase +
                                    static_cast<UINT>(groupMenuIds_.size()),
                                label.c_str());
                    groupMenuIds_.push_back(group.id);
                }
                // 和「抓取窗口置顶」同一个字形：它说的是「这一条会给你一个准星」。
                AppendMenuW(bookmarks, MF_POPUP, reinterpret_cast<UINT_PTR>(grabGroup),
                            L"\u2295抓取窗口加入分组");
            }
            if (!groups.empty()) {
                AppendMenuW(bookmarks, MF_STRING, kDisbandAllGroupsCommand, L"解散所有分组");
            }
        }

        AppendMenuW(bookmarks, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(bookmarks, MF_STRING, kSettingsCommand, L"书签设置...");
        AppendMenuW(menu, MF_POPUP | (enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    reinterpret_cast<UINT_PTR>(bookmarks), L"窗口书签");
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

    HMENU dragging = CreatePopupMenu();
    if (dragging) {
        AppendMenuW(dragging, MF_STRING | (dragEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kToggleDragCommand, L"启用窗口拖动");
        AppendMenuW(dragging, MF_STRING, kDragExcludeCommand, L"排除应用...");
        AppendMenuW(dragging, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(dragging, MF_STRING, kDragSettingsCommand, L"拖动设置...");
        AppendMenuW(menu, MF_POPUP | (dragEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    reinterpret_cast<UINT_PTR>(dragging), L"窗口拖动");
    }

    {
        HMENU ime = CreatePopupMenu();
        AppendMenuW(ime, MF_STRING | MF_DISABLED, 0, imeStatus_.c_str());
        AppendMenuW(ime, MF_STRING | (imeActive_ ? MF_CHECKED : MF_UNCHECKED),
                    kToggleImeGuardCommand, imeRequested_ ? L"关闭输入法占位保护" : L"启用输入法占位保护");
        AppendMenuW(ime, MF_STRING, kRestoreImeGuardCommand, L"恢复系统默认布局");
        AppendMenuW(ime, MF_STRING | (imeHints_ ? MF_CHECKED : MF_UNCHECKED), kToggleImeHintsCommand, L"空位显示轻松提示语");
        AppendMenuW(ime, MF_STRING, kCheckImeGuardCommand, L"检查兼容性（只读）");
        AppendMenuW(ime, MF_STRING, kImeGuardDiagnosticsCommand, L"打开诊断日志目录");
        AppendMenuW(menu, MF_POPUP | (imeActive_ ? MF_CHECKED : MF_UNCHECKED),
                    reinterpret_cast<UINT_PTR>(ime), L"任务栏输入法占位保护");
    }
    // 剪贴板守护。它是独立进程（ClipKeeper.exe），这一项开关的是它的**面板**：
    // 对勾 = 面板当前可见，与点击行为一一对应。停止守护在 ClipKeeper 自己的面板里做。
    //
    // 它也是顶层唯一能真正点击切换的功能项——上面四个带子菜单，MF_POPUP 项没有命令 ID，
    // 点击即展开子菜单，没法兼作开关。
    //
    // 对勾的含义是「它在不在守护」，不是「面板开着没开」——收起面板正是它的正常用法
    // （继续在托盘里守护），按面板可见来打勾，一收面板勾就没了，看上去像关掉了。
    //
    // 每次开菜单实时查，不缓存：用户随时可能从 ClipKeeper 自己的托盘里暂停或退出它。
    {
        namespace ck = windowmark::clipkeeper;
        const HWND panel = FindWindowW(ck::kWindowClass, nullptr);
        bool guarding = panel != nullptr;
        if (guarding) {
            // 超时发：菜单正弹着，不能被一个卡住的进程拖住。问不出来就按「在跑就算在守护」
            // 算——守护本来就是默认开着的。
            static const UINT query = RegisterWindowMessageW(ck::kQueryStateMessage);
            DWORD_PTR state = 0;
            if (query != 0 &&
                SendMessageTimeoutW(panel, query, 0, 0, SMTO_ABORTIFHUNG, 200, &state) != 0) {
                guarding = (state & ck::kStateGuarding) != 0;
            }
        }
        AppendMenuW(menu, MF_STRING | (guarding ? MF_CHECKED : MF_UNCHECKED),
                    kClipKeeperCommand, L"剪贴板守护");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Master switch above the per-feature ones: one click silences the whole app without
    // having to visit every submenu. The label names what the click will do rather than
    // what the state is - no tick to interpret, and two glyphs instead of five.
    //
    // 四个功能必须和 WinMain 里 onToggleAll 真正切换的是同一组。以前这里看「书签/边框/置顶」、
    // 那边切「书签/边框/拖动」：只开着置顶时这里显示「暂停所有」，点下去反而把另外三个全打开。
    const bool anythingOn = enabled_ || bordersEnabled_ || pinningEnabled_ || dragEnabled_;
    // 放在最上面：第一次见这个程序的人，菜单里能看懂的只有这一项。
    AppendMenuW(menu, MF_STRING, kPanelCommand, L"面板...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kToggleAllCommand, anythingOn ? L"暂停窗口工具" : L"启用窗口工具");
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
    AppendMenuW(menu, MF_STRING, kDesktopShortcutCommand, L"创建桌面快捷方式");
    AppendMenuW(menu, MF_STRING, kConfigPathCommand, L"配置文件...");
    // 安装 / 卸载是同一个 exe 的两个模式，所以入口在这里而不是另一个 exe 里。同时只出现
    // 一个：解压目录里跑着的那份能「安装到系统」，安装目录里跑着的那份能「卸载」。
    if (runningInstalled_) {
        AppendMenuW(menu, MF_STRING, kUninstallCommand, L"卸载 WindowMark...");
    } else {
        AppendMenuW(menu, MF_STRING, kInstallCommand, L"安装到系统...");
    }
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
    static const UINT showPanel = RegisterWindowMessageW(L"WindowMark.ShowMainPanel");
    if (showPanel && msg == showPanel) {
        if (handlers_.onShowPanel) handlers_.onShowPanel();
        return 0;
    }
    // Registered message ids are runtime values, so they cannot be switch labels.
    if (requestQuitMessage_ != 0 && msg == requestQuitMessage_) {
        if (handlers_.onExit) handlers_.onExit();
        return 0;
    }
    if (secondInstanceMessage_ != 0 && msg == secondInstanceMessage_) {
        ShowAlreadyRunningHint();
        return 0;
    }
    if (taskbarCreatedMessage_ != 0 && msg == taskbarCreatedMessage_) {
        // 图标已经被 explorer 销毁了，直接重新加，不用先 NIM_DELETE。
        PinDiag(L"收到 TaskbarCreated，重新加托盘图标");
        AddTrayIcon();
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
            return 0;
        }
        if (static_cast<int>(wParam) == kSwitchPreviousId && handlers_.onSwitchPrevious) {
            handlers_.onSwitchPrevious();
            return 0;
        }
        if (const int index = static_cast<int>(wParam) - kSwitchHotkeyBase;
            index >= 1 && index <= 9 && handlers_.onSwitchTo) {
            handlers_.onSwitchTo(index);
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
        // 左键**单击**有用，左键**拖动**没有。拖动试过又去掉了：图标常被折进溢出面板，
        // 在那里起手拖动就得把「普通的一次点击」和「手势开始」区分开，而菜单里的准星
        // 把同一件事做得更稳。单击不涉及这个问题——按下抬起就是一次点击。
        if (lParam == WM_LBUTTONUP) {
            if (handlers_.onShowPanel) handlers_.onShowPanel();
            return 0;
        }
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

        // 同上：一条命令一个现有分组，抓来的窗口进那个组。
        {
            const UINT command = LOWORD(wParam);
            if (command >= kGroupCommandBase &&
                command < kGroupCommandBase + kGroupCommandLimit) {
                const std::size_t index = command - kGroupCommandBase;
                if (index < groupMenuIds_.size()) {
                    BeginGrabFromMenu(GrabPurpose::JoinGroup, groupMenuIds_[index]);
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
        case kToggleDragCommand:     handler = &handlers_.onToggleDrag; break;
        case kDragSettingsCommand:   handler = &handlers_.onDragSettings; break;
        case kDragExcludeCommand:    handler = &handlers_.onDragExcludeApps; break;

        case kToggleImeGuardCommand: handler = &handlers_.onToggleImeGuard; break;
        case kRestoreImeGuardCommand: handler = &handlers_.onRestoreImeGuard; break;
        case kCheckImeGuardCommand: handler = &handlers_.onCheckImeGuard; break;
        case kImeGuardDiagnosticsCommand: handler = &handlers_.onImeGuardDiagnostics; break;
        case kToggleImeHintsCommand: handler = &handlers_.onToggleImeHints; break;
        case kUnpinAllCommand:       handler = &handlers_.onUnpinAll; break;
        case kPinSettingsCommand:    handler = &handlers_.onPinSettings; break;
        case kGrabToPinCommand:
            // Entered after the menu closes, so the menu's own mouse messages are gone.
            BeginGrabFromMenu(GrabPurpose::Pin, 0);
            return 0;
        case kGrabToGroupCommand:
            BeginGrabFromMenu(GrabPurpose::JoinGroup, 0);
            return 0;
        case kDisbandAllGroupsCommand: handler = &handlers_.onDisbandAllGroups; break;
        case kClipKeeperCommand:     handler = &handlers_.onClipKeeper; break;
        case kConfigPathCommand:     handler = &handlers_.onConfigPath; break;
        case kDesktopShortcutCommand: handler = &handlers_.onDesktopShortcut; break;
        case kInstallCommand:        handler = &handlers_.onInstall; break;
        case kPanelCommand:          handler = &handlers_.onShowPanel; break;
        case kPinLastWindowCommand:  handler = &handlers_.onPinLastWindow; break;
        case kDiagnoseCommand:       handler = &handlers_.onDiagnose; break;
        case kUninstallCommand:      handler = &handlers_.onUninstall; break;
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
