#pragma once

#include "windowmark/core/ConfigLocation.h"
#include "windowmark/core/Types.h"

#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace windowmark::win {

[[nodiscard]] std::string WideToUtf8(const std::wstring& value);
[[nodiscard]] std::wstring Utf8ToWide(const std::string& value);
[[nodiscard]] std::wstring QueryProcessPath(DWORD processId);

// ASCII 小写化。用来算「应用身份」这个 key：规范化的可执行文件路径。
//
// 放在这里而不是各自实现一份：边框的排除名单、书签的禁用名单、拖动的排除名单算的是
// 同一个 key，两份实现必然漂移，而 key 算不一致的后果是名单**静默失效**——看起来
// 一切正常，就是不生效。
[[nodiscard]] std::string LowerAscii(std::string value);

// 这个窗口是不是本进程的。
//
// 项目里还有两处同名判断，语义不同，别混用：WinDesktopSnapshot 那份按类名前缀
// （遮挡计算只关心「屏幕上那些 WindowMark.* 的画布」），WinWindowBackend 那份是
// 成员函数、用缓存的 processId_。这一份给不持有那个缓存的调用方用，按进程比，
// 比取类名便宜，也不会漏掉将来某个不叫 WindowMark.* 的自家窗口。
[[nodiscard]] bool IsOwnProcessWindow(HWND hwnd);
[[nodiscard]] std::string FileStemUtf8(const std::wstring& path);
[[nodiscard]] Rect ToCoreRect(const RECT& rect);
[[nodiscard]] RECT ToWinRect(const Rect& rect);
[[nodiscard]] std::filesystem::path InstalledExePath();
std::filesystem::path LocalDataRoot();
[[nodiscard]] std::filesystem::path RoamingDataRoot();

// 配置文件的三个候选位置。存在性与可写性在这里判断，选哪一个交给 core 的
// ResolveConfigLocation，那段优先级逻辑因此可以脱离文件系统被单测覆盖。
[[nodiscard]] std::filesystem::path PortableConfigPath();
[[nodiscard]] std::filesystem::path ReadConfiguredConfigPath();
bool WriteConfiguredConfigPath(const std::filesystem::path& path);
[[nodiscard]] bool IsDirectoryWritable(const std::filesystem::path& directory);
[[nodiscard]] ConfigLocation CurrentConfigLocation();
[[nodiscard]] bool IsCloaked(HWND hwnd);
// `alsoExclude` is the user's own list of window classes, added to the built-in one.
// 某个窗口类自绘的阴影有多厚。
//
// 客户区自绘阴影的窗口（GTK、以及不少自画弹出面板的应用）把阴影画在自己的窗口矩形
// 里，没有任何 API 报得出阴影到哪儿为止，只能由用户量出来填进配置。
//
// 这个修正有两个用处，两处都得用同一个值：给这个窗口画边框时要贴着它看得见的边缘；
// 它挡住别人时，遮挡范围也只该算看得见的那一块——否则别人的边框会在它旁边断掉比它
// 本身更宽的一截。
struct ShadowInset {
    std::wstring className;
    int left{};
    int top{};
    int right{};
    int bottom{};
};

// 解析 "类名:左,上,右,下" 形式的配置项，跳过写坏的和全零的。
[[nodiscard]] std::vector<ShadowInset> ParseShadowInsets(
    const std::vector<std::string>& entries);

// 按类名把矩形往里收。收过头会让矩形翻转，那种值一律忽略——写错了该看着不对，
// 而不是让边框消失或者反着画。
void ApplyShadowInset(RECT& rect, const wchar_t* className,
                      const std::vector<ShadowInset>& insets);

// forceInclude 里的类名跳过所有「这算不算一个窗口」的判据，只保留最基本的三条：
// 它得是个真窗口、可见、并且是顶级窗口。排除名单仍然优先。
[[nodiscard]] bool IsEligibleTopLevelWindow(
    HWND hwnd, const std::vector<std::wstring>& alsoExclude = {},
    const std::vector<std::wstring>& forceInclude = {});
[[nodiscard]] Rect ExtendedFrame(HWND hwnd);
// Same thing, but says whether DWM actually answered. ExtendedFrame falls back to
// GetWindowRect on failure and the caller cannot tell the difference - which is how a
// bogus zero inset ended up cached and a border sat 8px out until the window was resized.
[[nodiscard]] Rect ExtendedFrame(HWND hwnd, bool& fromDwm);
[[nodiscard]] Rect WorkAreaFor(HWND hwnd);
// The system accent colour as 0xAARRGGBB, read fresh from the registry every call so a
// theme change is picked up without any plumbing to notice one. Shared rather than
// duplicated: the registry path is a wide string full of backslashes, and having a second
// copy of it is how the "accent never applied" bug would come back.
[[nodiscard]] unsigned SystemAccentColor();
void PurgeAllUserData();
void RemoveStartupRegistration();

} // namespace windowmark::win
