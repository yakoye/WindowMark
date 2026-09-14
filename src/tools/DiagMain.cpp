// WindowMarkDiag.exe —— 出了问题时让用户双击运行一次，把生成的报告贴回来。
//
// 报告里凡是「WindowMark 会怎么判断」的部分，调用的都是 WindowMark 自己的代码：配置
// 位置解析、窗口资格判断、桌面快照、边框规划。不另写一份规则——WindowMarkInspect 的
// 注释里记着教训：另写的规则会漂移，然后自信地报出一个错误答案。
//
// 只读：不改配置、不动窗口、不抢焦点。写的只有报告文件和剪贴板。
//
// 隐私：不收集窗口标题（常有文档名、网址、聊天对象），路径里的用户目录换成
// %USERPROFILE%。排查边框问题有类名和进程名就够了——报告是要贴到别人面前的。
//
//   双击                          报告存到桌面、复制到剪贴板，弹窗提示
//   WindowMarkDiag.exe --out 文件  只写到指定文件，不弹窗、不碰剪贴板（自动化验证用）

#include "AppIdentity.h"
#include "WinBorderPlan.h"
#include "WinDesktopSnapshot.h"
#include "WinUtil.h"

#include "windowmark/core/ConfigLocation.h"
#include "windowmark/core/Settings.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

namespace app = windowmark::app;
namespace win = windowmark::win;
using windowmark::BorderModel;
using windowmark::Settings;

// ---------------------------------------------------------------------------
// 报告

std::wstring g_body;
std::vector<std::wstring> g_findings;
std::wstring g_profile;   // USERPROFILE，打印路径前替换掉

void Line(const wchar_t* format, ...) {
    wchar_t buffer[4096]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, format, args);
    va_end(args);
    g_body += buffer;
    g_body += L"\r\n";
}

void Section(const wchar_t* title) {
    g_body += L"\r\n";
    Line(L"== %ls ==", title);
}

void Finding(const wchar_t* format, ...) {
    wchar_t buffer[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, format, args);
    va_end(args);
    g_findings.emplace_back(buffer);
}

// 把路径里的用户目录换成 %USERPROFILE%。大小写不敏感：进程路径常被系统转成小写。
[[nodiscard]] std::wstring Redact(std::wstring text) {
    if (g_profile.empty()) return text;
    const auto same = [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); };
    constexpr std::wstring_view kPlaceholder = L"%USERPROFILE%";
    for (size_t from = 0;;) {
        const auto it = std::search(text.begin() + static_cast<std::ptrdiff_t>(from), text.end(),
                                    g_profile.begin(), g_profile.end(), same);
        if (it == text.end()) break;
        const size_t at = static_cast<size_t>(it - text.begin());
        text.replace(at, g_profile.size(), kPlaceholder);
        from = at + kPlaceholder.size();
    }
    return text;
}

[[nodiscard]] std::wstring RectText(const RECT& r) {
    wchar_t buffer[96]{};
    swprintf_s(buffer, L"(%ld,%ld,%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom,
               r.right - r.left, r.bottom - r.top);
    return buffer;
}

// ---------------------------------------------------------------------------
// 零碎的查询

[[nodiscard]] std::wstring ClassOf(HWND hwnd) {
    if (hwnd == nullptr) return L"(无)";
    wchar_t name[256]{};
    GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return name;
}

[[nodiscard]] DWORD ProcessIdOf(HWND hwnd) {
    DWORD pid = 0;
    if (hwnd != nullptr) GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

[[nodiscard]] std::wstring ProcessNameOf(HWND hwnd) {
    const std::wstring path = win::QueryProcessPath(ProcessIdOf(hwnd));
    if (path.empty()) return L"?";
    return std::filesystem::path(path).filename().wstring();
}

[[nodiscard]] std::wstring RegText(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

[[nodiscard]] DWORD RegNumber(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD data = 0;
    DWORD size = sizeof(data);
    RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    return data;
}

// 文件上的「来自网络」标记（Zone.Identifier 备用数据流）。-1 表示没有这个标记。
[[nodiscard]] int ZoneIdOf(const std::wstring& path) {
    HANDLE file = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return -1;
    char buffer[1024]{};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    const char* id = std::strstr(buffer, "ZoneId=");
    return id != nullptr ? std::atoi(id + 7) : 0;
}

[[nodiscard]] std::wstring FileVersionOf(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return L"?";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"?";
    LPVOID raw = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", &raw, &length) || raw == nullptr) return L"?";
    const auto* info = static_cast<VS_FIXEDFILEINFO*>(raw);
    wchar_t text[64]{};
    swprintf_s(text, L"%u.%u.%u", HIWORD(info->dwProductVersionMS),
               LOWORD(info->dwProductVersionMS), HIWORD(info->dwProductVersionLS));
    return text;
}

[[nodiscard]] bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD length = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &length);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

// GetWindowBand 没有公开文档，按名字去 user32 里找，找不到就不报。
//
// band 说明一个窗口在哪一层叠放序列里。锁屏、开始菜单这类沉浸式界面和普通桌面窗口
// 不在同一层，GetTopWindow 却会把它们混在一起吐出来——一个「可见、置顶、铺满全屏」
// 却在别的 band 里的窗口，在屏幕上根本看不见，却会被当成挡在所有窗口前面。
using GetWindowBandFn = BOOL(WINAPI*)(HWND, DWORD*);
GetWindowBandFn g_getWindowBand = nullptr;

[[nodiscard]] std::wstring BandOf(HWND hwnd) {
    if (g_getWindowBand == nullptr) return L"?";
    DWORD band = 0;
    if (!g_getWindowBand(hwnd, &band)) return L"?";
    return std::to_wstring(band);
}

// 带上 HRESULT：调用失败时变量保持 0，不查返回值就会把「问不出来」当成「没被藏」。
[[nodiscard]] std::wstring CloakOf(HWND hwnd) {
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return L"查不到";
    }
    return cloaked != 0 ? L"是" : L"否";
}

[[nodiscard]] std::wstring ExStyleOf(HWND hwnd) {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    std::wstring flags;
    const auto add = [&](LONG_PTR bit, const wchar_t* name) {
        if ((ex & bit) == 0) return;
        if (!flags.empty()) flags += L",";
        flags += name;
    };
    add(WS_EX_TOPMOST, L"TOPMOST");
    add(WS_EX_TRANSPARENT, L"TRANSPARENT");
    add(WS_EX_LAYERED, L"LAYERED");
    add(WS_EX_NOACTIVATE, L"NOACTIVATE");
    add(WS_EX_TOOLWINDOW, L"TOOL");
    return flags.empty() ? L"-" : flags;
}

[[nodiscard]] std::vector<std::wstring> ToWide(const std::vector<std::string>& items) {
    std::vector<std::wstring> out;
    out.reserve(items.size());
    for (const auto& item : items) out.push_back(win::Utf8ToWide(item));
    return out;
}

[[nodiscard]] std::wstring Join(const std::vector<std::string>& items) {
    if (items.empty()) return L"(空)";
    std::wstring out;
    for (const auto& item : items) {
        if (!out.empty()) out += L" | ";
        out += win::Utf8ToWide(item);
    }
    return out;
}

[[nodiscard]] std::filesystem::path OwnExePath() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return buffer;
}

// ---------------------------------------------------------------------------
// 屏幕取样：规划了边框的地方，屏幕上到底有没有边框色

class ScreenGrab {
public:
    ScreenGrab() {
        left_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        top_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (width_ <= 0 || height_ <= 0) return;

        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap != nullptr && bits != nullptr) {
            const HGDIOBJ old = SelectObject(memory, bitmap);
            // CAPTUREBLT：分层窗口也要截进来——边框画布本身就是分层窗口。
            const BOOL copied =
                BitBlt(memory, 0, 0, width_, height_, screen, left_, top_, SRCCOPY | CAPTUREBLT);
            SelectObject(memory, old);
            if (copied != FALSE) {
                const auto* first = static_cast<const std::uint32_t*>(bits);
                pixels_.assign(first,
                               first + static_cast<size_t>(width_) * static_cast<size_t>(height_));
            }
        }
        if (bitmap != nullptr) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        usable_ = HasVariety();
    }

    // 截到的东西能不能拿来下结论。
    //
    // 远程桌面窗口最小化、会话锁着、桌面没在显示的时候，截屏要么直接失败，要么拿回
    // 一整张同色的图。拿那种图去取样，「屏幕上一个边框色都没有」就成了必然——报告会
    // 言之凿凿地说画布没显示出来，其实是根本没看见屏幕。实测第一版就是这么误报的。
    [[nodiscard]] bool Usable() const { return usable_; }

    [[nodiscard]] bool Matches(int x, int y, unsigned argb, int tolerance) const {
        const int px = x - left_;
        const int py = y - top_;
        if (pixels_.empty() || px < 0 || py < 0 || px >= width_ || py >= height_) return false;
        const std::uint32_t pixel = pixels_[static_cast<size_t>(py) * static_cast<size_t>(width_) +
                                            static_cast<size_t>(px)];
        const auto channel = [](std::uint32_t value, int shift) {
            return static_cast<int>((value >> shift) & 0xFFU);
        };
        for (const int shift : {16, 8, 0}) {
            if (std::abs(channel(pixel, shift) - channel(argb, shift)) > tolerance) return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool HasVariety() const {
        if (pixels_.empty()) return false;
        const std::uint32_t first = pixels_.front() & 0xFFFFFFU;
        const size_t stride = std::max<size_t>(1, pixels_.size() / 4096);
        for (size_t i = 0; i < pixels_.size(); i += stride) {
            if ((pixels_[i] & 0xFFFFFFU) != first) return true;
        }
        return false;
    }

    int left_{};
    int top_{};
    int width_{};
    int height_{};
    bool usable_{false};
    std::vector<std::uint32_t> pixels_;
};

// ---------------------------------------------------------------------------
// 各节

void CollectSystem(bool dpiAware) {
    Section(L"系统");
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::wstring product = RegText(HKEY_LOCAL_MACHINE, nt, L"ProductName");
    const std::wstring edition = RegText(HKEY_LOCAL_MACHINE, nt, L"EditionID");
    const std::wstring display = RegText(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion");
    const std::wstring build = RegText(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild");
    const DWORD ubr = RegNumber(HKEY_LOCAL_MACHINE, nt, L"UBR");

    // ProductName 在 Windows 11 上照样写着「Windows 10」，只能按内部版本号判断。
    const bool win11 = std::wcstol(build.c_str(), nullptr, 10) >= 22000;
    Line(L"系统：%ls（ProductName=%ls）", win11 ? L"Windows 11" : L"Windows 10", product.c_str());
    Line(L"版本：%ls  内部版本 %ls.%lu  版次 %ls", display.c_str(), build.c_str(), ubr,
         edition.c_str());

    // LTSC 的 EditionID 以 S 结尾：EnterpriseS、IoTEnterpriseS。
    const bool ltsc = !edition.empty() && edition.back() == L'S' &&
                      edition.find(L"Enterprise") != std::wstring::npos;
    if (ltsc) {
        Finding(L"系统是 LTSC / IoT 版（%ls）。这类版本精简了系统自带的应用组件，SmartScreen "
                L"的警告界面可能根本弹不出来：带「来自网络」标记、没有数字签名的新程序会被"
                L"静默拦下，双击毫无反应、以管理员身份运行也一样。",
                edition.c_str());
    }

    const bool remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Line(L"会话：%ls  会话号 %lu  控制台会话号 %lu", remote ? L"远程桌面" : L"本机",
         session, WTSGetActiveConsoleSessionId());
    Line(L"以管理员身份运行：%ls", IsElevated() ? L"是" : L"否");
    Line(L"DPI 感知（Per-Monitor V2）：%ls", dpiAware ? L"设置成功" : L"设置失败");

    wchar_t temp[MAX_PATH]{};
    GetEnvironmentVariableW(L"TEMP", temp, static_cast<DWORD>(std::size(temp)));
    Line(L"临时目录：%ls", Redact(temp).c_str());
    if (RegText(HKEY_CURRENT_USER, L"Environment", L"TEMP").empty()) {
        Finding(L"当前用户的 TEMP 环境变量不见了（HKCU\\Environment 里没有 TEMP），临时文件"
                L"会落到系统目录，一些程序会因此莫名出错。");
    }
}

struct MonitorRow {
    RECT bounds{};
    RECT work{};
};

std::vector<MonitorRow> CollectMonitors() {
    Section(L"显示器");
    std::vector<MonitorRow> rows;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi) == FALSE) return TRUE;
            UINT dpiX = 0;
            UINT dpiY = 0;
            GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
            Line(L"%ls  屏幕 %ls  工作区 %ls  DPI %u（%u%%）%ls", mi.szDevice,
                 RectText(mi.rcMonitor).c_str(), RectText(mi.rcWork).c_str(), dpiX,
                 dpiX * 100U / 96U, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 ? L"  主屏" : L"");
            reinterpret_cast<std::vector<MonitorRow>*>(param)->push_back(
                MonitorRow{mi.rcMonitor, mi.rcWork});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&rows));
    return rows;
}

struct RunningCopy {
    DWORD pid{};
    DWORD session{};
    std::wstring path;
};

std::vector<RunningCopy> FindRunning(const wchar_t* exeName) {
    std::vector<RunningCopy> found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, exeName) != 0) continue;
        RunningCopy copy;
        copy.pid = entry.th32ProcessID;
        ProcessIdToSessionId(copy.pid, &copy.session);
        copy.path = win::QueryProcessPath(copy.pid);
        found.push_back(std::move(copy));
    }
    CloseHandle(snap);
    return found;
}

std::vector<RunningCopy> CollectWindowMark(const std::vector<MonitorRow>& monitors) {
    Section(L"WindowMark 进程");
    const std::vector<RunningCopy> running = FindRunning(app::kMainExeName);
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    if (running.empty()) {
        Line(L"没有在运行");
        Finding(L"WindowMark 没有在运行。如果是双击之后毫无反应，最常见的原因是 exe 带着「来自"
                L"网络」标记被 SmartScreen 拦下了，见「文件标记」一节。");
    }
    for (const RunningCopy& copy : running) {
        const int zone = ZoneIdOf(copy.path);
        Line(L"pid %lu  会话 %lu  版本 %ls  网络标记 %ls  路径 %ls", copy.pid, copy.session,
             FileVersionOf(copy.path).c_str(),
             zone < 0 ? L"无" : (L"ZoneId=" + std::to_wstring(zone)).c_str(),
             Redact(copy.path).c_str());
        if (copy.session != mySession) {
            Finding(L"WindowMark（pid %lu）运行在会话 %lu，而这个诊断工具在会话 %lu。边框画在"
                    L"那个会话的桌面上，在这里看不到；下面的画布和屏幕取样也只查得到本会话。",
                    copy.pid, copy.session, mySession);
        }
    }
    if (running.size() > 1) {
        Finding(L"同时有 %zu 个 WindowMark 进程在跑。正常只该有一个——多出来的多半是不同会话或"
                L"不同版本，版本号以托盘里那个为准。",
                running.size());
    }

    Line(L"托盘控制窗口：%ls",
         FindWindowW(app::kControlWindowClass, nullptr) != nullptr ? L"找到" : L"没找到");

    std::vector<HWND> overlays;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
            if (std::wcscmp(name, app::kOverlayWindowClass) == 0) {
                reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&overlays));

    Line(L"边框画布：%zu 张", overlays.size());
    int mismatched = 0;
    for (HWND overlay : overlays) {
        RECT r{};
        GetWindowRect(overlay, &r);
        const bool matchesMonitor = std::any_of(monitors.begin(), monitors.end(),
            [&r](const MonitorRow& m) { return EqualRect(&m.bounds, &r) != FALSE; });
        if (!matchesMonitor) ++mismatched;
        Line(L"  %ls  可见=%ls  %ls%ls", RectText(r).c_str(),
             IsWindowVisible(overlay) != FALSE ? L"是" : L"否", ExStyleOf(overlay).c_str(),
             matchesMonitor ? L"" : L"  <- 对不上任何一块显示器");
    }
    const bool runsHere = std::any_of(running.begin(), running.end(),
        [mySession](const RunningCopy& copy) { return copy.session == mySession; });
    if (runsHere && overlays.empty()) {
        Finding(L"WindowMark 在运行，但一张边框画布都没有：边框后端没有启动（边框被关掉了，"
                L"或者启动失败）。");
    }
    if (mismatched > 0) {
        Finding(L"有 %d 张边框画布的尺寸对不上当前的显示器。显示配置变过（插拔、改分辨率、"
                L"远程接入）而画布没有跟上，边框会画在错的地方或被截掉。",
                mismatched);
    }
    return running;
}

void CollectFileMarks() {
    Section(L"文件标记（来自网络）");
    const std::filesystem::path self = OwnExePath();
    const std::filesystem::path dir = self.parent_path();
    Line(L"检查目录：%ls", Redact(dir.wstring()).c_str());

    int marked = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".exe") != 0) continue;
        const int zone = ZoneIdOf(entry.path().wstring());
        Line(L"  %-28ls %ls", entry.path().filename().c_str(),
             zone < 0 ? L"无标记" : (L"ZoneId=" + std::to_wstring(zone)).c_str());
        if (zone >= 3) ++marked;
    }
    if (marked > 0) {
        Finding(L"这个目录里有 %d 个 exe 带「来自网络」标记（ZoneId=3）。没有数字签名的程序带着"
                L"这个标记，双击时要先过 SmartScreen；系统的警告界面坏了或被精简掉时连提示都"
                L"弹不出来，表现就是双击毫无反应。解决办法：退出 WindowMark，右键下载的 zip →"
                L"「属性」→ 勾选「解除锁定」→ 确定，然后重新解压。",
                marked);
    }
}

[[nodiscard]] const wchar_t* SourceName(windowmark::ConfigSource source) {
    switch (source) {
    case windowmark::ConfigSource::Portable: return L"exe 同目录（便携）";
    case windowmark::ConfigSource::Configured: return L"注册表指定";
    case windowmark::ConfigSource::Fallback: return L"默认位置";
    }
    return L"?";
}

Settings CollectConfig(const std::vector<RunningCopy>& running) {
    Section(L"配置");

    // 「exe 同目录」指的是**正在运行的那个 WindowMark.exe** 的目录，不是诊断工具自己的。
    // 两者不在一处时（跑的是安装版，诊断工具在解压目录里），按自己的目录去解析就会读到
    // 另一份配置，报告和实际行为对不上。
    std::filesystem::path exe = OwnExePath();
    if (!running.empty() && !running.front().path.empty()) exe = running.front().path;

    windowmark::ConfigLocationInputs inputs;
    std::error_code ec;
    inputs.portable = exe.parent_path() / L"settings.conf";
    inputs.portableExists = std::filesystem::exists(inputs.portable, ec);
    inputs.configured = win::ReadConfiguredConfigPath();
    inputs.configuredUsable =
        !inputs.configured.empty() && win::IsDirectoryWritable(inputs.configured.parent_path());
    const std::filesystem::path root = win::LocalDataRoot();
    inputs.fallback = root.empty() ? std::filesystem::path{} : root / L"settings.conf";
    const windowmark::ConfigLocation location = windowmark::ResolveConfigLocation(inputs);

    Line(L"候选：便携 %ls（%ls）", Redact(inputs.portable.wstring()).c_str(),
         inputs.portableExists ? L"存在" : L"不存在");
    Line(L"候选：注册表 %ls", inputs.configured.empty()
                                 ? L"(未指定)"
                                 : Redact(inputs.configured.wstring()).c_str());
    Line(L"候选：默认 %ls", Redact(inputs.fallback.wstring()).c_str());
    Line(L"实际生效：%ls（%ls）", Redact(location.path.wstring()).c_str(),
         SourceName(location.source));
    if (location.configuredUnavailable) {
        Finding(L"注册表里指定的配置位置用不了（目录不存在或不可写），已回落到默认位置——改过"
                L"的设置可能不在你以为的那份文件里。");
    }

    Settings settings;
    // LoadOrCreate 在文件不存在时会创建它，诊断工具不能写用户的配置——只在存在时读。
    if (std::filesystem::exists(location.path, ec)) {
        settings = Settings::LoadOrCreate(location.path);
    } else {
        Line(L"配置文件不存在，下面按默认值");
    }

    const auto& b = settings.border;
    Line(L"border.enabled=%ls  width=%d  offset=%d  corners=%ls  corner_radius=%d  "
         L"corner_width_extra=%d  corner_inset=%d",
         b.enabled ? L"true" : L"false", b.width, b.offset,
         win::Utf8ToWide(windowmark::ToString(b.corners)).c_str(), b.cornerRadius,
         b.cornerWidthExtra, b.cornerInset);
    Line(L"border.active_color=#%06X  inactive_color=#%06X", b.activeColor & 0xFFFFFFU,
         b.inactiveColor & 0xFFFFFFU);
    // 排除名单里存的是完整路径，只报文件名：路径里常带用户名和私人目录。
    std::wstring excluded;
    for (const auto& key : b.excludedAppKeys) {
        if (!excluded.empty()) excluded += L" | ";
        excluded += std::filesystem::path(win::Utf8ToWide(key)).filename().wstring();
    }
    Line(L"不画边框的应用：%ls", excluded.empty() ? L"(无)" : excluded.c_str());
    Line(L"pin.enabled=%ls  pin.width=%d  drawer.enabled=%ls  drag.enabled=%ls",
         settings.pin.enabled ? L"true" : L"false", settings.pin.width,
         settings.drawer.enabled ? L"true" : L"false", settings.drag.enabled ? L"true" : L"false");
    Line(L"tracking.exclude_classes=%ls", Join(settings.tracking.excludeClasses).c_str());
    Line(L"tracking.force_include_classes=%ls", Join(settings.tracking.forceIncludeClasses).c_str());
    Line(L"tracking.treat_as_topmost_classes=%ls",
         Join(settings.tracking.treatAsTopmostClasses).c_str());
    Line(L"tracking.shadow_insets=%ls", Join(settings.tracking.shadowInsets).c_str());

    if (!b.enabled) {
        Finding(L"border.enabled=false：边框功能是关着的。托盘 →「窗口边框」→「启用」。");
    }
    return settings;
}

[[nodiscard]] bool SameRect(const windowmark::Rect& a, const windowmark::Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

[[nodiscard]] bool Intersects(const RECT& a, const windowmark::Rect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

void CollectBorders(const Settings& settings, const std::vector<RunningCopy>& running) {
    Section(L"边框规划回放（调用 WindowMark 自己的快照与规划代码）");
    if (!settings.border.enabled) {
        Line(L"边框是关着的；下面按「打开」的情况回放，看打开之后会怎样");
    }
    Settings plan = settings;
    plan.border.enabled = true;

    const auto shadowInsets = win::ParseShadowInsets(plan.tracking.shadowInsets);
    const auto treatAsTopmost = ToWide(plan.tracking.treatAsTopmostClasses);
    const auto excludeClasses = ToWide(plan.tracking.excludeClasses);
    const auto forceInclude = ToWide(plan.tracking.forceIncludeClasses);

    const win::DesktopSnapshot snapshot = win::CaptureDesktop(shadowInsets, treatAsTopmost);
    const ScreenGrab grab;

    HWND foreground = snapshot.foreground;
    Line(L"快照：%zu 个窗口。前台：%ls（%ls）", snapshot.windows.size(),
         ClassOf(foreground).c_str(), ProcessNameOf(foreground).c_str());

    // 按 Coordinator::BuildBorderModels 的口径挑出该画边框的窗口，挑不上的记下原因。
    struct Row {
        const win::SnapshotWindow* window{};
        std::wstring skip;
        windowmark::Rect outer{};
    };
    std::vector<Row> rows;
    std::vector<BorderModel> models;
    const int reach = std::max(0, std::max(1, plan.border.width) + plan.border.offset);

    for (const win::SnapshotWindow& w : snapshot.windows) {
        if (w.desktop) continue;
        Row row;
        row.window = &w;
        const DWORD pid = ProcessIdOf(w.hwnd);
        const std::wstring path = win::QueryProcessPath(pid);
        const bool ownProcess = std::any_of(running.begin(), running.end(),
            [pid](const RunningCopy& copy) { return copy.pid == pid; });

        if (!win::IsEligibleTopLevelWindow(w.hwnd, excludeClasses, forceInclude)) {
            row.skip = L"不算普通窗口";
        } else if (ownProcess || pid == GetCurrentProcessId()) {
            row.skip = L"WindowMark 自己的窗口";
        } else if (path.empty()) {
            row.skip = L"取不到进程路径";
        } else if (w.minimized) {
            row.skip = L"最小化";
        } else if (w.maximized) {
            row.skip = L"最大化：按设计不画";
        } else if (w.cloaked) {
            row.skip = L"被 DWM 隐藏";
        } else {
            const std::string key = win::LowerAscii(win::WideToUtf8(path));
            const auto& keys = plan.border.excludedAppKeys;
            if (std::any_of(keys.begin(), keys.end(), [&key](const std::string& k) {
                    return win::LowerAscii(k) == key;
                })) {
                row.skip = L"应用在「不画边框」名单里";
            }
        }

        const windowmark::Rect frame = win::ToCoreRect(w.frame);
        row.outer = windowmark::Rect{frame.left - reach, frame.top - reach, frame.right + reach,
                                     frame.bottom + reach};
        if (row.skip.empty()) {
            BorderModel model;
            model.windowId = static_cast<windowmark::WindowId>(reinterpret_cast<std::uintptr_t>(w.hwnd));
            model.frame = frame;
            model.active = w.hwnd == foreground;
            models.push_back(model);
        }
        rows.push_back(std::move(row));
    }

    const std::vector<win::BorderStroke> strokes = win::PlanBorders(snapshot, models, plan);
    Line(L"该画边框的窗口 %zu 个，规划出 %zu 段", models.size(), strokes.size());
    Line(L"");
    Line(L"%-3ls %-30ls %-18ls %-6ls %-6ls %-8ls %ls", L"#", L"类名", L"进程", L"段数", L"取样",
         L"置顶", L"状态");

    int shown = 0;
    int plannedWindows = 0;
    int clippedWindows = 0;
    int sampledExpected = 0;
    int sampledFound = 0;
    std::map<std::wstring, int> blockers;

    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const win::SnapshotWindow& w = *row.window;
        const bool isForeground = w.hwnd == foreground;

        std::vector<const win::BorderStroke*> own;
        for (const win::BorderStroke& s : strokes) {
            if (SameRect(s.ringOuter, row.outer)) own.push_back(&s);
        }

        std::wstring status = row.skip;
        std::wstring sample = L"-";
        if (row.skip.empty()) {
            if (!own.empty() && !grab.Usable()) {
                ++plannedWindows;
                status = isForeground ? L"画（前台）" : L"画";
                sample = L"?";
            } else if (!own.empty()) {
                ++plannedWindows;
                // 四条边的中点往外两像素，落在规划的段里才算「应该看得见」，再垂直于边
                // 扫一小段找边框色。被别的窗口挡住的边不算——那本来就不该画。
                const windowmark::Rect f = win::ToCoreRect(w.frame);
                const int midX = (f.left + f.right) / 2;
                const int midY = (f.top + f.bottom) / 2;
                const std::array<std::array<int, 4>, 4> probes{{
                    {f.left, midY, -1, 0}, {f.right - 1, midY, 1, 0},
                    {midX, f.top, 0, -1}, {midX, f.bottom - 1, 0, 1}}};
                int expected = 0;
                int found = 0;
                for (const auto& p : probes) {
                    const int ox = p[0] + p[2] * 2;
                    const int oy = p[1] + p[3] * 2;
                    const bool covered = std::any_of(own.begin(), own.end(),
                        [ox, oy](const win::BorderStroke* s) {
                            return ox >= s->rect.left && ox < s->rect.right &&
                                   oy >= s->rect.top && oy < s->rect.bottom;
                        });
                    if (!covered) continue;
                    ++expected;
                    for (int k = -4; k <= 10; ++k) {
                        if (grab.Matches(p[0] + p[2] * k, p[1] + p[3] * k, own.front()->color, 48)) {
                            ++found;
                            break;
                        }
                    }
                }
                sampledExpected += expected;
                sampledFound += found;
                sample = std::to_wstring(found) + L"/" + std::to_wstring(expected);
                status = isForeground ? L"画（前台）" : L"画";
            } else {
                ++clippedWindows;
                status = L"整圈被裁掉：";
                // 列出排在它前面、和它的边框环相交的窗口——PlanBorders 就是拿它们裁的。
                int listed = 0;
                for (size_t j = 0; j < i && listed < 3; ++j) {
                    const win::SnapshotWindow& other = *rows[j].window;
                    if (other.cloaked || other.minimized) continue;
                    if (isForeground && !(other.topmost || other.treatAsTopmost ||
                                          other.owner == w.hwnd)) {
                        continue;
                    }
                    if (!Intersects(other.frame, row.outer)) continue;
                    const std::wstring who = ClassOf(other.hwnd) + L"(" + ProcessNameOf(other.hwnd) + L")";
                    status += (listed > 0 ? L"、" : L"") + who;
                    ++blockers[who];
                    ++listed;
                }
                if (listed == 0) status += L"没找到挡住它的窗口";
            }
        }

        if (shown < 40) {
            Line(L"%-3zu %-30ls %-18ls %-6zu %-6ls %-8ls %ls%ls", i + 1, ClassOf(w.hwnd).c_str(),
                 ProcessNameOf(w.hwnd).c_str(), own.size(), sample.c_str(),
                 w.topmost ? L"是" : L"", isForeground ? L"<前台> " : L"", status.c_str());
            ++shown;
        }
    }
    if (rows.size() > 40) Line(L"……其余 %zu 个窗口略", rows.size() - 40);

    // 前台窗口的补充：它是用户最在意、也最能说明问题的那一个。
    Line(L"");
    for (const Row& row : rows) {
        if (row.window->hwnd != foreground) continue;
        const win::SnapshotWindow& w = *row.window;
        Line(L"前台窗口：%ls（%ls） 矩形 %ls  band %ls  cloak %ls  扩展样式 %ls",
             ClassOf(w.hwnd).c_str(), ProcessNameOf(w.hwnd).c_str(), RectText(w.frame).c_str(),
             BandOf(w.hwnd).c_str(), CloakOf(w.hwnd).c_str(), ExStyleOf(w.hwnd).c_str());
        if (w.maximized) {
            Finding(L"前台窗口是最大化的。v0.4.9 起最大化窗口按设计不画边框（v0.4.8 及以前会画）"
                    L"——如果平时窗口都是最大化的，看起来就像边框功能完全没用。");
        }
    }

    // 排在最前面、又大又置顶的窗口单独列出来：它们最可能把所有人都挡掉。
    const int screenArea = GetSystemMetrics(SM_CXVIRTUALSCREEN) * GetSystemMetrics(SM_CYVIRTUALSCREEN);
    for (const Row& row : rows) {
        const win::SnapshotWindow& w = *row.window;
        if (!w.topmost || w.cloaked || w.minimized) continue;
        const long area = (w.frame.right - w.frame.left) * (w.frame.bottom - w.frame.top);
        if (screenArea <= 0 || area * 2 < screenArea) continue;
        Line(L"大的置顶窗口：%ls（%ls） 矩形 %ls  band %ls  cloak %ls  扩展样式 %ls",
             ClassOf(w.hwnd).c_str(), ProcessNameOf(w.hwnd).c_str(), RectText(w.frame).c_str(),
             BandOf(w.hwnd).c_str(), CloakOf(w.hwnd).c_str(), ExStyleOf(w.hwnd).c_str());
    }

    if (!models.empty() && plannedWindows == 0) {
        std::wstring top;
        for (const auto& [who, count] : blockers) {
            if (!top.empty()) top += L"、";
            top += who + L"×" + std::to_wstring(count);
        }
        Finding(L"有 %zu 个窗口该画边框，但每一个的整圈边框都被遮挡计算裁掉了。挡住它们的："
                L"%ls。如果这个窗口在屏幕上其实看不见，就是它把所有边框都吃掉了。",
                models.size(), top.empty() ? L"(没找到)" : top.c_str());
    } else if (clippedWindows > 0) {
        Line(L"另有 %d 个窗口的整圈边框被裁掉（被别的窗口完全挡住时这是正常的）", clippedWindows);
    }

    const bool runsHere = std::any_of(running.begin(), running.end(), [](const RunningCopy& copy) {
        DWORD mine = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &mine);
        return copy.session == mine;
    });
    if (!grab.Usable()) {
        Line(L"屏幕取样：截不到屏幕，跳过");
        Finding(L"截不到这个会话的屏幕%ls：桌面此刻没有在显示（远程桌面窗口最小化或断开、"
                L"锁屏中）。这种状态下「边框有没有画出来」没法检查，请在能正常看到桌面的时候"
                L"重新运行本工具。",
                foreground == nullptr ? L"，也拿不到前台窗口" : L"");
    } else {
        Line(L"屏幕取样：应当看得见的边 %d 条，找到边框色 %d 条", sampledExpected, sampledFound);
    }
    if (grab.Usable() && runsHere && settings.border.enabled && sampledExpected > 0 &&
        sampledFound == 0) {
        Finding(L"规划里有边框，屏幕上却一个边框色都没取到：画布没有显示出来（渲染或合成出了"
                L"问题），或者边框颜色被改过。");
    }
}

void CollectLogs() {
    Section(L"WindowMark 自己的日志");
    const std::filesystem::path root = win::LocalDataRoot();
    for (const wchar_t* name : {L"startup.log", L"diag.log"}) {
        const std::filesystem::path path = root / name;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            Line(L"%ls：没有", name);
            continue;
        }
        // 只读末尾一截：diag.log 开着诊断时一秒一条，可能很大。
        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        const std::streamoff start = std::max<std::streamoff>(0, size - 16000);
        input.seekg(start);
        std::string tail((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::vector<std::string> lines;
        size_t from = 0;
        while (from < tail.size()) {
            size_t end = tail.find('\n', from);
            if (end == std::string::npos) end = tail.size();
            std::string one = tail.substr(from, end - from);
            if (!one.empty() && one.back() == '\r') one.pop_back();
            if (!one.empty()) lines.push_back(std::move(one));
            from = end + 1;
        }
        if (start > 0 && !lines.empty()) lines.erase(lines.begin());   // 截断的半行不要
        const size_t keep = name == std::wstring(L"diag.log") ? 30U : 10U;
        Line(L"%ls：最后 %zu 行", name, std::min(keep, lines.size()));
        for (size_t i = lines.size() > keep ? lines.size() - keep : 0; i < lines.size(); ++i) {
            Line(L"  %ls", Redact(win::Utf8ToWide(lines[i])).c_str());
        }
    }
    Line(L"想要更详细的记录：在 %%LOCALAPPDATA%%\\WindowMark 里新建一个空文件 diag.on，"
         L"复现一次问题，再运行本工具。");
}

[[nodiscard]] std::filesystem::path DesktopReportPath() {
    PWSTR desktop = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) {
        dir = desktop;
    }
    if (desktop != nullptr) CoTaskMemFree(desktop);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[96]{};
    swprintf_s(name, L"WindowMark诊断_%04u%02u%02u_%02u%02u%02u.txt", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond);
    return dir.empty() ? std::filesystem::path(name) : dir / name;
}

bool WriteUtf8(const std::filesystem::path& path, const std::wstring& text) {
    const std::string utf8 = win::WideToUtf8(text);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    // 带 BOM：老版本记事本不认无 BOM 的 UTF-8，中文会变成乱码。
    out.write("\xEF\xBB\xBF", 3);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(out);
}

void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        auto* target = static_cast<wchar_t*>(GlobalLock(memory));
        if (target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool dpiAware =
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;

    std::wstring outPath;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::wcscmp(argv[i], L"--out") == 0) outPath = argv[i + 1];
        }
        LocalFree(argv);
    }

    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile))) > 0) {
        g_profile = profile;
    }
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        g_getWindowBand = reinterpret_cast<GetWindowBandFn>(GetProcAddress(user32, "GetWindowBand"));
    }

    CollectSystem(dpiAware);
    const std::vector<MonitorRow> monitors = CollectMonitors();
    const std::vector<RunningCopy> running = CollectWindowMark(monitors);
    CollectFileMarks();
    const Settings settings = CollectConfig(running);
    CollectBorders(settings, running);
    CollectLogs();

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t header[512]{};
    swprintf_s(header,
               L"WindowMark 诊断报告\r\n生成时间 %04u-%02u-%02u %02u:%02u:%02u  诊断工具版本 %ls\r\n"
               L"（不含窗口标题；路径里的用户目录已替换成 %%USERPROFILE%%）\r\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
               app::kProductVersion);

    std::wstring report = header;
    report += L"\r\n== 发现的问题 ==\r\n";
    if (g_findings.empty()) {
        report += L"没有发现明显问题。\r\n";
    } else {
        for (size_t i = 0; i < g_findings.size(); ++i) {
            report += std::to_wstring(i + 1) + L". " + g_findings[i] + L"\r\n";
        }
    }
    report += g_body;

    if (!outPath.empty()) {
        return WriteUtf8(outPath, report) ? 0 : 1;
    }

    const std::filesystem::path path = DesktopReportPath();
    const bool saved = WriteUtf8(path, report);
    CopyToClipboard(report);

    std::wstring message;
    if (saved) {
        message = L"诊断报告已经保存到桌面，并复制到了剪贴板：\n\n" + path.filename().wstring() +
                  L"\n\n直接粘贴发给开发者就行。\n\n现在打开报告看看吗？";
    } else {
        message = L"报告没能保存到桌面，但已经复制到了剪贴板——直接粘贴发给开发者就行。";
    }
    const int choice = MessageBoxW(nullptr, message.c_str(), L"WindowMark 诊断",
                                   (saved ? MB_YESNO : MB_OK) | MB_ICONINFORMATION);
    if (saved && choice == IDYES) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
