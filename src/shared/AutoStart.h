#pragma once

// Start-with-Windows, shared by the installer and the app.
//
// The registry value under HKCU\...\Run is the *only* record of this setting. It is
// deliberately not mirrored into settings.conf: Windows lets the user turn a startup entry
// off from Task Manager and from Settings - Apps - Startup, and a copy in our own file
// would go on claiming the opposite forever. Reading the registry every time costs one
// key open and is always right.

#include "AppIdentity.h"

#include <windows.h>

#include <string>

namespace windowmark::app {

[[nodiscard]] inline bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status =
        RegQueryValueExW(key, kRunValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// `exePath` is what Windows will run at logon. Quoted, because the install path sits under
// a profile directory and those routinely contain spaces.
inline bool SetAutoStart(const std::wstring& exePath, bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status = ERROR_SUCCESS;
    if (enable) {
        const std::wstring quoted = L"\"" + exePath + L"\"";
        status = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(quoted.c_str()),
                                static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }

    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// Turning the entry on is not enough on its own. Windows keeps a separate approval byte
// per startup entry, and once the user has disabled the app from Task Manager or from
// Settings - Apps - Startup, that byte overrides the Run value: the entry is present,
// looks enabled to us, and never runs. Re-enabling from inside the app has to clear it,
// otherwise ticking the box appears to work and changes nothing at the next logon.
inline void ClearAutoStartVeto() {
    constexpr wchar_t kApprovedPath[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    // 02 00 ... = enabled. The remaining ten bytes are the disable timestamp, which is
    // meaningless for an enabled entry and which Windows itself zeroes.
    BYTE enabled[12]{};
    enabled[0] = 0x02;
    RegSetValueExW(key, kRunValueName, 0, REG_BINARY, enabled, sizeof(enabled));
    RegCloseKey(key);
}

} // namespace windowmark::app
