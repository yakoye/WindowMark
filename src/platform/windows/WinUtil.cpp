#include "WinUtil.h"

#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
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

bool IsEligibleTopLevelWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    if (IsCloaked(hwnd)) return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) return false;

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const std::wstring cls(className);
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd") {
        return false;
    }
    return true;
}

Rect ExtendedFrame(HWND hwnd) {
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        GetWindowRect(hwnd, &rect);
    }
    return ToCoreRect(rect);
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
