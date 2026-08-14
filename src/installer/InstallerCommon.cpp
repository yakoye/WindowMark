#include "InstallerCommon.h"

#include "AppIdentity.h"
#include "AutoStart.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <system_error>

namespace windowmark::setup {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)) || !value) {
        return {};
    }
    std::filesystem::path result(value);
    CoTaskMemFree(value);
    return result;
}

} // namespace

// --- process control -------------------------------------------------------

std::vector<RunningInstance> FindRunningInstances() {
    std::vector<RunningInstance> found;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) continue;
            if (_wcsicmp(entry.szExeFile, app::kMainExeName) != 0) continue;

            RunningInstance instance;
            instance.processId = entry.th32ProcessID;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process) {
                wchar_t buffer[MAX_PATH * 4]{};
                DWORD size = static_cast<DWORD>(std::size(buffer));
                if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
                    instance.imagePath = std::filesystem::path(buffer);
                }
                CloseHandle(process);
            }
            found.push_back(std::move(instance));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

namespace {

BOOL CALLBACK PostQuitToControlWindow(HWND hwnd, LPARAM lParam) {
    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) == 0) return TRUE;
    if (_wcsicmp(className, app::kControlWindowClass) != 0) return TRUE;
    PostMessageW(hwnd, static_cast<UINT>(lParam), 0, 0);
    return TRUE;
}

} // namespace

bool StopRunningInstances(unsigned graceMs) {
    auto instances = FindRunningInstances();
    if (instances.empty()) return true;

    // Step 1: ask politely, addressing the tray control window directly instead of
    // broadcasting. It turns this into a normal PostQuitMessage, so the tray icon,
    // hooks and overlays are torn down in the usual order.
    if (const UINT quitMessage = RegisterWindowMessageW(app::kRequestQuitMessage)) {
        EnumWindows(PostQuitToControlWindow, static_cast<LPARAM>(quitMessage));
    }

    const ULONGLONG deadline = GetTickCount64() + graceMs;
    bool allGone = true;

    for (const auto& instance : instances) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, instance.processId);
        if (!process) {
            // Already gone, or not ours to touch. Either way there is nothing to report.
            continue;
        }

        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);

        if (WaitForSingleObject(process, remaining) != WAIT_OBJECT_0) {
            // Step 2: it did not go on its own. Terminating is safe for this process.
            TerminateProcess(process, 0);
            if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) {
                allGone = false;
            }
        }
        CloseHandle(process);
    }

    // Waiting on the process handle is not enough on its own: if OpenProcess failed we
    // skipped the wait entirely, and a predecessor that is mid-exit still owns the
    // singleton mutex. A new instance that sees that mutex concludes it is a second
    // launch and exits silently - which looks exactly like "the installer did not start
    // the app". So block until the singleton is genuinely free.
    allGone = WaitForSingletonRelease(4000) && allGone;

    // Give the shell a moment to reclaim the tray slot before files are replaced.
    Sleep(120);
    return allGone;
}

bool WaitForSingletonRelease(unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        // Acquire-and-release is the only reliable probe: OpenMutex can fail for reasons
        // unrelated to whether anyone holds it.
        HANDLE mutex = CreateMutexW(nullptr, TRUE, app::kSingletonMutex);
        if (mutex) {
            const bool free = GetLastError() != ERROR_ALREADY_EXISTS;
            if (free) ReleaseMutex(mutex);
            // Closing drops our reference so the instance we are about to start can take
            // the mutex itself.
            CloseHandle(mutex);
            if (free) return true;
        }
        if (GetTickCount64() >= deadline) return false;
        Sleep(50);
    }
}

// --- locations -------------------------------------------------------------

std::filesystem::path SelfPath() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return std::filesystem::path(buffer);
}

std::filesystem::path SelfDir() {
    const auto self = SelfPath();
    return self.empty() ? std::filesystem::path{} : self.parent_path();
}

std::filesystem::path InstallDir() {
    const auto base = KnownFolder(FOLDERID_LocalAppData);
    return base.empty() ? std::filesystem::path{} : base / app::kInstallSubdir;
}

std::filesystem::path LocalDataDir() {
    const auto base = KnownFolder(FOLDERID_LocalAppData);
    return base.empty() ? std::filesystem::path{} : base / app::kDataSubdir;
}

std::filesystem::path RoamingDataDir() {
    const auto base = KnownFolder(FOLDERID_RoamingAppData);
    return base.empty() ? std::filesystem::path{} : base / app::kDataSubdir;
}

std::filesystem::path StartMenuShortcutPath() {
    const auto base = KnownFolder(FOLDERID_Programs);
    return base.empty() ? std::filesystem::path{} : base / app::kShortcutName;
}

std::filesystem::path LocatePayload(const wchar_t* fileName) {
    const auto here = SelfDir();
    if (here.empty()) return {};

    const std::filesystem::path candidates[] = {
        here / fileName,
        here / L"build" / L"Release" / fileName,
        here / L"build" / fileName,
        here.parent_path() / L"build" / L"Release" / fileName,
        here.parent_path() / fileName,
    };

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::weakly_canonical(candidate, ec);
        }
    }
    return {};
}

// --- registry / shell integration -----------------------------------------

// Both of these live in AutoStart.h now, shared with the app so the settings dialog and
// the installer cannot drift apart on what "start with Windows" means.
bool SetStartWithWindows(const std::filesystem::path& exePath, bool enable) {
    if (enable) app::ClearAutoStartVeto();
    return app::SetAutoStart(exePath.wstring(), enable);
}

bool IsStartWithWindowsEnabled() { return app::IsAutoStartEnabled(); }

namespace {

bool SetRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetRegDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

} // namespace

bool WriteUninstallEntry(const std::filesystem::path& installDir, unsigned long sizeKb) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, app::kUninstallKeyPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring mainExe = (installDir / app::kMainExeName).wstring();
    const std::wstring uninstallExe = (installDir / app::kUninstallExeName).wstring();

    bool ok = true;
    ok &= SetRegString(key, L"DisplayName", app::kProductName);
    ok &= SetRegString(key, L"DisplayVersion", app::kProductVersion);
    ok &= SetRegString(key, L"Publisher", app::kPublisher);
    ok &= SetRegString(key, L"DisplayIcon", mainExe);
    ok &= SetRegString(key, L"InstallLocation", installDir.wstring());
    ok &= SetRegString(key, L"UninstallString", L"\"" + uninstallExe + L"\"");
    ok &= SetRegString(key, L"QuietUninstallString", L"\"" + uninstallExe + L"\" /S");
    ok &= SetRegDword(key, L"NoModify", 1);
    ok &= SetRegDword(key, L"NoRepair", 1);
    ok &= SetRegDword(key, L"EstimatedSize", sizeKb);

    RegCloseKey(key);
    return ok;
}

void RemoveUninstallEntry() {
    RegDeleteKeyExW(HKEY_CURRENT_USER, app::kUninstallKeyPath, KEY_WOW64_64KEY, 0);
}

bool CreateStartMenuShortcut(const std::filesystem::path& target) {
    const auto linkPath = StartMenuShortcutPath();
    if (linkPath.empty()) return false;

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link)))) {
        return false;
    }

    bool ok = false;
    link->SetPath(target.wstring().c_str());
    link->SetWorkingDirectory(target.parent_path().wstring().c_str());
    link->SetDescription(L"WindowMark - 同应用多窗口书签层");
    link->SetIconLocation(target.wstring().c_str(), 0);

    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(linkPath.wstring().c_str(), TRUE));
        file->Release();
    }

    link->Release();
    return ok;
}

void RemoveStartMenuShortcut() {
    const auto linkPath = StartMenuShortcutPath();
    if (linkPath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(linkPath, ec);
}

// --- filesystem helpers ----------------------------------------------------

bool EnsureDirectory(const std::filesystem::path& dir, std::wstring& error) {
    if (dir.empty()) {
        error = L"目标目录路径为空。";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec && !std::filesystem::is_directory(dir, ec)) {
        error = L"无法创建目录：" + dir.wstring();
        return false;
    }
    return true;
}

bool CopyFileTo(const std::filesystem::path& from, const std::filesystem::path& to, std::wstring& error) {
    if (CopyFileW(from.wstring().c_str(), to.wstring().c_str(), FALSE)) {
        return true;
    }

    const DWORD code = GetLastError();
    error = L"复制失败：" + from.filename().wstring() + L"\n" + DescribeLastError(code);
    return false;
}

bool RemoveTree(const std::filesystem::path& dir, std::wstring& error) {
    if (dir.empty()) return true;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return true;

    std::filesystem::remove_all(dir, ec);
    if (ec) {
        error = L"无法删除：" + dir.wstring();
        return false;
    }
    return true;
}

unsigned long DirectorySizeKb(const std::filesystem::path& dir) {
    std::error_code ec;
    unsigned long long bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        std::error_code inner;
        if (entry.is_regular_file(inner)) {
            bytes += entry.file_size(inner);
        }
    }
    return static_cast<unsigned long>(bytes / 1024ULL);
}

void ScheduleSelfDelete(const std::filesystem::path& path) {
    if (path.empty()) return;

    // A detached cmd waits for this process to release the image, then deletes it.
    std::wstring command = L"/c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + path.wstring() + L"\"";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // NO_UI: this runs during silent uninstalls too, and a ShellExecuteEx error dialog
    // would block with nobody there to dismiss it.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = L"cmd.exe";
    info.lpParameters = command.c_str();
    info.nShow = SW_HIDE;
    if (ShellExecuteExW(&info) && info.hProcess) {
        CloseHandle(info.hProcess);
    }
}

// --- command line ----------------------------------------------------------

namespace {

class Arguments {
public:
    Arguments() {
        int count = 0;
        LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw) return;
        for (int i = 1; i < count; ++i) {
            values.emplace_back(raw[i]);
        }
        LocalFree(raw);
    }

    std::vector<std::wstring> values;
};

const Arguments& Args() {
    static const Arguments args;
    return args;
}

} // namespace

bool HasSwitch(const wchar_t* name) {
    const auto& values = Args().values;
    return std::any_of(values.begin(), values.end(), [name](const std::wstring& value) {
        return _wcsicmp(value.c_str(), name) == 0;
    });
}

std::wstring SwitchValue(const wchar_t* name) {
    const auto& values = Args().values;
    for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        if (_wcsicmp(values[i].c_str(), name) == 0) {
            return values[i + 1];
        }
    }
    return {};
}

std::wstring DescribeLastError(DWORD code) {
    LPWSTR text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&text), 0, nullptr);

    std::wstring result;
    if (length && text) {
        result.assign(text, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    }
    if (text) LocalFree(text);
    if (result.empty()) {
        result = L"系统错误代码 " + std::to_wstring(code) + L"。";
    }
    return result;
}

} // namespace windowmark::setup
