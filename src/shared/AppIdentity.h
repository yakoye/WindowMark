#pragma once

// Identity shared by WindowMark.exe, WindowMarkSetup.exe and WindowMarkUninstall.exe.
// Keeping the mutex name, registry paths and broadcast message names in one place is
// what lets the installers talk to a running instance instead of failing on it.

namespace windowmark::app {

inline constexpr wchar_t kProductName[] = L"WindowMark";
// Bump on every change that gets installed. If it is ever ambiguous whether a running
// binary is the current one, the build stamp in BuildStamp.h settles it - that one is
// written by the build itself and cannot be forgotten.
inline constexpr wchar_t kProductVersion[] = L"0.4.6";
inline constexpr wchar_t kPublisher[] = L"WindowMark";

inline constexpr wchar_t kSingletonMutex[] = L"Local\\WindowMark.Singleton.v0";

// The hidden tray control window. Messages are posted straight to it rather than to
// HWND_BROADCAST, so no other process on the system is disturbed.
inline constexpr wchar_t kControlWindowClass[] = L"WindowMark.Control";

// One layered window per outlined window. Shared here because WindowMarkInspect.exe finds
// the outlines on screen by class name; two copies of the string would drift.
inline constexpr wchar_t kBorderWindowClass[] = L"WindowMark.WindowBorder";

// Resolve with RegisterWindowMessageW before use.
inline constexpr wchar_t kRequestQuitMessage[] = L"WindowMark.RequestQuit.v1";
inline constexpr wchar_t kSecondInstanceMessage[] = L"WindowMark.SecondInstance.v1";

inline constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValueName[] = L"WindowMark";
inline constexpr wchar_t kUninstallKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WindowMark";

// WindowMark 自己的注册表根。到 v0.4.4 为止这里只有系统的 Run / Uninstall 键；配置文件
// 位置是第一件必须由 WindowMark 自己存起来的设置——它不可能存在配置文件里，因为得先
// 找到配置文件才能读它。ConfigPath 是 REG_SZ，存含文件名的完整路径，值不存在或为空串
// 等同于「未指定」。
inline constexpr wchar_t kProductKeyPath[] = L"Software\\WindowMark";
inline constexpr wchar_t kConfigPathValue[] = L"ConfigPath";

inline constexpr wchar_t kInstallSubdir[] = L"Programs\\WindowMark";
inline constexpr wchar_t kDataSubdir[] = L"WindowMark";
inline constexpr wchar_t kMainExeName[] = L"WindowMark.exe";
inline constexpr wchar_t kUninstallExeName[] = L"WindowMarkUninstall.exe";
inline constexpr wchar_t kShortcutName[] = L"WindowMark.lnk";

} // namespace windowmark::app
