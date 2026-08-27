#include "WinUtil.h"

#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

namespace windowmark::win {

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

std::wstring QueryProcessPath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};

    // Virtually every path fits in MAX_PATH; only fall back to a heap buffer for the
    // rare long one, instead of allocating 64 KB on every call.
    std::wstring result;
    wchar_t inlineBuffer[MAX_PATH];
    DWORD size = static_cast<DWORD>(std::size(inlineBuffer));

    if (QueryFullProcessImageNameW(process, 0, inlineBuffer, &size) && size > 0) {
        result.assign(inlineBuffer, size);
    } else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::wstring buffer(32768, L'\0');
        size = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) && size > 0) {
            buffer.resize(size);
            result = std::move(buffer);
        }
    }

    CloseHandle(process);
    return result;
}

std::string FileStemUtf8(const std::wstring& path) {
    if (path.empty()) return {};
    return WideToUtf8(std::filesystem::path(path).stem().wstring());
}

Rect ToCoreRect(const RECT& rect) {
    return Rect{rect.left, rect.top, rect.right, rect.bottom};
}

RECT ToWinRect(const Rect& rect) {
    return RECT{rect.left, rect.top, rect.right, rect.bottom};
}

namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)) || !value) {
        return {};
    }
    std::filesystem::path result(value);
    CoTaskMemFree(value);
    return result;
}

} // namespace

std::filesystem::path InstalledExePath() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return std::filesystem::path(buffer);
}

std::filesystem::path LocalDataRoot() {
    auto base = KnownFolder(FOLDERID_LocalAppData);
    return base.empty() ? std::filesystem::path{} : base / L"WindowMark";
}

std::filesystem::path RoamingDataRoot() {
    auto base = KnownFolder(FOLDERID_RoamingAppData);
    return base.empty() ? std::filesystem::path{} : base / L"WindowMark";
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

// System UI that is not a window in the sense the user means. Everything here was seen
// getting an outline in practice, or is a default tacky-borders ships for the same reason.
//
// The IME entry is the one that matters most. "Windows 输入体验" is a full-screen
// Windows.UI.Core.CoreWindow owned by TextInputHost that DWM keeps cloaked, so it passes
// the cloaked check while idle - but typing uncloaks it and the switcher flyout uncloaks
// it too, which put a screen-sized outline on screen for as long as the candidate list was
// up. Cloak, uncloak, cloak: that is the flicker.
//
// Making this list configurable is ROADMAP item 1 (window_rules).
constexpr std::wstring_view kExcludedClasses[] = {
    L"Progman",                             // desktop
    L"WorkerW",                             // desktop wallpaper host
    L"Shell_TrayWnd",                       // taskbar
    L"Shell_SecondaryTrayWnd",              // taskbar on secondary monitors
    L"Windows.UI.Core.CoreWindow",          // 「Windows 输入体验」, full-screen IME host
    L"XamlExplorerHostIslandWindow",        // Explorer's XAML islands
    L"TopLevelWindowForOverflowXamlIsland", // tray overflow flyout
    L"Shell_InputSwitchTopLevelWindow",     // 「Input Flyout」, the IME switcher, 480x410
    L"MS_WebcheckMonitor",                  // Explorer helper; 1920x1010 but never painted
    L"CicLoaderWndClass",                   // ctfmon helper
};

// ApplicationFrameWindow is the host frame for UWP apps, and a real one - 设置, 照片,
// 计算器 - is a window the user means, so the class cannot simply be excluded. The shell
// reuses it for its own chrome though: the IME candidate bar is an ApplicationFrameWindow
// too, measured at 751x90 with an empty title, and that one must not get an outline.
//
// What tells them apart is ownership. An app's frame belongs to ApplicationFrameHost.exe;
// the shell's belongs to explorer.exe. GetShellWindow() is the desktop window, so whoever
// owns it is the shell - looked up per call rather than cached, because explorer can
// restart and come back with a different pid.
bool IsShellOwned(HWND hwnd) {
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == 0) return false;

    DWORD shellPid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    return shellPid != 0 && windowPid == shellPid;
}

bool IsEligibleTopLevelWindow(HWND hwnd, const std::vector<std::wstring>& alsoExclude) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    if (IsCloaked(hwnd)) return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) return false;

    // Two bits that say "this is not a window anyone switches to", which is the only kind
    // this app is about. Both were arrived at by catching an actual offender rather than
    // by guessing: ChatGPT's hover chip, 61x32 at the cursor, empty title,
    // style 0x96000000 (WS_POPUP only - no caption, no frame),
    // ex 0x08200028 (NOACTIVATE | NOREDIRECTIONBITMAP | TRANSPARENT | TOPMOST).
    //
    //   WS_EX_NOACTIVATE  - the window refuses activation. Clicking a bookmark activates a
    //                       window; something that cannot be activated cannot be a target.
    //   WS_EX_TRANSPARENT - hit-test transparent, so the mouse passes straight through.
    //                       A window the user cannot even click is not one they mean.
    //
    // Note what is *not* used here. Excluding by class name would have taken out Chrome,
    // Claude and ChatGPT along with it - all three are Chrome_WidgetWin_1, the same class
    // as the chip. Excluding by size would have taken out every minimized window, whose
    // DWM extended bounds measure 183x34. Neither is a property of "is this a real
    // window"; these two flags are.
    if ((exStyle & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) != 0) return false;

    // Chromeless composition overlays: the launcher panel an app drops out of its tray
    // icon, and its like. Measured on Claude's quick-launch box, whose window rect is
    // 767x595 while only a strip at the top is opaque - it collected an outline several
    // times its visible size, and a bookmark tab besides.
    //
    // All four conditions together, because no one of them is enough:
    //   no WS_CAPTION and no WS_THICKFRAME - frameless, but so are plenty of real windows
    //   no WS_SYSMENU               - YeImageViewer and Picasa are frameless yet keep one
    //   WS_EX_NOREDIRECTIONBITMAP   - renders only through DirectComposition
    //
    // WS_EX_TOPMOST separates it just as well and was deliberately not used: pinning a
    // window *sets* that bit, so a frameless window like Czkawka would drop out of
    // tracking the moment the user pinned it, taking its own pin with it. Nothing ever
    // sets WS_EX_NOREDIRECTIONBITMAP on another app's window.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const bool frameless = (style & WS_CAPTION) != WS_CAPTION &&
                           (style & WS_THICKFRAME) == 0 &&
                           (style & WS_SYSMENU) == 0;
    if (frameless && (exStyle & WS_EX_NOREDIRECTIONBITMAP) != 0) return false;

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const std::wstring_view cls(className);
    for (const auto excluded : kExcludedClasses) {
        if (cls == excluded) return false;
    }
    for (const auto& excluded : alsoExclude) {
        if (cls == excluded) return false;
    }
    if (cls == L"ApplicationFrameWindow" && IsShellOwned(hwnd)) return false;
    return true;
}

Rect ExtendedFrame(HWND hwnd, bool& fromDwm) {
    RECT rect{};
    fromDwm = SUCCEEDED(
        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)));
    if (!fromDwm) GetWindowRect(hwnd, &rect);
    return ToCoreRect(rect);
}

Rect ExtendedFrame(HWND hwnd) {
    bool ignored = false;
    return ExtendedFrame(hwnd, ignored);
}

Rect WorkAreaFor(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        RECT desktop{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
        return ToCoreRect(desktop);
    }
    return ToCoreRect(info.rcWork);
}

void RemoveStartupRegistration() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0,
                      KEY_SET_VALUE,
                      &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"WindowMark");
        RegCloseKey(key);
    }
}

unsigned SystemAccentColor() {
    // DWM stores it as ABGR, which is why the red and blue bytes come out swapped from
    // the order the name suggests.
    DWORD abgr = 0;
    DWORD size = sizeof(abgr);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"AccentColor", RRF_RT_REG_DWORD, &type, &abgr, &size) != ERROR_SUCCESS) {
        return 0xFF0078D4u;  // Windows' own default blue, for a machine with no override.
    }
    const unsigned r = abgr & 0xFFu;
    const unsigned g = (abgr >> 8) & 0xFFu;
    const unsigned b = (abgr >> 16) & 0xFFu;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void PurgeAllUserData() {
    RemoveStartupRegistration();
    std::error_code ec;
    const auto local = LocalDataRoot();
    if (!local.empty()) std::filesystem::remove_all(local, ec);
    ec.clear();
    const auto roaming = RoamingDataRoot();
    if (!roaming.empty()) std::filesystem::remove_all(roaming, ec);
}

} // namespace windowmark::win
