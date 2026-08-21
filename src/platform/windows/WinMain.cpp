#include "WinBorderBackend.h"
#include "WinControlWindow.h"
#include "WinOverlayBackend.h"
#include "WinPinBackend.h"
#include "WinPreviewBackend.h"
#include "WinRenameDialog.h"
#include "WinSelectionDialog.h"
#include "WinSettingsDialog.h"
#include "WinUtil.h"
#include "WinWindowBackend.h"

#include "windowmark/core/Coordinator.h"
#include "windowmark/core/Settings.h"

#include "AppIdentity.h"
#include "BuildStamp.h"
#include "Resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScopedCom com;
    if (!com.ok()) {
        MessageBoxW(nullptr, L"COM 初始化失败。WindowMark 未启动。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 2;
    }

    const bool purgeRequested = HasArgument(L"--purge");
    ScopedHandle singleInstance(CreateMutexW(nullptr, FALSE, windowmark::app::kSingletonMutex));
    if (!singleInstance.get()) return 3;

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
        return 0;
    }

    if (purgeRequested) {
        windowmark::win::PurgeAllUserData();
        MessageBoxW(nullptr,
                    L"WindowMark 的配置、缓存和开机自启项已清理。\n"
                    L"如果程序本体已安装，请再运行安装目录下的 WindowMarkUninstall.exe 删除程序文件。",
                    L"WindowMark - 完全清理",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const std::filesystem::path dataRoot = windowmark::win::LocalDataRoot();
    if (dataRoot.empty()) {
        MessageBoxW(nullptr, L"无法确定本地配置目录。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 4;
    }
    const auto settingsPath = dataRoot / L"settings.conf";
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

    const auto persist = [&]() {
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

    const auto openSettingsPage = [&](windowmark::win::SettingsPage page) {
        exclusive([&] {
            windowmark::Settings draft = coordinator.CurrentSettings();
            if (!windowmark::win::WinSettingsDialog::ShowModal(control.NativeHandle(), draft, page)) {
                return;
            }
            coordinator.UpdateSettings(draft);
            control.SetBorderState(coordinator.CurrentSettings().border.enabled);
            control.SetPinState(coordinator.CurrentSettings().pin.enabled);
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
        const bool anythingOn = draft.drawer.enabled || draft.border.enabled;
        draft.drawer.enabled = !anythingOn;
        draft.border.enabled = !anythingOn;
        coordinator.UpdateSettings(draft);
        control.SetEnabledState(draft.drawer.enabled);
        control.SetBorderState(draft.border.enabled);
        persist();
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
    handlers.onPinForeground = [&]() {
        if (const auto id = coordinator.ActiveWindow(); id != 0) coordinator.TogglePin(id);
    };
    handlers.onUnpinAll = [&]() { coordinator.UnpinAll(); };
    handlers.onPinSettings = [&]() {
        openSettingsPage(windowmark::win::SettingsPage::Pinning);
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
                         L"  • 书签 — 同一应用的每个窗口共享一组书签，点击即可切换\n"
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
            if (msg == TDN_CREATED) *reinterpret_cast<HWND*>(data) = hwnd;
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
        MessageBoxW(nullptr, L"托盘控制器初始化失败，程序已安全退出。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 6;
    }
    control.SetBorderState(coordinator.CurrentSettings().border.enabled);
    control.SetPinState(coordinator.CurrentSettings().pin.enabled);
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

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Explicit shutdown order: UI first, then overlay/preview/hooks.
    // Even if the process is force-terminated, all owned HWNDs disappear with the process.
    control.Stop();
    coordinator.Stop();
    return static_cast<int>(msg.wParam);
}
