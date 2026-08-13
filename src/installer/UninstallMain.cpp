// WindowMarkUninstall.exe - per-user uninstaller.
//
// A running WindowMark is never an error: it is asked to close, waited for, and only
// force-terminated if it does not go. That is safe because every resource WindowMark
// owns (WinEvent hooks, DWM thumbnails, overlay HWNDs) is process-owned user-mode
// state that disappears with the process.
//
// The uninstaller normally lives inside the directory it has to delete, so it first
// re-launches itself from %TEMP% ("staged") and cleans up that copy on the way out.

#include "InstallerCommon.h"

#include "AppIdentity.h"

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

constexpr int kUninstallButtonId = 201;

class ScopedCom {
public:
    ScopedCom() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT hr_{};
};

void ShowMessage(PCWSTR icon, const std::wstring& instruction, const std::wstring& detail) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = L"WindowMark 卸载程序";
    config.pszMainIcon = icon;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = detail.c_str();
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

bool AskToUninstall(bool running, bool& purge) {
    std::wstring content =
        L"将删除程序文件、开始菜单快捷方式、开机自启项和「设置 - 应用」中的卸载入口。";
    if (running) {
        content += L"\n\n检测到 WindowMark 正在运行，卸载程序会先自动关闭它。";
    }
    content += L"\n\n默认保留 " + setup::LocalDataDir().wstring() + L" 中的个人设置。";

    const TASKDIALOG_BUTTON buttons[] = {
        {kUninstallButtonId, L"卸载"},
    };

    // Must outlive TaskDialogIndirect: pointing pszMainInstruction at a temporary's
    // c_str() leaves the dialog reading freed memory, which shows up as a garbled title.
    const std::wstring instruction = std::wstring(L"卸载 WindowMark ") + app::kProductVersion;

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"WindowMark 卸载程序";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDCANCEL;
    config.pszVerificationText = L"同时删除我的设置和数据";
    if (purge) config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;

    int pressed = 0;
    BOOL verified = FALSE;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, &verified))) {
        return false;
    }

    purge = verified != FALSE;
    return pressed == kUninstallButtonId;
}

// Copies this executable to %TEMP% and restarts it from there so the install directory
// can be deleted. Returns false when staging is not possible; the caller then proceeds
// in place, which still works when the uninstaller is run from a build tree.
bool StageAndRelaunch(bool purge, bool silent) {
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(tempDir)), tempDir) == 0) return false;

    const std::filesystem::path staged =
        std::filesystem::path(tempDir) /
        (L"WindowMarkUninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");

    std::wstring error;
    if (!setup::CopyFileTo(setup::SelfPath(), staged, error)) return false;

    std::wstring arguments = L"/staged";
    if (purge) arguments += L" /Purge";
    if (silent) arguments += L" /S";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // NO_UI so a failure here surfaces as a return value instead of a modal error box
    // that nobody is watching during a /S uninstall.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = staged.wstring().c_str();
    info.lpParameters = arguments.c_str();
    info.lpDirectory = tempDir;
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        std::error_code ec;
        std::filesystem::remove(staged, ec);
        return false;
    }
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

bool RunningFromInstallDir() {
    const auto here = setup::SelfDir();
    const auto installed = setup::InstallDir();
    if (here.empty() || installed.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(installed, ec)) return false;
    return std::filesystem::equivalent(here, installed, ec) && !ec;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScopedCom com;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const bool staged = setup::HasSwitch(L"/staged");
    const bool silent = setup::HasSwitch(L"/S") || setup::HasSwitch(L"/silent");
    bool purge = setup::HasSwitch(L"/Purge");

    if (!staged) {
        const bool running = !setup::FindRunningInstances().empty();
        if (!silent && !AskToUninstall(running, purge)) {
            return 1;
        }
        if (RunningFromInstallDir() && StageAndRelaunch(purge, silent)) {
            return 0;
        }
    }

    // A running instance is expected, not an error.
    setup::StopRunningInstances(4000);

    setup::SetStartWithWindows({}, false);
    setup::RemoveStartMenuShortcut();
    setup::RemoveUninstallEntry();

    std::wstring warnings;
    std::wstring error;
    if (!setup::RemoveTree(setup::InstallDir(), error)) {
        warnings += error + L"\n";
    }

    if (purge) {
        if (!setup::RemoveTree(setup::LocalDataDir(), error)) warnings += error + L"\n";
        if (!setup::RemoveTree(setup::RoamingDataDir(), error)) warnings += error + L"\n";
    }

    if (!silent) {
        std::wstring detail = purge
            ? L"程序文件、开机自启项、快捷方式，以及全部设置和缓存数据都已删除。"
            : L"程序文件、开机自启项和快捷方式已删除。\n个人设置保留在：\n" +
                  setup::LocalDataDir().wstring();
        detail += L"\n\n没有安装过服务、驱动、Explorer 补丁或系统级注册表项，无需其他清理。";
        if (!warnings.empty()) {
            detail += L"\n\n以下项目需要手动处理：\n" + warnings;
        }
        ShowMessage(warnings.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON,
                    L"WindowMark 已卸载", detail);
    }

    if (staged) {
        setup::ScheduleSelfDelete(setup::SelfPath());
    }
    return warnings.empty() ? 0 : 6;
}
