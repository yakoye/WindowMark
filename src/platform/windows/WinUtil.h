#pragma once

#include "windowmark/core/Types.h"

#include <filesystem>
#include <string>
#include <vector>
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
// `alsoExclude` is the user's own list of window classes, added to the built-in one.
[[nodiscard]] bool IsEligibleTopLevelWindow(HWND hwnd,
                                            const std::vector<std::wstring>& alsoExclude = {});
[[nodiscard]] Rect ExtendedFrame(HWND hwnd);
[[nodiscard]] Rect WorkAreaFor(HWND hwnd);
// The system accent colour as 0xAARRGGBB, read fresh from the registry every call so a
// theme change is picked up without any plumbing to notice one. Shared rather than
// duplicated: the registry path is a wide string full of backslashes, and having a second
// copy of it is how the "accent never applied" bug would come back.
[[nodiscard]] unsigned SystemAccentColor();
void PurgeAllUserData();
void RemoveStartupRegistration();

} // namespace windowmark::win
