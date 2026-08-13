#pragma once

// Identity shared by WindowMark.exe, WindowMarkSetup.exe and WindowMarkUninstall.exe.
// Keeping the mutex name, registry paths and broadcast message names in one place is
// what lets the installers talk to a running instance instead of failing on it.

namespace windowmark::app {

inline constexpr wchar_t kProductName[] = L"WindowMark";
inline constexpr wchar_t kProductVersion[] = L"0.2.0";
inline constexpr wchar_t kPublisher[] = L"WindowMark";

inline constexpr wchar_t kSingletonMutex[] = L"Local\\WindowMark.Singleton.v0";

// The hidden tray control window. Messages are posted straight to it rather than to
// HWND_BROADCAST, so no other process on the system is disturbed.
inline constexpr wchar_t kControlWindowClass[] = L"WindowMark.Control";

// Resolve with RegisterWindowMessageW before use.
inline constexpr wchar_t kRequestQuitMessage[] = L"WindowMark.RequestQuit.v1";
inline constexpr wchar_t kSecondInstanceMessage[] = L"WindowMark.SecondInstance.v1";

inline constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValueName[] = L"WindowMark";
inline constexpr wchar_t kUninstallKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WindowMark";

inline constexpr wchar_t kInstallSubdir[] = L"Programs\\WindowMark";
inline constexpr wchar_t kDataSubdir[] = L"WindowMark";
inline constexpr wchar_t kMainExeName[] = L"WindowMark.exe";
inline constexpr wchar_t kUninstallExeName[] = L"WindowMarkUninstall.exe";
inline constexpr wchar_t kShortcutName[] = L"WindowMark.lnk";

} // namespace windowmark::app
