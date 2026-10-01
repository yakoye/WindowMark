#include "SelfInstall.h"

#include "InstallerCommon.h"

#include "AppIdentity.h"
#include "ClipKeeperIdentity.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windows.h>

#include <array>
#include <iterator>
#include <string>
#include <system_error>

namespace app = windowmark::app;

namespace windowmark::setup {
namespace {

constexpr int kInstallButtonId = 101;
constexpr int kUninstallButtonId = 201;

// 安装目录下这些文件是旧版留下的：那时安装程序、卸载程序和两个诊断工具都躺在顶层。
// 现在顶层只应当有 WindowMark.exe，工具都在 tools\ 里。不删，用户打开安装目录看到的
// 还是一堆 exe，而且那个旧的 WindowMarkUninstall.exe 会去读已经被改写的注册表项。
constexpr std::array<const wchar_t*, 5> kLegacyTopLevelFiles{
    L"WindowMarkSetup.exe", L"WindowMarkUninstall.exe", L"WindowMarkDiag.exe",
    L"WindowMarkInspect.exe", L"ClipKeeper.exe",
};

// 跟着主程序一起装的附属 exe，都放 tools\ 子目录。ClipKeeper 是可选功能（剪贴板守护），
// Diag/Inspect 是出问题时让用户双击跑一下的诊断工具。
constexpr std::array<const wchar_t*, 3> kToolExeNames{
    L"ClipKeeper.exe", L"WindowMarkDiag.exe", L"WindowMarkInspect.exe",
};

constexpr wchar_t kToolsDirName[] = L"tools";

void EnsureCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
}

void ShowMessage(PCWSTR title, PCWSTR icon, const std::wstring& instruction,
                 const std::wstring& detail) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = title;
    config.pszMainIcon = icon;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = detail.c_str();
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

// 返回 false = 用户取消。startWithWindows 进来是当前状态，出去是用户勾的结果。
bool AskToInstall(bool alreadyInstalled, const std::filesystem::path& installDir,
                  bool& startWithWindows) {
    const std::wstring instruction = alreadyInstalled
        ? std::wstring(L"更新已安装的 WindowMark 到 ") + app::kProductVersion
        : std::wstring(L"把 WindowMark ") + app::kProductVersion + L" 安装到系统";

    std::wstring content = L"安装位置：\n" + installDir.wstring() +
                           L"\n\n会做的事：拷一个 exe 过去、建开始菜单快捷方式、注册「设置 - "
                           L"应用」里的卸载入口。只写当前用户目录，不装服务、驱动或 "
                           L"Explorer 扩展。\n\n装完之后当前这份会退出，换成安装的那份运行；"
                           L"现在这个文件夹就可以删了。";

    const TASKDIALOG_BUTTON buttons[] = {
        {kInstallButtonId, alreadyInstalled ? L"更新" : L"安装"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"WindowMark 安装";
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
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, &verified))) return false;

    startWithWindows = verified != FALSE;
    return pressed == kInstallButtonId;
}

bool AskToUninstall(bool& purge) {
    std::wstring content =
        L"将删除程序文件、开始菜单快捷方式、开机自启项和「设置 - 应用」里的卸载入口。\n\n"
        L"正在运行的 WindowMark 会被先关掉。";

    const TASKDIALOG_BUTTON buttons[] = {
        {kUninstallButtonId, L"卸载"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"WindowMark 卸载";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"卸载 WindowMark";
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDCANCEL;
    config.pszVerificationText = L"同时删除我的设置和缓存数据";
    if (purge) config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;

    int pressed = 0;
    BOOL verified = FALSE;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, &verified))) return false;

    purge = verified != FALSE;
    return pressed == kUninstallButtonId;
}

// 启动指定的 exe。刚写出来的文件偶尔会一次启动不起来（杀毒扫描、句柄还没放），所以重试，
// 而不是把第一次失败当定论。
bool Launch(const std::filesystem::path& exePath, const wchar_t* arguments) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) Sleep(250);

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        // NO_UI：静默安装时 ShellExecuteEx 自己弹的错误框没人去点，会把安装挂住。
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        const std::wstring file = exePath.wstring();
        const std::wstring dir = exePath.parent_path().wstring();
        info.lpFile = file.c_str();
        info.lpParameters = arguments;
        info.lpDirectory = dir.c_str();
        info.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&info)) continue;
        if (!info.hProcess) return true;
        CloseHandle(info.hProcess);
        return true;
    }
    return false;
}

std::filesystem::path ToolSource(const std::filesystem::path& selfDir, const wchar_t* name) {
    // 发布包的样子：tools\ 子目录。
    const auto packaged = selfDir / kToolsDirName / name;
    std::error_code ec;
    if (std::filesystem::exists(packaged, ec)) return packaged;
    // 直接从构建目录安装时所有 exe 都在一层里，LocatePayload 认得那几个位置。
    return LocatePayload(name);
}

// 附属工具装不上只是少几个功能，主程序照常工作，所以记进 warnings 而不是让安装失败。
void CopyTools(const std::filesystem::path& selfDir, const std::filesystem::path& installDir,
               std::wstring& warnings) {
    const auto toolsDir = installDir / kToolsDirName;
    std::wstring error;
    if (!EnsureDirectory(toolsDir, error)) {
        warnings += error + L"\n";
        return;
    }
    for (const wchar_t* name : kToolExeNames) {
        const auto source = ToolSource(selfDir, name);
        if (source.empty()) {
            warnings += L"未找到 " + std::wstring(name) + L"，对应功能不可用。\n";
            continue;
        }
        if (!CopyFileTo(source, toolsDir / name, error)) warnings += error + L"\n";
    }
}

void RemoveLegacyTopLevelFiles(const std::filesystem::path& installDir) {
    std::error_code ec;
    for (const wchar_t* name : kLegacyTopLevelFiles) {
        std::filesystem::remove(installDir / name, ec);
    }
}

// fromTray：这次安装是正在运行的实例自己发起的。那就不能去「停掉正在运行的实例」——
// 要停的就是我们自己。单实例互斥体保证了此刻不会有第二份 WindowMark 在跑，所以也没有
// 别人占着目标文件。
int DoInstall(bool silent, bool fromTray, bool& quitSelf) {
    quitSelf = false;
    EnsureCommonControls();

    const auto self = SelfPath();
    const auto selfDir = SelfDir();
    const auto installDir = InstallDir();
    if (self.empty() || installDir.empty()) {
        if (!silent) {
            ShowMessage(L"WindowMark 安装", TD_ERROR_ICON, L"无法确定安装位置",
                        L"读取 %LOCALAPPDATA% 失败。");
        }
        return 3;
    }

    const auto targetExe = installDir / app::kMainExeName;
    std::error_code ec;
    if (std::filesystem::equivalent(selfDir, installDir, ec) && !ec) {
        if (!silent) {
            ShowMessage(L"WindowMark 安装", TD_INFORMATION_ICON, L"已经是安装好的那份",
                        L"当前运行的就是安装目录里的 WindowMark：\n" + installDir.wstring());
        }
        return 0;
    }

    const bool alreadyInstalled = std::filesystem::exists(targetExe, ec);
    bool startWithWindows = IsStartWithWindowsEnabled();
    if (HasSwitch(L"/StartWithWindows")) startWithWindows = true;
    if (HasSwitch(L"/NoStartWithWindows")) startWithWindows = false;

    if (!silent && !AskToInstall(alreadyInstalled, installDir, startWithWindows)) return 1;

    // 命令行安装时可能有一份已安装的在跑，占着目标文件。托盘发起时跳过——见函数头。
    if (!fromTray) StopRunningInstances(4000);
    // ClipKeeper 是独立进程，不受单实例互斥体约束，运行中它的 exe 覆盖不了。
    StopRunningInstances(4000, clipkeeper::kExeName, clipkeeper::kWindowClass,
                         clipkeeper::kRequestQuitMessage);

    std::wstring error;
    if (!EnsureDirectory(installDir, error)) {
        if (!silent) ShowMessage(L"WindowMark 安装", TD_ERROR_ICON, L"安装失败", error);
        return 4;
    }
    if (!CopyFileTo(self, targetExe, error)) {
        if (!silent) ShowMessage(L"WindowMark 安装", TD_ERROR_ICON, L"安装失败", error);
        return 5;
    }

    std::wstring warnings;
    RemoveLegacyTopLevelFiles(installDir);
    CopyTools(selfDir, installDir, warnings);

    if (!WriteUninstallEntry(installDir, DirectorySizeKb(installDir))) {
        warnings += L"卸载入口注册失败，「设置 - 应用」里可能看不到 WindowMark。\n";
    }
    if (!SetStartWithWindows(targetExe, startWithWindows)) {
        warnings += L"开机自启设置失败。\n";
    }
    if (!CreateStartMenuShortcut(targetExe)) {
        warnings += L"开始菜单快捷方式创建失败。\n";
    }

    if (!silent) {
        std::wstring detail = L"安装位置：\n" + installDir.wstring() + L"\n\n设置文件：\n" +
                              LocalDataDir().wstring() +
                              L"\n\n卸载：托盘菜单里的「卸载 WindowMark」，或「设置 - 应用」。";
        if (!warnings.empty()) detail += L"\n\n以下项目需要注意：\n" + warnings;
        ShowMessage(L"WindowMark 安装", warnings.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON,
                    L"WindowMark 安装完成", detail);
    }

    // --replace：让新起来的那份等我们放开单实例互斥体，而不是一看有人在跑就默默退出。
    if (!Launch(targetExe, L"--replace")) {
        if (!silent) {
            ShowMessage(L"WindowMark 安装", TD_WARNING_ICON, L"WindowMark 未能启动",
                        L"文件已经装好了，但启动失败。可以从开始菜单手动启动。");
        }
        return 7;
    }
    quitSelf = true;
    return 0;
}

// 卸载程序通常就住在它要删掉的那个目录里，所以先把自己拷到 %TEMP% 再从那儿重新开工。
bool StageAndRelaunchUninstall(bool purge, bool silent) {
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(tempDir)), tempDir) == 0) return false;

    const std::filesystem::path staged =
        std::filesystem::path(tempDir) /
        (L"WindowMarkUninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");

    std::wstring error;
    if (!CopyFileTo(SelfPath(), staged, error)) return false;

    std::wstring arguments = L"--uninstall /staged";
    if (purge) arguments += L" /Purge";
    if (silent) arguments += L" /S";
    return Launch(staged, arguments.c_str());
}

int DoUninstall(bool silent, bool staged, bool purge, bool& quitSelf) {
    quitSelf = false;
    EnsureCommonControls();

    if (!staged) {
        if (!silent && !AskToUninstall(purge)) return 1;
        if (!StageAndRelaunchUninstall(purge, silent)) {
            if (!silent) {
                ShowMessage(L"WindowMark 卸载", TD_ERROR_ICON, L"卸载没能开始",
                            L"无法把卸载程序复制到临时目录。");
            }
            return 2;
        }
        // 临时副本接手了，它马上会来停掉我们。
        quitSelf = true;
        return 0;
    }

    StopRunningInstances(4000);
    // ClipKeeper 也要停。不是为了删它的文件——RemoveTree 是整个安装目录 remove_all，它
    // 自然跟着走——而是因为运行中的 exe 删不掉，会让整个 remove_all 失败并留下残留。
    StopRunningInstances(4000, clipkeeper::kExeName, clipkeeper::kWindowClass,
                         clipkeeper::kRequestQuitMessage);

    SetStartWithWindows({}, false);
    // ClipKeeper 面板上有自己的「开机启动」勾选框，写的是 Run\ClipKeeper。不清掉会留下
    // 一个指向已删除 exe 的启动项。
    if (HKEY runKey = nullptr;
        RegOpenKeyExW(HKEY_CURRENT_USER, app::kRunKeyPath, 0, KEY_SET_VALUE, &runKey) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(runKey, L"ClipKeeper");
        RegCloseKey(runKey);
    }
    RemoveStartMenuShortcut();
    RemoveUninstallEntry();

    std::wstring warnings;
    std::wstring error;
    if (!RemoveTree(InstallDir(), error)) warnings += error + L"\n";
    if (purge) {
        if (!RemoveTree(LocalDataDir(), error)) warnings += error + L"\n";
        if (!RemoveTree(RoamingDataDir(), error)) warnings += error + L"\n";
    }

    if (!silent) {
        std::wstring detail = purge
            ? L"程序文件、开机自启项、快捷方式，以及全部设置和缓存数据都已删除。"
            : L"程序文件、开机自启项和快捷方式已删除。\n设置保留在：\n" +
                  LocalDataDir().wstring();
        detail += L"\n\n没有安装过服务、驱动、Explorer 补丁或系统级注册表项，无需其他清理。";
        if (!warnings.empty()) detail += L"\n\n以下项目需要手动处理：\n" + warnings;
        ShowMessage(L"WindowMark 卸载", warnings.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON,
                    L"WindowMark 已卸载", detail);
    }

    ScheduleSelfDelete(SelfPath());
    return warnings.empty() ? 0 : 6;
}

} // namespace

bool HandleCommandLine(int& exitCode) {
    const bool install = HasSwitch(L"--install");
    const bool uninstall = HasSwitch(L"--uninstall");
    if (!install && !uninstall) return false;

    const bool silent = HasSwitch(L"/S") || HasSwitch(L"/silent");
    bool quitSelf = false;
    exitCode = install ? DoInstall(silent, false, quitSelf)
                       : DoUninstall(silent, HasSwitch(L"/staged"), HasSwitch(L"/Purge"), quitSelf);
    return true;
}

bool RunningFromInstallDir() {
    const auto here = SelfDir();
    const auto installed = InstallDir();
    if (here.empty() || installed.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(installed, ec)) return false;
    return std::filesystem::equivalent(here, installed, ec) && !ec;
}

bool InstallFromTray() {
    bool quitSelf = false;
    DoInstall(false, true, quitSelf);
    return quitSelf;
}

bool UninstallFromTray() {
    bool quitSelf = false;
    DoUninstall(false, false, false, quitSelf);
    return quitSelf;
}

} // namespace windowmark::setup
