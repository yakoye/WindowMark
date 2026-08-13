#pragma once

#include "windowmark/core/Types.h"

#include <filesystem>
#include <string>
#include <windows.h>

namespace windowmark::win {

[[nodiscard]] std::string WideToUtf8(const std::wstring& value);
[[nodiscard]] std::wstring Utf8ToWide(const std::string& value);
[[nodiscard]] std::wstring QueryProcessPath(DWORD processId);
[[nodiscard]] std::string FileStemUtf8(const std::wstring& path);
[[nodiscard]] Rect ToCoreRect(const RECT& rect);
[[nodiscard]] RECT ToWinRect(const Rect& rect);
[[nodiscard]] std::filesystem::path InstalledExePath();
std::filesystem::path LocalDataRoot();
[[nodiscard]] std::filesystem::path RoamingDataRoot();
[[nodiscard]] bool IsCloaked(HWND hwnd);
[[nodiscard]] bool IsEligibleTopLevelWindow(HWND hwnd);
[[nodiscard]] Rect ExtendedFrame(HWND hwnd);
[[nodiscard]] Rect WorkAreaFor(HWND hwnd);
void PurgeAllUserData();
void RemoveStartupRegistration();

} // namespace windowmark::win
