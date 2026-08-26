#pragma once

// Start-with-Windows, shared by the installer and the app.
//
// Windows has two records for a classic Run-key startup item:
//   1. HKCU\...\Run contains the command.
//   2. Explorer\StartupApproved\Run can veto that command.
// The second record matters: Task Manager and Settings disable an item by changing the
// approval byte while leaving the Run value in place. Treating Run-value presence as
// "enabled" makes the tray menu lie and is exactly the failure this file must prevent.

#include "AppIdentity.h"

#include <windows.h>

#include <string>
#include <vector>

namespace windowmark::app {

inline constexpr wchar_t kStartupApprovedKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

namespace detail {

struct AutoStartLocation {
    const wchar_t* runKeyPath;
    const wchar_t* approvedKeyPath;
    const wchar_t* valueName;
};

[[nodiscard]] inline bool HasRunnableCommand(const AutoStartLocation& location) {
    if (!location.runKeyPath || !location.valueName || location.valueName[0] == L'\0') return false;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, location.runKeyPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, location.valueName, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> command(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, location.valueName, nullptr, &type,
                              reinterpret_cast<BYTE*>(command.data()), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && command[0] != L'\0';
}

[[nodiscard]] inline bool ApprovalAllowsStartup(const AutoStartLocation& location) {
    if (!location.approvedKeyPath || !location.valueName || location.valueName[0] == L'\0') {
        return false;
    }

    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, location.approvedKeyPath, 0,
                                         KEY_QUERY_VALUE, &key);
    // No approval record means Windows has never vetoed this Run item.
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, location.valueName, nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return true;
    }
    if (status != ERROR_SUCCESS || type != REG_BINARY || bytes == 0) {
        RegCloseKey(key);
        return false;
    }

    std::vector<BYTE> state(bytes);
    status = RegQueryValueExW(key, location.valueName, nullptr, &type, state.data(), &bytes);
    RegCloseKey(key);
    // 0x02 is Windows' enabled state. Any other recorded state is treated as a veto;
    // failing closed is better than showing a checked menu item that will not launch.
    return status == ERROR_SUCCESS && !state.empty() && state[0] == 0x02;
}

[[nodiscard]] inline bool IsAutoStartEnabled(const AutoStartLocation& location) {
    return HasRunnableCommand(location) && ApprovalAllowsStartup(location);
}

inline bool WriteApprovalEnabled(const AutoStartLocation& location) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, location.approvedKeyPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    BYTE enabled[12]{};
    enabled[0] = 0x02;
    const LSTATUS status = RegSetValueExW(key, location.valueName, 0, REG_BINARY, enabled,
                                          sizeof(enabled));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// `exePath` is what Windows will run at logon. The explicit --autostart marker lets the
// app record whether Windows actually attempted the launch after the next sign-in.
inline bool SetAutoStart(const AutoStartLocation& location, const std::wstring& exePath,
                         bool enable) {
    if (!location.runKeyPath || !location.approvedKeyPath || !location.valueName ||
        location.valueName[0] == L'\0') {
        return false;
    }

    HKEY key = nullptr;
    if (!enable) {
        const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, location.runKeyPath, 0,
                                             KEY_SET_VALUE, &key);
        if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS) return false;
        LSTATUS status = RegDeleteValueW(key, location.valueName);
        RegCloseKey(key);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        return status == ERROR_SUCCESS;
    }

    if (exePath.empty() || exePath.find(L'"') != std::wstring::npos) return false;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, location.runKeyPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring command = L"\"" + exePath + L"\" --autostart";
    const LSTATUS status = RegSetValueExW(
        key, location.valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return false;

    // Write the command first, then clear the veto. This leaves no window in which Windows
    // sees an approval record for an item that does not exist yet. Failure is returned to
    // the caller instead of being swallowed, so the installer/menu can tell the user.
    return WriteApprovalEnabled(location);
}

} // namespace detail

[[nodiscard]] inline detail::AutoStartLocation ProductionAutoStartLocation() {
    return {kRunKeyPath, kStartupApprovedKeyPath, kRunValueName};
}

[[nodiscard]] inline bool IsAutoStartEnabled() {
    return detail::IsAutoStartEnabled(ProductionAutoStartLocation());
}

inline bool SetAutoStart(const std::wstring& exePath, bool enable) {
    return detail::SetAutoStart(ProductionAutoStartLocation(), exePath, enable);
}

} // namespace windowmark::app
