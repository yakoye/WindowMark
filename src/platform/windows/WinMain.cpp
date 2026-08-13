#include "WinControlWindow.h"
#include "WinOverlayBackend.h"
#include "WinPreviewBackend.h"
#include "WinRenameDialog.h"
#include "WinSelectionDialog.h"
#include "WinSettingsDialog.h"
#include "WinUtil.h"
#include "WinWindowBackend.h"

#include "windowmark/core/Coordinator.h"
#include "windowmark/core/Settings.h"

#include "AppIdentity.h"

#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <iterator>
#include <memory>
#include <string>

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
    windowmark::Coordinator coordinator(settings, windowBackend, overlayBackend, previewBackend);

    // Declared before Start() so the context-menu handlers below can capture it; it is
    // only actually started further down, once the coordinator is running.
    windowmark::win::WinControlWindow control;

    const auto openSettings = [&]() {
        windowmark::Settings draft = coordinator.CurrentSettings();
        if (!windowmark::win::WinSettingsDialog::ShowModal(control.NativeHandle(), draft)) {
            return;
        }
        coordinator.UpdateSettings(draft);
        if (!windowmark::Settings::Save(settingsPath, coordinator.CurrentSettings())) {
            MessageBoxW(control.NativeHandle(),
                        L"设置已在本次运行中生效，但保存 settings.conf 失败，下次启动不会保留。",
                        L"WindowMark",
                        MB_OK | MB_ICONWARNING);
        }
    };

    coordinator.SetMenuHandlers(
        [&](windowmark::WindowId id) {
            std::wstring name = windowmark::win::Utf8ToWide(coordinator.CustomLabel(id));
            const std::wstring title = windowmark::win::Utf8ToWide(coordinator.DefaultLabel(id));
            if (windowmark::win::WinRenameDialog::ShowModal(control.NativeHandle(), title, name)) {
                coordinator.SetCustomLabel(id, windowmark::win::WideToUtf8(name));
            }
        },
        openSettings);

    if (!coordinator.Start()) {
        MessageBoxW(nullptr,
                    L"WindowMark 初始化失败。程序已安全退出，不会修改 Explorer、任务栏或系统驱动。",
                    L"WindowMark",
                    MB_OK | MB_ICONERROR);
        return 5;
    }

    if (!control.Start(
            [&]() {
                coordinator.SetOverlayEnabled(!coordinator.OverlayEnabled());
                control.SetEnabledState(coordinator.OverlayEnabled());
            },
            [&]() {
                auto selection = coordinator.SelectionSnapshot();
                if (windowmark::win::WinSelectionDialog::ShowModal(control.NativeHandle(), selection)) {
                    coordinator.ApplySelection(selection);
                    if (!windowmark::Settings::Save(settingsPath, coordinator.CurrentSettings())) {
                        MessageBoxW(control.NativeHandle(),
                                    L"选择已经在本次运行中生效，但保存 settings.conf 失败。应用级选择下次启动可能不会保留。",
                                    L"WindowMark",
                                    MB_OK | MB_ICONWARNING);
                    }
                }
            },
            openSettings,
            [&]() {
                const std::wstring content =
                    std::wstring(L"版本 ") + windowmark::app::kProductVersion + L"\n\n" +
                    L"同应用多窗口书签层。同一个应用的每个窗口都会得到同一组书签，\n"
                    L"点击任意书签即可切换到对应窗口。\n\n" +
                    L"程序位置：\n" + windowmark::win::InstalledExePath().wstring() + L"\n\n" +
                    L"配置文件：\n" + settingsPath.wstring() + L"\n\n" +
                    L"以普通用户进程运行，不注入 DLL、不修改 Explorer 或任务栏、\n"
                    L"不安装服务、驱动或系统级注册表项。";
                MessageBoxW(control.NativeHandle(), content.c_str(),
                            L"关于 WindowMark", MB_OK | MB_ICONINFORMATION);
            },
            []() { PostQuitMessage(0); })) {
        coordinator.Stop();
        MessageBoxW(nullptr, L"托盘控制器初始化失败，程序已安全退出。", L"WindowMark", MB_OK | MB_ICONERROR);
        return 6;
    }

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
