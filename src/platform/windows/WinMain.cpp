#include "ClipKeeperIdentity.h"
#include "WinBorderBackend.h"
#include "WinConfigPathDialog.h"
#include "WinControlWindow.h"
#include "WinDragBackend.h"
#include "WinDragSettingsDialog.h"
#include "WinHomePanel.h"
#include "WinOverlayBackend.h"
#include "PinDiag.h"
#include "WinPinBackend.h"
#include "WinPreviewBackend.h"
#include "WinRenameDialog.h"
#include "WinSelectionDialog.h"
#include "WinSettingsDialog.h"
#include "WinUtil.h"
#include "WinWindowBackend.h"

#include "windowmark/core/Coordinator.h"
#include "windowmark/core/Hotkey.h"
#include "windowmark/core/Settings.h"

#include "AppIdentity.h"
#include "AutoStart.h"
#include "BuildStamp.h"
#include "InstallerCommon.h"
#include "SelfInstall.h"
#include "Resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Without this the app gets comctl32 v5 and the selection panel's tree view renders
// with pre-XP visuals.
#pragma comment(linker,                                                      \
                "/manifestdependency:\"type='win32' "                        \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

bool HasArgument(const wchar_t* expected) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], expected) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

// A Run-key launch is otherwise indistinguishable from "Windows never tried" once the
// process has disappeared. Keep a tiny append-only audit trail only for --autostart runs;
// ordinary manual launches stay silent. This deliberately uses LOCALAPPDATA and Win32/C
// primitives so it also works before COM, the coordinator and the tray window exist.
void LogAutoStartPhase(bool autoStartLaunch, const wchar_t* phase, DWORD code = 0) {
    if (!autoStartLaunch) return;

    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0 || length >= static_cast<DWORD>(std::size(localAppData))) return;

    const std::wstring directory = std::wstring(localAppData) + L"\\WindowMark";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, (directory + L"\\startup.log").c_str(), L"a, ccs=UTF-8") != 0 ||
        !file) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(file, L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu phase=%ls code=%lu\n",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
             now.wMilliseconds, GetCurrentProcessId(), phase, code);
    fclose(file);
}

class ScopedCom {
public:
    ScopedCom() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT hr_{};
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() { if (handle_) CloseHandle(handle_); }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
private:
    HANDLE handle_{};
};

BOOL CALLBACK PostToControlWindow(HWND hwnd, LPARAM lParam) {
    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) == 0) return TRUE;
    if (_wcsicmp(className, windowmark::app::kControlWindowClass) != 0) return TRUE;
    PostMessageW(hwnd, static_cast<UINT>(lParam), 0, 0);
    return TRUE;
}

// Nudges the instance that already owns the singleton mutex. Used both to say "you
// are already running" and, for --purge, to ask it to shut down first.
void NotifyExistingInstance(const wchar_t* messageName) {
    if (const UINT message = RegisterWindowMessageW(messageName)) {
        EnumWindows(PostToControlWindow, static_cast<LPARAM>(message));
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool autoStartLaunch = HasArgument(L"--autostart");
    LogAutoStartPhase(autoStartLaunch, L"attempt");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScopedCom com;
    if (!com.ok()) {
        LogAutoStartPhase(autoStartLaunch, L"com_failed", 2);
        MessageBoxW(nullptr, L"COM 初始化失败。WindowMark 未启动。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 2;
    }

    // 安装和卸载是同一个 exe 的两个模式（发布包里只有 WindowMark.exe 一个 exe）。必须在
    // 抢单实例互斥体之前处理：两边都要先停掉正在运行的那份，自己不能先把互斥体占上。
    if (int selfInstallExit = 0; windowmark::setup::HandleCommandLine(selfInstallExit)) {
        return selfInstallExit;
    }

    const bool purgeRequested = HasArgument(L"--purge");
    // --replace：刚被「安装到系统」启动起来的那份。发起安装的实例还活着（它要等我们起来
    // 才退），所以这里要等互斥体放开，而不是一看有人在跑就默默退出。
    if (HasArgument(L"--replace")) windowmark::setup::WaitForSingletonRelease(8000);
    ScopedHandle singleInstance(CreateMutexW(nullptr, FALSE, windowmark::app::kSingletonMutex));
    if (!singleInstance.get()) {
        LogAutoStartPhase(autoStartLaunch, L"mutex_failed", 3);
        return 3;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Being launched twice is normal (double-clicked shortcut, startup entry plus a
        // manual start). Hand the request to the live instance and leave quietly instead
        // of interrupting the user with an error dialog.
        if (purgeRequested) {
            NotifyExistingInstance(windowmark::app::kRequestQuitMessage);
            for (int waited = 0; waited < 4000; waited += 100) {
                Sleep(100);
                ScopedHandle probe(CreateMutexW(nullptr, FALSE, windowmark::app::kSingletonMutex));
                if (probe.get() && GetLastError() != ERROR_ALREADY_EXISTS) break;
            }
            windowmark::win::PurgeAllUserData();
            return 0;
        }
        NotifyExistingInstance(windowmark::app::kSecondInstanceMessage);
        LogAutoStartPhase(autoStartLaunch, L"already_running");
        return 0;
    }

    if (purgeRequested) {
        windowmark::win::PurgeAllUserData();
        MessageBoxW(nullptr,
                    L"WindowMark 的配置、缓存和开机自启项已清理。\n"
                    L"如果程序本体已安装，再运行一次 WindowMark.exe --uninstall 删除程序文件。",
                    L"WindowMark - 完全清理",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // 三层查找：exe 同目录 -> 注册表 ConfigPath -> %LOCALAPPDATA%。
    const windowmark::ConfigLocation configLocation = windowmark::win::CurrentConfigLocation();
    if (configLocation.path.empty()) {
        LogAutoStartPhase(autoStartLaunch, L"data_path_failed", 4);
        MessageBoxW(nullptr, L"无法确定配置文件位置。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 4;
    }
    const auto settingsPath = configLocation.path;
    // 诊断开关（diag.on）和日志（diag.log）跟着配置文件走：绿色版的配置在 exe 旁边，
    // 排查文件也就在 exe 旁边，整个文件夹仍然自带一切。startup.log 不跟——那条审计日志
    // 要在任何解析之前就能写，永远在 %LOCALAPPDATA%\WindowMark。
    windowmark::win::SetPinDiagDir(settingsPath.parent_path().wstring());

    // 指定过位置但那里已经没了（U 盘拔了、目录被删、变成只读）。这时候用的是默认位置的
    // 配置，用户看到的会是一套「回到默认」的设置——不说清楚他会以为软件把设置弄丢了。
    // 开机自启动时也照样弹：这是罕见事件，而后果是整套设置都不是他要的那份。
    if (configLocation.configuredUnavailable) {
        MessageBoxW(nullptr,
                    L"指定的配置文件位置当前不可用，已改用默认位置。\n\n"
                    L"常见原因：U 盘没插、目录被删、或那个位置变成了只读。\n"
                    L"接好之后在托盘菜单的「配置文件...」里重新指定即可。",
                    L"WindowMark", MB_OK | MB_ICONWARNING);
    }
    // 配置文件还不存在 = 这是第一次运行（绿色版解压就是这个状态）。LoadOrCreate 会把它
    // 建出来，所以要在那之前问。
    std::error_code firstRunEc;
    const bool firstRun = !std::filesystem::exists(settingsPath, firstRunEc);
    const windowmark::Settings settings = windowmark::Settings::LoadOrCreate(settingsPath);

    windowmark::win::WinWindowBackend windowBackend(settings.performance.geometryThrottleMs);
    windowmark::win::WinOverlayBackend overlayBackend;
    windowmark::win::WinPreviewBackend previewBackend;
    windowmark::win::WinBorderBackend borderBackend;
    windowmark::win::WinPinBackend pinBackend;
    windowmark::Coordinator coordinator(
        settings, windowBackend, overlayBackend, previewBackend, &borderBackend, &pinBackend);

    // Declared before Start() so the context-menu handlers below can capture it; it is
    // only actually started further down, once the coordinator is running.
    windowmark::win::WinControlWindow control;

    // 拖动的钩子。配置一变就要重新装/卸，所以放在 persist 旁边，两者总是一起调用。
    windowmark::win::WinDragBackend dragBackend;
    const auto applyDrag = [&]() {
        dragBackend.Apply(coordinator.CurrentSettings().drag);
    };

    const auto persist = [&]() {
        applyDrag();
        if (!windowmark::Settings::Save(settingsPath, coordinator.CurrentSettings())) {
            MessageBoxW(control.NativeHandle(),
                        L"设置已在本次运行中生效，但保存 settings.conf 失败，下次启动不会保留。",
                        L"WindowMark",
                        MB_OK | MB_ICONWARNING);
        }
    };

    // Every dialog below is modal to the hidden tray window. Disabling that window blocks
    // input to it but not the tray icon's callback message, so the menu stayed usable and
    // could stack a second copy of any dialog on top of the first.
    bool dialogOpen = false;
    HWND aboutDialog = nullptr;
    const auto exclusive = [&](auto&& body) {
        if (dialogOpen) return;
        dialogOpen = true;
        body();
        dialogOpen = false;
    };

    // Assigned once the control window exists, because registering a shortcut needs its
    // HWND. Declared here so the settings dialog - which is wired up before that - can
    // re-apply the shortcut the moment the user changes it.
    std::function<void()> reapplyHotkey;

    // 窗口边框 -> 排除应用. Mirrors handlers.onSelection, with two differences: the list is
    // keyed for borders rather than bookmarks, and the highlight follows the selected row
    // so the user can see which window a line refers to without any numbering.
    const auto excludeDragApps = [&](HWND owner) {
        auto selection = coordinator.DragSelectionSnapshot();
        windowmark::win::SelectionDialogOptions options;
        options.title = L"WindowMark - 排除不参与拖动的应用/窗口";
        options.checkedMeansExcluded = true;
        options.note =
            L"说明：勾上 = 按住修饰键也不拖动它。有些程序自己就用 Alt+拖动做别的事"
            L"（Photoshop 等），把它们勾上，手势就不会把那些操作吞掉。"
            L"这份名单和「不画边框」是分开的两件事。"
            L"选中一行时，对应窗口会在屏幕上高亮。";
        options.onHighlight = [&](windowmark::WindowId id) { coordinator.SetPinPreview(id); };
        const bool applied =
            windowmark::win::WinSelectionDialog::ShowModal(owner, selection, options);
        if (applied) {
            coordinator.ApplyDragSelection(selection);
            persist();
        }
        coordinator.SetPinPreview(0);
    };

    const auto excludeBorderApps = [&](HWND owner) {
        auto selection = coordinator.BorderSelectionSnapshot();
        windowmark::win::SelectionDialogOptions options;
        options.title = L"WindowMark - 排除不画边框的应用/窗口";
        options.checkedMeansExcluded = true;
        options.note =
            L"说明：勾上 = 不画边框。应用的勾选会保存；单个窗口的勾选只对本次运行有效。"
            L"勾上的应用，其下面的窗口一律不画。被置顶的窗口仍然会画——那是置顶生效的唯一提示。"
            L"选中一行时，对应窗口会在屏幕上高亮。";
        options.onHighlight = [&](windowmark::WindowId id) { coordinator.SetPinPreview(id); };
        const bool applied =
            windowmark::win::WinSelectionDialog::ShowModal(owner, selection, options);
        if (applied) {
            coordinator.ApplyBorderSelection(selection);
            persist();
        }
        return applied;
    };

    const auto openSettingsPage = [&](windowmark::win::SettingsPage page) {
        exclusive([&] {
            windowmark::Settings draft = coordinator.CurrentSettings();
            if (!windowmark::win::WinSettingsDialog::ShowModal(control.NativeHandle(), draft, page)) {
                return;
            }
            // 开关只归托盘管。对话框开着的时候托盘照样能切开关，draft 里却还是打开对话框
            // 那一刻的旧值——不在这里取回当前值，点「确定」就会把刚切的开关悄悄改回去。
            const windowmark::Settings& now = coordinator.CurrentSettings();
            draft.drawer.enabled = now.drawer.enabled;
            draft.border.enabled = now.border.enabled;
            draft.pin.enabled = now.pin.enabled;
            draft.drag.enabled = now.drag.enabled;
            coordinator.UpdateSettings(draft);
            control.SetBorderState(coordinator.CurrentSettings().border.enabled);
            control.SetDragState(coordinator.CurrentSettings().drag.enabled);
            control.SetPinState(coordinator.CurrentSettings().pin.enabled);
            if (reapplyHotkey) reapplyHotkey();
            persist();
        });
    };

    const auto openBookmarkSettings = [&]() {
        openSettingsPage(windowmark::win::SettingsPage::Bookmarks);
    };

    coordinator.SetMenuHandlers(
        [&](windowmark::WindowId id) {
            exclusive([&] {
                std::wstring name = windowmark::win::Utf8ToWide(coordinator.CustomLabel(id));
                const std::wstring title = windowmark::win::Utf8ToWide(coordinator.DefaultLabel(id));
                if (windowmark::win::WinRenameDialog::ShowModal(control.NativeHandle(), title, name)) {
                    coordinator.SetCustomLabel(id, windowmark::win::WideToUtf8(name));
                }
            });
        },
        openBookmarkSettings);

    if (!coordinator.Start()) {
        LogAutoStartPhase(autoStartLaunch, L"coordinator_failed", 5);
        MessageBoxW(nullptr,
                    L"WindowMark 初始化失败。程序已安全退出，不会修改 Explorer、任务栏或系统驱动。",
                    L"WindowMark",
                    MB_OK | MB_ICONERROR);
        return 5;
    }

    windowmark::win::WinControlWindow::Handlers handlers;
    // Master switch. "Anything on" turns everything off; everything off turns both back on,
    // so one click always changes something - a switch that can land on "half on" and then
    // do nothing visible on the next click would be worse than no switch.
    handlers.onToggleAll = [&]() {
        windowmark::Settings draft = coordinator.CurrentSettings();
        // 四个功能都算：书签、边框、置顶、拖动，和托盘上「暂停所有 / 启用所有」判断用的是
        // 同一组（以前那边看置顶、这边切拖动，两边对不上）。拖动暂停时连钩子一起卸掉；置顶
        // 暂停会先把钉住的窗口放开。剪贴板守护不在其列——那是独立进程。
        const bool anythingOn = draft.drawer.enabled || draft.border.enabled ||
                                draft.pin.enabled || draft.drag.enabled;
        draft.drawer.enabled = !anythingOn;
        draft.border.enabled = !anythingOn;
        draft.pin.enabled = !anythingOn;
        draft.drag.enabled = !anythingOn;
        coordinator.UpdateSettings(draft);
        control.SetEnabledState(draft.drawer.enabled);
        control.SetBorderState(draft.border.enabled);
        control.SetPinState(draft.pin.enabled);
        control.SetDragState(draft.drag.enabled);
        persist();
    };
    // 托盘菜单弹出前现读一遍：对勾永远以当前配置为准。
    handlers.onMenuOpening = [&]() {
        const windowmark::Settings& now = coordinator.CurrentSettings();
        control.SetEnabledState(now.drawer.enabled);
        control.SetBorderState(now.border.enabled);
        control.SetPinState(now.pin.enabled);
        control.SetDragState(now.drag.enabled);
    };
    handlers.onToggleBookmarks = [&]() {
        coordinator.SetOverlayEnabled(!coordinator.OverlayEnabled());
        control.SetEnabledState(coordinator.OverlayEnabled());
        persist();
    };
    handlers.onSelection = [&]() {
        exclusive([&] {
            auto selection = coordinator.SelectionSnapshot();
            if (windowmark::win::WinSelectionDialog::ShowModal(control.NativeHandle(), selection)) {
                coordinator.ApplySelection(selection);
                persist();
            }
        });
    };
    handlers.onBookmarkSettings = openBookmarkSettings;
    handlers.onToggleBorders = [&]() {
        windowmark::Settings draft = coordinator.CurrentSettings();
        draft.border.enabled = !draft.border.enabled;
        coordinator.UpdateSettings(draft);
        control.SetBorderState(draft.border.enabled);
        persist();
    };
    handlers.onBorderExcludeApps = [&]() {
        exclusive([&] { excludeBorderApps(control.NativeHandle()); });
    };
    handlers.onToggleDrag = [&]() {
        windowmark::Settings draft = coordinator.CurrentSettings();
        draft.drag.enabled = !draft.drag.enabled;
        coordinator.UpdateSettings(draft);
        control.SetDragState(draft.drag.enabled);
        persist();
    };
    handlers.onDragExcludeApps = [&]() {
        exclusive([&] { excludeDragApps(control.NativeHandle()); });
    };
    handlers.onDragSettings = [&]() {
        exclusive([&] {
            std::string modifiers = coordinator.CurrentSettings().drag.modifiers;
            if (windowmark::win::WinDragSettingsDialog::ShowModal(control.NativeHandle(),
                                                                 modifiers)) {
                // 关掉对话框之后再取当前配置：对话框开着时托盘可能切过开关，拿打开前的
                // 副本整份写回会把它改回去。这里只动修饰键这一项。
                windowmark::Settings draft = coordinator.CurrentSettings();
                draft.drag.modifiers = modifiers;
                coordinator.UpdateSettings(draft);
                persist();
            }
        });
    };
    handlers.onBorderSettings = [&]() {
        openSettingsPage(windowmark::win::SettingsPage::Borders);
    };
    handlers.onTogglePinning = [&]() {
        windowmark::Settings draft = coordinator.CurrentSettings();
        draft.pin.enabled = !draft.pin.enabled;
        // UpdateSettings releases every pinned window when this goes false - the switch
        // that could let them go is the one being turned off.
        coordinator.UpdateSettings(draft);
        control.SetPinState(draft.pin.enabled);
        persist();
    };
    handlers.onTogglePinWindow = [&](windowmark::WindowId id) { coordinator.TogglePin(id); };
    handlers.onGrabPreview = [&](windowmark::WindowId id) { coordinator.SetPinPreview(id); };
    handlers.onGrabCommit = [&](windowmark::WindowId id) {
        windowmark::win::PinDiag(L"GrabCommit: id=%llu 被跟踪=%d 已置顶=%d 置顶功能开=%d",
                                 static_cast<unsigned long long>(id),
                                 coordinator.IsTracked(id) ? 1 : 0,
                                 coordinator.IsPinned(id) ? 1 : 0,
                                 coordinator.CurrentSettings().pin.enabled ? 1 : 0);
        coordinator.TogglePin(id);
    };
    handlers.onGrabCancel = [&]() { coordinator.SetPinPreview(0); };
    handlers.onUnpinAll = [&]() { coordinator.UnpinAll(); };
    handlers.isPinnable = [&](windowmark::WindowId id) { return coordinator.IsTracked(id); };
    handlers.onPinSettings = [&]() {
        openSettingsPage(windowmark::win::SettingsPage::Pinning);
    };
    // The shortcut acts on whatever the user is looking at. This works where the tray
    // menu's equivalent did not: opening the menu makes WindowMark the foreground process,
    // so by the time the handler runs there is no user window left to read. A hotkey
    // leaves the foreground exactly where it was.
    handlers.onPinHotkey = [&]() {
        const HWND foreground = GetForegroundWindow();
        if (!foreground) return;
        // Walk to the root: pressed while a child control or an owned dialog has focus,
        // the shortcut should still pin the window the user thinks of as "this one".
        const HWND root = GetAncestor(foreground, GA_ROOT);
        const auto id =
            static_cast<windowmark::WindowId>(reinterpret_cast<std::uintptr_t>(root));
        wchar_t cls[64]{};
        GetClassNameW(root, cls, static_cast<int>(std::size(cls)));
        const bool tracked = coordinator.IsTracked(id);
        windowmark::win::PinDiag(L"快捷键触发: %s id=%llu 可置顶=%d", cls,
                static_cast<unsigned long long>(id), tracked ? 1 : 0);
        if (!tracked) return;
        coordinator.TogglePin(id);
    };
    // 「置顶刚才那个窗口」。面板上那个按钮用；没有菜单项——菜单做不到这件事（菜单弹出时
    // 前台窗口已经变成我们自己，这正是准星和全局快捷键存在的理由），而面板是从托盘点出来的，
    // 托盘和面板都不在跟踪列表里，所以 LastTrackedActiveWindow 正是用户打开面板之前在用的
    // 那个窗口。
    handlers.onPinLastWindow = [&]() {
        if (!coordinator.CurrentSettings().pin.enabled) {
            control.ShowBalloon(L"窗口置顶是关着的",
                                L"托盘菜单 →「窗口置顶」→「启用」打开之后，左键单击此图标就能"
                                L"把当前窗口置顶。");
            return;
        }
        const windowmark::WindowId id = coordinator.ActiveWindow();
        const bool tracked = id != 0 && coordinator.IsTracked(id);
        windowmark::win::PinDiag(L"托盘左键: id=%llu 可置顶=%d",
                                 static_cast<unsigned long long>(id), tracked ? 1 : 0);
        if (!tracked) {
            // 刚启动还没有过活动窗口，或者那个窗口在排除名单里、已经关掉了。说一句，
            // 别让它看起来像点坏了。
            control.ShowBalloon(L"没有可置顶的窗口",
                                L"先点一下要置顶的窗口，再左键单击此图标。被排除的应用和"
                                L"最小化的窗口不参与。");
            return;
        }
        const bool wasPinned = coordinator.IsPinned(id);
        coordinator.TogglePin(id);
        // 成功不弹气泡：置顶高亮本身就是回执，取消时它消失也是。只有「以为点了却没反应」
        // 才需要说话。
        if (coordinator.IsPinned(id) == wasPinned) {
            control.ShowBalloon(L"置顶没能生效",
                                L"这个窗口拒绝了置顶设置。换用托盘菜单里的准星再试一次。");
        }
    };
    // 主面板：托盘左键单击，或菜单里的「面板...」。
    //
    // 托盘菜单对会用的人够快，对第一次见它的人不行：四个功能叫什么、干什么、怎么用、现在开着
    // 没开，全藏在三层子菜单里，而菜单看不下任何一句说明。面板把这些摊开，顺便做所有设置的
    // 入口。
    //
    // 面板自己不干活：要开窗口的动作一律先关面板、再把对应的托盘命令投递回去执行（面板和菜单
    // 因此走同一条分发路径）；开关类当场同步执行，面板留着并刷新自己的勾。
    windowmark::win::HomePanelContext homeContext;
    homeContext.settings = [&]() { return coordinator.CurrentSettings(); };
    homeContext.toggleFeature = [&](windowmark::win::HomeFeature feature) {
        using F = windowmark::win::HomeFeature;
        switch (feature) {
        case F::Bookmarks:
            control.RunCommandNow(windowmark::win::WinControlWindow::kToggleCommand);
            break;
        case F::Borders:
            control.RunCommandNow(windowmark::win::WinControlWindow::kToggleBordersCommand);
            break;
        case F::Pinning:
            control.RunCommandNow(windowmark::win::WinControlWindow::kTogglePinningCommand);
            break;
        case F::Drag:
            control.RunCommandNow(windowmark::win::WinControlWindow::kToggleDragCommand);
            break;
        }
    };
    homeContext.autoStartEnabled = []() { return windowmark::app::IsAutoStartEnabled(); };
    homeContext.toggleAutoStart = [&]() {
        control.RunCommandNow(windowmark::win::WinControlWindow::kAutoStartCommand);
    };
    homeContext.lastWindowTitle = [&]() {
        const auto id = coordinator.ActiveWindow();
        if (id == 0) return std::wstring{};
        return windowmark::win::Utf8ToWide(coordinator.PinnedTitle(id));
    };
    homeContext.lastWindowPinned = [&]() {
        const auto id = coordinator.ActiveWindow();
        return id != 0 && coordinator.IsPinned(id);
    };

    handlers.onShowPanel = [&]() {
        using Action = windowmark::win::HomeAction;
        using Cmd = windowmark::win::WinControlWindow;
        if (dialogOpen) return;
        homeContext.runningFromInstallDir = windowmark::setup::RunningFromInstallDir();
        dialogOpen = true;
        const Action action =
            windowmark::win::WinHomePanel::ShowModal(control.NativeHandle(), homeContext);
        // 面板已经关了，守卫放开——接下来那个窗口自己去拿。
        dialogOpen = false;

        switch (action) {
        case Action::None:             break;
        case Action::BookmarkSettings: control.RunCommand(Cmd::kSettingsCommand); break;
        case Action::BookmarkApps:     control.RunCommand(Cmd::kSelectionCommand); break;
        case Action::BorderSettings:   control.RunCommand(Cmd::kBorderSettingsCommand); break;
        case Action::BorderApps:       control.RunCommand(Cmd::kBorderExcludeCommand); break;
        case Action::PinSettings:      control.RunCommand(Cmd::kPinSettingsCommand); break;
        case Action::PinGrab:          control.RunCommand(Cmd::kGrabToPinCommand); break;
        case Action::DragSettings:     control.RunCommand(Cmd::kDragSettingsCommand); break;
        case Action::DragApps:         control.RunCommand(Cmd::kDragExcludeCommand); break;
        case Action::PinLastWindow:    control.RunCommand(Cmd::kPinLastWindowCommand); break;
        case Action::ConfigPath:       control.RunCommand(Cmd::kConfigPathCommand); break;
        case Action::DesktopShortcut:  control.RunCommand(Cmd::kDesktopShortcutCommand); break;
        case Action::ClipKeeper:       control.RunCommand(Cmd::kClipKeeperCommand); break;
        case Action::Diagnose:         control.RunCommand(Cmd::kDiagnoseCommand); break;
        case Action::Install:          control.RunCommand(Cmd::kInstallCommand); break;
        case Action::Uninstall:        control.RunCommand(Cmd::kUninstallCommand); break;
        case Action::About:            control.RunCommand(Cmd::kAboutCommand); break;
        case Action::Exit:             control.RunCommand(Cmd::kExitCommand); break;
        }
    };
    // 诊断报告：出问题时点这里，它把报告存到桌面并复制到剪贴板，直接粘给开发者。
    // exe 和 ClipKeeper 一样在 tools\ 里。
    handlers.onDiagnose = [&]() {
        const auto exe =
            windowmark::win::InstalledExePath().parent_path() / L"tools" / L"WindowMarkDiag.exe";
        std::error_code ec;
        if (!std::filesystem::exists(exe, ec)) {
            control.ShowBalloon(L"找不到诊断工具",
                                L"它应当在这里：" + exe.wstring() +
                                    L"\n解压发布包时把 tools 文件夹一起解压出来即可。");
            return;
        }
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        const std::wstring file = exe.wstring();
        const std::wstring dir = exe.parent_path().wstring();
        info.lpFile = file.c_str();
        info.lpDirectory = dir.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            control.ShowBalloon(L"诊断工具没能启动", L"可以手动双击 " + file);
        }
    };
    handlers.onClipKeeper = [&]() {
        namespace ck = windowmark::clipkeeper;
        const HWND panel = FindWindowW(ck::kWindowClass, nullptr);

        if (!panel) {
            // 没在跑：启动它，它自带面板。附属 exe 都在 WindowMark.exe 旁边的 tools\ 里——
            // 顶层只留一个 exe，用户打开文件夹不用猜该双击哪个。
            const auto exe = windowmark::win::InstalledExePath().parent_path() / L"tools" /
                             ck::kExeName;
            std::error_code ec;
            if (!std::filesystem::exists(exe, ec)) {
                MessageBoxW(nullptr,
                            (L"找不到 ClipKeeper.exe。\n\n它应当在这里：\n" + exe.wstring() +
                             L"\n\n解压发布包时如果只取了 WindowMark.exe，把 tools 文件夹一起"
                             L"解压出来即可。")
                                .c_str(),
                            L"WindowMark", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                return;
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::wstring command = exe.wstring();
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                MessageBoxW(control.NativeHandle(), L"启动 ClipKeeper 失败。", L"WindowMark",
                            MB_OK | MB_ICONWARNING);
            }
            return;
        }

        if (IsWindowVisible(panel)) {
            // 收起面板。WM_CLOSE 是 ClipKeeper 的既有行为：隐藏窗口，留在托盘里继续守护。
            PostMessageW(panel, WM_CLOSE, 0, 0);
            return;
        }

        // 在托盘里：叫出来。
        if (const UINT showPanel = RegisterWindowMessageW(ck::kShowPanelMessage)) {
            PostMessageW(panel, showPanel, 0, 0);
        }
    };

    handlers.onDesktopShortcut = [&]() {
        // owner 传 nullptr 的理由同首次运行那个框：控制窗口是 0x0 的隐藏窗口，拿它当 owner
        // 会把框摆到屏幕左上角。
        std::filesystem::path link;
        if (windowmark::win::CreateDesktopShortcut(link)) {
            MessageBoxW(nullptr, (L"已在桌面创建快捷方式：\n\n" + link.wstring()).c_str(),
                        L"WindowMark", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        } else {
            MessageBoxW(nullptr, L"创建桌面快捷方式失败。桌面目录可能不可写。", L"WindowMark",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
    };
    // 安装 / 卸载：同一个 exe 的两个模式。两个处理函数返回 true 表示「这份该退场了」——
    // 安装完要把位置让给安装目录里的那份，卸载则已经在临时副本里开工，它马上会来停我们。
    handlers.onInstall = [&]() {
        exclusive([&] {
            if (windowmark::setup::InstallFromTray()) PostQuitMessage(0);
        });
    };
    handlers.onUninstall = [&]() {
        exclusive([&] {
            if (windowmark::setup::UninstallFromTray()) PostQuitMessage(0);
        });
    };
    handlers.onConfigPath = [&]() {
        exclusive([&] {
            const auto before = windowmark::win::CurrentConfigLocation();
            std::filesystem::path chosen;
            if (!windowmark::win::WinConfigPathDialog::ShowModal(
                    control.NativeHandle(), before.path, before.source, chosen)) {
                return;
            }
            if (chosen == before.path) return;

            // 先把现有配置搬过去，否则用户改完设置会发现配置「没了」。搬不过去就整个
            // 放弃，别留下一个指向空位置的注册表值。
            if (!windowmark::Settings::Save(chosen, coordinator.CurrentSettings())) {
                MessageBoxW(control.NativeHandle(), L"写入新位置失败，配置位置未改变。",
                            L"WindowMark", MB_OK | MB_ICONWARNING);
                return;
            }

            const auto portable = windowmark::win::PortableConfigPath();
            const bool choosingPortable = !portable.empty() && chosen == portable;

            // 从便携切走时，exe 旁边那份必须改名。它在查找顺序里排第一，留着的话下次
            // 启动又会把它选回来，用户改的设置等于没生效。改名而不是删除：那是用户的
            // 数据，不是我们的。
            if (before.source == windowmark::ConfigSource::Portable && !choosingPortable) {
                std::error_code ec;
                std::filesystem::rename(portable, portable.wstring() + L".disabled", ec);
            }

            // 注册表只在选「自定义」时才写。选默认位置或程序目录都要清掉它，否则下次
            // 启动第 2 层会把旧值又捡回来。
            const auto fallbackRoot = windowmark::win::LocalDataRoot();
            const auto fallback = fallbackRoot.empty() ? std::filesystem::path{}
                                                       : fallbackRoot / L"settings.conf";
            const bool custom = !choosingPortable && chosen != fallback;
            windowmark::win::WriteConfiguredConfigPath(custom ? chosen
                                                              : std::filesystem::path{});

            MessageBoxW(control.NativeHandle(),
                        L"配置位置已更改，重启 WindowMark 后生效。", L"WindowMark",
                        MB_OK | MB_ICONINFORMATION);
        });
    };

    handlers.onAbout = [&]() {
        // A TaskDialog does not even disable its owner, so this one also remembers its own
        // HWND and raises the existing box rather than just swallowing the second click.
        if (aboutDialog && IsWindow(aboutDialog)) {
            SetForegroundWindow(aboutDialog);
            return;
        }
        if (dialogOpen) return;
        dialogOpen = true;
        // TaskDialog rather than MessageBox: MessageBox only takes the stock system icons,
        // so the about box was showing the generic blue "i" instead of the app's own.
        const std::wstring content =
            std::wstring(L"两个独立的窗口增强功能，合在一个托盘程序里：\n"
                         L"  • 窗口书签 — 同一应用的每个窗口共享一组书签，点击即可切换\n"
                         L"  • 窗口边框 — 为每个窗口描边，区分当前活动窗口\n\n"
                         L"作者：yekoye\n"
                         L"邮箱：yuxiang_163com@163.com\n\n"
                         L"程序位置：\n") +
            windowmark::win::InstalledExePath().wstring() + L"\n\n" +
            L"配置文件：\n" + settingsPath.wstring() + L"\n\n" +
            L"以普通用户进程运行，不注入 DLL、不修改 Explorer 或任务栏、\n"
            L"不安装服务、驱动或系统级注册表项。";
        // Version and build stamp together: the version says which release this is, the
        // stamp says which build, and only the stamp is impossible to forget to update.
        const std::wstring instruction =
            std::wstring(L"WindowMark ") + windowmark::app::kProductVersion +
            L"   （构建于 " + windowmark::app::kBuildStamp + L"）";

        // TaskDialog draws the main icon at 32px, so ask the .ico for that size rather
        // than letting it shrink the 256.
        HICON icon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));

        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = control.NativeHandle();
        config.hInstance = GetModuleHandleW(nullptr);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.dwCommonButtons = TDCBF_OK_BUTTON;
        config.pszWindowTitle = L"关于 WindowMark";
        if (icon) {
            // hMainIcon shares a union with pszMainIcon; without the flag the handle would
            // be read as a resource id.
            config.dwFlags |= TDF_USE_HICON_MAIN;
            config.hMainIcon = icon;
        } else {
            config.pszMainIcon = TD_INFORMATION_ICON;
        }
        // These must outlive the call: pointing the dialog at a temporary's c_str() leaves
        // it reading freed memory, which is what garbled the installer's title once.
        config.pszMainInstruction = instruction.c_str();
        config.pszContent = content.c_str();
        // TDN_CREATED is the only place the dialog's own HWND is handed out, and it is
        // what the duplicate check above needs.
        config.pfCallback = [](HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
            if (msg == TDN_CREATED) {
                *reinterpret_cast<HWND*>(data) = hwnd;
                // Same reason as the settings window: a pinned window sits in the topmost
                // band and nothing below it can be raised above it. TaskDialog has no flag
                // for this, so it is done here, the one place its HWND is available.
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return S_OK;
        };
        config.lpCallbackData = reinterpret_cast<LONG_PTR>(&aboutDialog);

        TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
        aboutDialog = nullptr;
        dialogOpen = false;
        if (icon) DestroyIcon(icon);
    };
    handlers.onExit = []() { PostQuitMessage(0); };

    if (!control.Start(std::move(handlers))) {
        coordinator.Stop();
        LogAutoStartPhase(autoStartLaunch, L"tray_failed", 6);
        MessageBoxW(nullptr, L"托盘控制器初始化失败，程序已安全退出。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 6;
    }
    // 第一次运行：绿色版双击就跑，没有任何安装向导说过话。这是唯一能交代「图标在托盘里」
    // 并且顺手把开机启动和桌面图标办了的机会——那两件事安装程序会替用户做，绿色版没人做，
    // 藏在托盘右键里等于默认没有。所以这里直接问一次，答完就再也不问。
    //
    // 用 MessageBox 而不是托盘气泡：气泡会被专注助手、通知设置静默吞掉，而这是双击之后用户
    // 正等着看反应的一刻，必须保证送到。开机自启动那次不问——那时用户没在等它说话。
    if (firstRun && !autoStartLaunch) {
        // 不传 owner：托盘控制窗口是 0x0、钉在 (0,0) 的隐藏窗口，MessageBox 会居中到它身上，
        // 结果整个框挤在屏幕左上角。无主的框居中在主屏，MB_SETFOREGROUND 保证它抢到前台
        // （进程刚被用户双击起来，这时系统允许）。
        const int answer = MessageBoxW(
            nullptr,
            L"WindowMark 已经在运行，图标在任务栏右下角的托盘里（可能折在「^」里面）。\n"
            L"同一个程序开两个窗口，就能看到窗口书签。\n\n"
            L"现在顺手设好开机自动启动，并在桌面放一个图标吗？\n"
            L"以后随时能在托盘右键菜单里改。",
            L"WindowMark", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (answer == IDYES) {
            std::wstring failed;
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe))) == 0 ||
                !windowmark::app::SetAutoStart(exe, true)) {
                failed += L"\n·开机自动启动（注册表写不进去）";
            }
            std::filesystem::path link;
            if (!windowmark::win::CreateDesktopShortcut(link)) {
                failed += L"\n·桌面快捷方式（桌面目录写不进去）";
            }
            // 成功不再弹第二个框：桌面上多出来的图标就是回执。只有失败才需要说话，
            // 否则用户会以为设好了，下次开机发现没启动，无从查起。
            if (!failed.empty()) {
                MessageBoxW(nullptr,
                            (L"下面这些没设成，可以稍后在托盘右键菜单里重试：" + failed).c_str(),
                            L"WindowMark", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            }
        }
    }
    // 决定菜单里是「安装到系统...」还是「卸载 WindowMark...」。
    control.SetRunningFromInstallDir(windowmark::setup::RunningFromInstallDir());
    // 书签这一项以前漏了：托盘里它的初值写死是「开」，配置里关着也照样打勾。
    control.SetEnabledState(coordinator.CurrentSettings().drawer.enabled);
    control.SetBorderState(coordinator.CurrentSettings().border.enabled);
    control.SetDragState(coordinator.CurrentSettings().drag.enabled);
    control.SetPinState(coordinator.CurrentSettings().pin.enabled);

    // Applies the shortcut from settings, and says so out loud when Windows refuses it.
    // Refusal always means another process claimed the combination first; the alternative
    // to telling the user is a shortcut that quietly does nothing, which is
    // indistinguishable from a broken feature.
    const auto applyHotkey = [&](bool announceFailure) {
        const windowmark::Hotkey hotkey =
            windowmark::ParseHotkey(coordinator.CurrentSettings().pin.hotkey);
        if (control.SetPinHotkey(hotkey)) return;
        if (!announceFailure) return;
        const std::wstring text =
            L"快捷键 " + windowmark::FormatHotkeyWide(hotkey) +
            L" 已被其他程序占用，注册失败。\n\n"
            L"Windows 的全局快捷键先到先得，先注册的程序会一直占着它。"
            L"请换一个组合。";
        MessageBoxW(control.NativeHandle(), text.c_str(), L"WindowMark",
                    MB_OK | MB_ICONWARNING);
    };
    // Silent at startup: a message box before the tray icon is even up would be the first
    // thing seen after logging in, about a shortcut set long ago. The diag log still
    // records it, and changing it in the dialog does report failure immediately.
    applyHotkey(false);
    reapplyHotkey = [&]() { applyHotkey(true); };
    LogAutoStartPhase(autoStartLaunch, L"running");

    control.SetPinnedProvider([&]() {
        std::vector<std::pair<windowmark::WindowId, std::wstring>> out;
        for (const auto& record : coordinator.PinnedWindows()) {
            std::wstring title = windowmark::win::Utf8ToWide(coordinator.PinnedTitle(record.windowId));
            // A window can vanish between being pinned and the menu being opened; keep the
            // entry so the user can still release it, just without a name to show.
            if (title.empty()) title = L"(已关闭的窗口)";
            if (title.size() > 40) title = title.substr(0, 39) + L"…";
            out.emplace_back(record.windowId, std::move(title));
        }
        return out;
    });

    // 钩子在这里第一次装上。放在消息循环之前、控制窗口起来之后：钩子回调要靠这个
    // 线程的消息泵驱动，装早了没人处理。
    applyDrag();

    // 看门狗：每 5 秒让 Coordinator 对一次账（系统上真实存在的窗口 vs 自己记着的那份），
    // 顺带让边框检查自己有没有卡在挂起里。防的是那类从外面看一模一样的故障——窗口变了、
    // 程序毫无反应、重启才好。
    //
    // 定时器挂在 nullptr 上而不是某个窗口：这件事不属于任何一个窗口，WM_TIMER 直接回到
    // 这个循环里，分发前就处理掉（hwnd 为空的 WM_TIMER 不会被 DispatchMessage 送给谁）。
    //
    // WM_TIMER 是最低优先级的消息，只在队列空下来时才生成——所以拖动窗口那种每秒上百条
    // 事件的时候它会被推后（实测 95 秒里巡了 12 次而不是 19 次）。对看门狗来说这正合适：
    // 它永远不会和真正要干的活抢时间，而「程序忙」本身就说明事件流是通的。
    constexpr UINT_PTR kWatchdogTimer = 0x57415443;   // 'WATC'
    constexpr UINT kWatchdogIntervalMs = 5000;
    const UINT_PTR watchdog = SetTimer(nullptr, kWatchdogTimer, kWatchdogIntervalMs, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr && msg.wParam == watchdog) {
            coordinator.WatchdogTick();
            // 每分钟一条心跳（只在诊断打开时写）。没有它就分不清「看门狗跑着而且一切正常」
            // 和「看门狗自己也停了」——而后者恰恰是要防的那类故障的一种。
            static unsigned ticks = 0;
            if (++ticks % 12 == 0) {
                windowmark::win::PinDiag(L"看门狗：已巡 %u 次，恢复过 %u 次", ticks,
                                         coordinator.WatchdogRecoveries());
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (watchdog) KillTimer(nullptr, watchdog);

    // Explicit shutdown order: UI first, then overlay/preview/hooks.
    // Even if the process is force-terminated, all owned HWNDs disappear with the process.
    dragBackend.Shutdown();
    control.Stop();
    coordinator.Stop();
    return static_cast<int>(msg.wParam);
}
