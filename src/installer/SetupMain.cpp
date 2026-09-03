// WindowMarkSetup.exe - per-user installer.
//
// Deliberately does the same amount of work the old install.ps1 did: copy into
// %LOCALAPPDATA%\Programs\WindowMark, optionally register a startup entry, and start
// the app. No service, no driver, no shell extension, no machine-wide registry state.
//
// A running WindowMark is never an error here. It is asked to close, waited for, and
// restarted after the files are replaced.

#include "InstallerCommon.h"

#include "AppIdentity.h"
#include "ClipKeeperIdentity.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <string>

#pragma comment(linker,                                                      \
                "/manifestdependency:\"type='win32' "                        \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace setup = windowmark::setup;
namespace app = windowmark::app;

namespace {

constexpr int kInstallButtonId = 101;
constexpr int kLaunchButtonId = 102;
constexpr int kCloseButtonId = 103;

class ScopedCom {
public:
    ScopedCom() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT hr_{};
};

void ShowError(const std::wstring& instruction, const std::wstring& detail) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = L"WindowMark 安装程序";
    config.pszMainIcon = TD_ERROR_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = detail.c_str();
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

// Returns false when the user cancels.
bool AskToInstall(bool alreadyInstalled, bool running, const std::filesystem::path& installDir,
                  bool& startWithWindows) {
    const std::wstring instruction = alreadyInstalled
        ? std::wstring(L"更新 WindowMark 到 ") + app::kProductVersion
        : std::wstring(L"安装 WindowMark ") + app::kProductVersion;

    std::wstring content = L"安装位置：\n" + installDir.wstring() +
                           L"\n\n只写入当前用户目录，不安装服务、驱动或 Explorer 扩展。";
    if (running) {
        content += L"\n\n检测到 WindowMark 正在运行，安装程序会自动关闭并在完成后重新启动它。";
    }

    const TASKDIALOG_BUTTON buttons[] = {
        {kInstallButtonId, alreadyInstalled ? L"更新" : L"安装"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"WindowMark 安装程序";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = kInstallButtonId;
    config.pszVerificationText = L"开机时自动启动 WindowMark";
    if (startWithWindows) config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;

    int pressed = 0;
    BOOL verified = FALSE;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, &verified))) {
        return false;
    }

    startWithWindows = verified != FALSE;
    return pressed == kInstallButtonId;
}

// Returns true when the user wants the app started now.
bool ShowSuccess(const std::filesystem::path& installDir, const std::wstring& warnings) {
    std::wstring content = L"安装位置：\n" + installDir.wstring() +
                           L"\n\n用户配置：\n" + setup::LocalDataDir().wstring() +
                           L"\n\n可以在「设置 - 应用」中卸载，或直接运行安装目录下的 " +
                           app::kUninstallExeName + L"。";
    if (!warnings.empty()) {
        content += L"\n\n" + warnings;
    }

    const TASKDIALOG_BUTTON buttons[] = {
        {kLaunchButtonId, L"立即启动 WindowMark"},
        {kCloseButtonId, L"完成"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    config.pszWindowTitle = L"WindowMark 安装程序";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"WindowMark 安装完成";
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = kLaunchButtonId;

    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        return false;
    }
    return pressed == kLaunchButtonId;
}

bool LaunchInstalled(const std::filesystem::path& exePath) {
    // Make sure no predecessor still owns the single-instance mutex, or the process we
    // are about to start will treat itself as a second launch and quit without a word.
    setup::WaitForSingletonRelease(4000);

    // A file that was written moments ago can briefly refuse to launch - a scanner or a
    // lingering handle is enough. Retry rather than treating the first failure as final.
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) Sleep(250);

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        // NO_UI matters: without it ShellExecuteEx pops its own error dialog on failure
        // and blocks waiting for a click, which turns a silent /S install into a hang.
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = exePath.wstring().c_str();
        info.lpDirectory = exePath.parent_path().wstring().c_str();
        info.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&info)) continue;
        if (!info.hProcess) return true;

        // WindowMark is a tray app: it should still be alive a moment later. An early
        // exit means it bailed out on startup, which is otherwise completely silent.
        const bool exitedEarly = WaitForSingleObject(info.hProcess, 1200) == WAIT_OBJECT_0;
        CloseHandle(info.hProcess);
        if (!exitedEarly) return true;
    }
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScopedCom com;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const bool silent = setup::HasSwitch(L"/S") || setup::HasSwitch(L"/silent");
    const bool forceAutoStart = setup::HasSwitch(L"/StartWithWindows");
    const bool forceNoAutoStart = setup::HasSwitch(L"/NoStartWithWindows");

    const auto sourceExe = setup::LocatePayload(app::kMainExeName);
    const auto sourceUninstaller = setup::LocatePayload(app::kUninstallExeName);
    const auto sourceClipKeeper = setup::LocatePayload(windowmark::clipkeeper::kExeName);

    if (sourceExe.empty()) {
        if (!silent) {
            ShowError(L"找不到 WindowMark.exe",
                      L"请把安装程序和 WindowMark.exe 放在同一个目录，或先运行 build.bat 完成构建。");
        }
        return 2;
    }

    const auto installDir = setup::InstallDir();
    if (installDir.empty()) {
        if (!silent) ShowError(L"无法确定安装目录", L"读取 %LOCALAPPDATA% 失败。");
        return 3;
    }

    const auto targetExe = installDir / app::kMainExeName;
    const auto targetUninstaller = installDir / app::kUninstallExeName;
    const auto targetClipKeeper = installDir / windowmark::clipkeeper::kExeName;

    std::error_code ec;
    const bool alreadyInstalled = std::filesystem::exists(targetExe, ec);
    const bool running = !setup::FindRunningInstances().empty();

    bool startWithWindows = setup::IsStartWithWindowsEnabled();
    if (forceAutoStart) startWithWindows = true;
    if (forceNoAutoStart) startWithWindows = false;

    if (!silent) {
        if (!AskToInstall(alreadyInstalled, running, installDir, startWithWindows)) {
            return 1;
        }
    }

    // A running instance is expected, not an error: close it, then replace the files.
    setup::StopRunningInstances(4000);
    // 同理，运行中的 ClipKeeper 会让它的 exe 覆盖不了。
    setup::StopRunningInstances(4000, windowmark::clipkeeper::kExeName,
                                windowmark::clipkeeper::kWindowClass,
                                windowmark::clipkeeper::kRequestQuitMessage);

    std::wstring error;
    if (!setup::EnsureDirectory(installDir, error)) {
        if (!silent) ShowError(L"安装失败", error);
        return 4;
    }
    if (!setup::CopyFileTo(sourceExe, targetExe, error)) {
        if (!silent) ShowError(L"安装失败", error);
        return 5;
    }

    std::wstring warnings;
    if (sourceUninstaller.empty()) {
        warnings += L"未找到 " + std::wstring(app::kUninstallExeName) +
                    L"，本次没有注册卸载入口。\n";
    } else if (!setup::CopyFileTo(sourceUninstaller, targetUninstaller, error)) {
        warnings += error + L"\n";
    } else if (!setup::WriteUninstallEntry(installDir, setup::DirectorySizeKb(installDir))) {
        warnings += L"卸载入口注册失败，「设置 - 应用」中可能看不到 WindowMark。\n";
    }

    // 剪贴板守护是可选组件：装不上只是少一个功能，主程序照常工作，所以走 warning
    // 而不是像主 exe 那样直接失败返回。
    if (sourceClipKeeper.empty()) {
        warnings += L"未找到 " + std::wstring(windowmark::clipkeeper::kExeName) +
                    L"，剪贴板守护将不可用。\n";
    } else if (!setup::CopyFileTo(sourceClipKeeper, targetClipKeeper, error)) {
        warnings += error + L"\n";
    }

    if (!setup::SetStartWithWindows(targetExe, startWithWindows)) {
        warnings += L"开机自启设置失败。\n";
    }
    if (!setup::CreateStartMenuShortcut(targetExe)) {
        warnings += L"开始菜单快捷方式创建失败。\n";
    }

    if (silent) {
        // Report the launch outcome: a scripted install has no other way to notice that
        // the files landed but the app never came up.
        return LaunchInstalled(targetExe) ? 0 : 7;
    }

    if (ShowSuccess(installDir, warnings)) {
        if (!LaunchInstalled(targetExe)) {
            ShowError(L"WindowMark 未能启动",
                      L"文件已安装完成，但启动失败。请从开始菜单手动启动 WindowMark。");
            return 7;
        }
    }
    return 0;
}
