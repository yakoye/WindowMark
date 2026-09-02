#pragma once

#include "windowmark/core/ConfigLocation.h"
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

// 配置文件的三个候选位置。存在性与可写性在这里判断，选哪一个交给 core 的
// ResolveConfigLocation，那段优先级逻辑因此可以脱离文件系统被单测覆盖。
[[nodiscard]] std::filesystem::path PortableConfigPath();
[[nodiscard]] std::filesystem::path ReadConfiguredConfigPath();
bool WriteConfiguredConfigPath(const std::filesystem::path& path);
[[nodiscard]] bool IsDirectoryWritable(const std::filesystem::path& directory);
[[nodiscard]] ConfigLocation CurrentConfigLocation();
[[nodiscard]] bool IsCloaked(HWND hwnd);
// `alsoExclude` is the user's own list of window classes, added to the built-in one.
[[nodiscard]] bool IsEligibleTopLevelWindow(HWND hwnd,
                                            const std::vector<std::wstring>& alsoExclude = {});
[[nodiscard]] Rect ExtendedFrame(HWND hwnd);
// Same thing, but says whether DWM actually answered. ExtendedFrame falls back to
// GetWindowRect on failure and the caller cannot tell the difference - which is how a
// bogus zero inset ended up cached and a border sat 8px out until the window was resized.
[[nodiscard]] Rect ExtendedFrame(HWND hwnd, bool& fromDwm);
[[nodiscard]] Rect WorkAreaFor(HWND hwnd);
// The system accent colour as 0xAARRGGBB, read fresh from the registry every call so a
// theme change is picked up without any plumbing to notice one. Shared rather than
// duplicated: the registry path is a wide string full of backslashes, and having a second
// copy of it is how the "accent never applied" bug would come back.
[[nodiscard]] unsigned SystemAccentColor();
void PurgeAllUserData();
void RemoveStartupRegistration();

} // namespace windowmark::win
