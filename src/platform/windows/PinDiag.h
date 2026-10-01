#pragma once

// 全程序唯一的诊断日志。
//
// 开关是**一个文件**：配置文件（settings.conf）旁边的 diag.on。日志写在同一个目录里的
// diag.log。绿色版的配置在 exe 旁边，诊断文件也就在 exe 旁边——整个文件夹仍然是自带的，
// 排查问题不用去 %LOCALAPPDATA% 找。
//
// 为什么用文件而不是环境变量：用户是双击启动的，托盘菜单点出来的进程也没有自定义环境变量，
// 环境变量开关等于只有开发机上能用。书签那套计时日志以前就只认 WINDOWMARK_DIAG=1，于是
// 「照文档建一个 diag.on」永远拿不到书签部分的日志。
//
// 名字里的 Pin 是历史：最早只给置顶那条路用。现在边框、书签、拖动、托盘都写在这里。
//
// 日志目录由 WinMain 在解析出配置位置之后用 SetPinDiagDir 定下来。在那之前（极早期的几行）
// 用 %LOCALAPPDATA%\WindowMark，和 startup.log 同一个地方——那条审计日志必须在任何解析之前
// 就能写，所以它永远在 %LOCALAPPDATA%。

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace windowmark::win {

namespace detail {

inline std::wstring& PinDiagDirStorage() {
    static std::wstring dir;
    return dir;
}

} // namespace detail

// 配置文件所在目录。WinMain 解析完配置位置就调一次。
inline void SetPinDiagDir(std::wstring directory) {
    if (directory.empty()) return;
    if (directory.back() != L'\\' && directory.back() != L'/') directory += L'\\';
    detail::PinDiagDirStorage() = std::move(directory);
}

[[nodiscard]] inline std::wstring PinDiagDir() {
    if (!detail::PinDiagDirStorage().empty()) return detail::PinDiagDirStorage();
    wchar_t dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) == 0) return {};
    // Forward slashes on purpose: the Win32 file APIs take them, and it keeps these paths
    // free of the backslash escaping that every text tool in the chain mangles.
    return std::wstring(dir) + L"/WindowMark/";
}

[[nodiscard]] inline bool PinDiagOn() {
    // 标记是「丢进去 / 删掉」用的，所以要反复查，不能只在启动时看一眼——否则开诊断得重启，
    // 而重启本身就会把要查的现场冲掉。但也不能每次都查：书签那边每帧都有计时器调它，
    // 一秒几百次 GetFileAttributesW 是实打实的开销。折中是缓存 1 秒。
    //
    // 两个 static 上的竞态是良性的：这些路径都在主线程（钩子是 OUTOFCONTEXT，经消息循环
    // 回到主线程），最坏情况也只是多查一次文件属性。
    static ULONGLONG checkedAt = 0;
    static bool on = false;
    const ULONGLONG now = GetTickCount64();
    if (checkedAt == 0 || now - checkedAt >= 1000) {
        checkedAt = now;
        const auto dir = PinDiagDir();
        on = !dir.empty() &&
             GetFileAttributesW((dir + L"diag.on").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return on;
}

inline void PinDiag(const wchar_t* format, ...) {
    if (!PinDiagOn()) return;
    const auto dir = PinDiagDir();
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"diag.log").c_str(), L"a, ccs=UTF-8") != 0 || f == nullptr) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fwprintf(f, L"[PIN %02d:%02d:%02d.%03d] ", now.wHour, now.wMinute, now.wSecond,
                  now.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfwprintf(f, format, args);
    va_end(args);
    std::fwprintf(f, L"\n");
    std::fclose(f);
}

} // namespace windowmark::win
