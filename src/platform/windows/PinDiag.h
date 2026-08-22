#pragma once

// Tracing for the pin path. The path crosses three layers - tray input, Coordinator
// policy, Win32 call - and from outside the process a failure in any of them looks
// identical: nothing happens.
//
// Switched on by the presence of %LOCALAPPDATA%\WindowMark\diag.on rather than an
// environment variable, so the installed copy can be traced while the user drives it by
// hand. A grab is a mouse gesture; it cannot be reproduced by sending the app messages,
// because only the foreground window may capture the mouse and a process that sends
// itself a command never becomes one.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace windowmark::win {

[[nodiscard]] inline std::wstring PinDiagDir() {
    wchar_t dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) == 0) return {};
    // Forward slashes on purpose: the Win32 file APIs take them, and it keeps these paths
    // free of the backslash escaping that every text tool in the chain mangles.
    return std::wstring(dir) + L"/WindowMark/";
}

[[nodiscard]] inline bool PinDiagOn() {
    const auto dir = PinDiagDir();
    if (dir.empty()) return false;
    // Checked every call rather than cached: the marker is meant to be dropped in and
    // deleted while the app keeps running, so a cached answer would need a restart to
    // notice either.
    return GetFileAttributesW((dir + L"diag.on").c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline void PinDiag(const wchar_t* format, ...) {
    if (!PinDiagOn()) return;
    const auto dir = PinDiagDir();
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"diag.log").c_str(), L"a, ccs=UTF-8") != 0 || f == nullptr) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fwprintf(f, L"[PIN %02d:%02d:%02d.%03d] ", now.wHour, now.wMinute, now.wSecond,
                  now.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfwprintf(f, format, args);
    va_end(args);
    std::fwprintf(f, L"\n");
    std::fclose(f);
}

} // namespace windowmark::win
