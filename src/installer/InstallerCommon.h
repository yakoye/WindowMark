#pragma once

// 默认参数要用到 kMainExeName / kControlWindowClass / kRequestQuitMessage。
#include "AppIdentity.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace windowmark::setup {

// --- process control -------------------------------------------------------

struct RunningInstance {
    DWORD processId{};
    std::filesystem::path imagePath;
};

[[nodiscard]] std::vector<RunningInstance> FindRunningInstances(
    const wchar_t* exeName = app::kMainExeName);

// Asks every running WindowMark to close through its own message loop, waits up to
// graceMs, and only then force-terminates whatever is left. Force termination is safe
// here: hooks, DWM thumbnails and overlay HWNDs are all process-owned user-mode
// resources that disappear with the process.
// Returns true when no instance is running any more, which is also the result when
// none was running to begin with.
//
// 三个标识串带默认值，是为了让同一套「先礼后兵」的逻辑也能停 ClipKeeper——它有自己的
// exe 名、窗口类和退出消息。默认值保持原样，现有调用点一字不用改。
bool StopRunningInstances(unsigned graceMs,
                          const wchar_t* exeName = app::kMainExeName,
                          const wchar_t* windowClass = app::kControlWindowClass,
                          const wchar_t* quitMessage = app::kRequestQuitMessage);

// Blocks until nobody holds the single-instance mutex, so a freshly launched WindowMark
// will not mistake a predecessor that is still exiting for "already running" and quit.
bool WaitForSingletonRelease(unsigned timeoutMs);

// --- locations -------------------------------------------------------------

[[nodiscard]] std::filesystem::path SelfPath();
[[nodiscard]] std::filesystem::path SelfDir();
[[nodiscard]] std::filesystem::path InstallDir();
[[nodiscard]] std::filesystem::path LocalDataDir();
[[nodiscard]] std::filesystem::path RoamingDataDir();
[[nodiscard]] std::filesystem::path StartMenuShortcutPath();

// Looks for `fileName` next to the running executable first, then in the usual
// build output directories so the installer also works straight from a build tree.
[[nodiscard]] std::filesystem::path LocatePayload(const wchar_t* fileName);

// --- registry / shell integration -----------------------------------------

bool SetStartWithWindows(const std::filesystem::path& exePath, bool enable);
[[nodiscard]] bool IsStartWithWindowsEnabled();

bool WriteUninstallEntry(const std::filesystem::path& installDir, unsigned long sizeKb);
void RemoveUninstallEntry();

bool CreateStartMenuShortcut(const std::filesystem::path& target);
void RemoveStartMenuShortcut();

// --- filesystem helpers ----------------------------------------------------

bool EnsureDirectory(const std::filesystem::path& dir, std::wstring& error);
bool CopyFileTo(const std::filesystem::path& from, const std::filesystem::path& to, std::wstring& error);
bool RemoveTree(const std::filesystem::path& dir, std::wstring& error);
[[nodiscard]] unsigned long DirectorySizeKb(const std::filesystem::path& dir);

// Schedules `path` for deletion by a detached shell command once this process exits.
// Used by the uninstaller to remove the copy of itself it is running from.
void ScheduleSelfDelete(const std::filesystem::path& path);

// --- command line ----------------------------------------------------------

[[nodiscard]] bool HasSwitch(const wchar_t* name);
[[nodiscard]] std::wstring SwitchValue(const wchar_t* name);

[[nodiscard]] std::wstring DescribeLastError(DWORD code);

} // namespace windowmark::setup
