#include "WinDesktopSnapshot.h"

#include <dwmapi.h>

#include <cwchar>

namespace windowmark::win {
namespace {

// 遍历 z 序的上限。桌面上挂着数百个隐藏的顶级窗口（实测单个 Excel 窗口上方就压着
// 144 个），这个上限只是防止病态 z 序把线程转死。
constexpr int kZOrderLimit = 4096;

[[nodiscard]] bool IsCloaked(HWND hwnd) {
    // 切到别的虚拟桌面时，那边的窗口 IsWindowVisible 仍然返回 true——只看它会让 z 序
    // 和遮挡计算全乱（实测切到空桌面时枚举结果完全对不上）。DWMWA_CLOAKED 才问得出
    // 「DWM 到底画不画它」。
    int cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != 0;
}

// 自家的窗口一律不进快照。
//
// 最要紧的是 overlay 自己：它是 topmost、排在 z 序最前，一旦被当成遮挡物，每个窗口
// 的边框都会被它整片裁掉——现象是「非激活窗口全都没有边框」。书签条、设置对话框
// 同理，它们该不该盖住边框由它们自己的 z 序决定，不该在遮挡计算里再算一遍。
[[nodiscard]] bool IsOwnWindow(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) == 0) return false;
    return std::wcsncmp(cls, L"WindowMark.", 11) == 0;
}

[[nodiscard]] RECT FrameOf(HWND hwnd) {
    RECT frame{};
    // GetWindowRect 会把不可见的 resize border 算进去（本机 125% 缩放下实测 8px），
    // 还有 DPI virtualization 掺一脚。DWM 的扩展边界才是屏幕上实际画出来的那一圈。
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                        sizeof(frame)))) {
        return frame;
    }
    GetWindowRect(hwnd, &frame);
    return frame;
}

} // namespace

DesktopSnapshot CaptureDesktop() {
    DesktopSnapshot snapshot;
    snapshot.foreground = GetForegroundWindow();

    HWND hwnd = GetTopWindow(nullptr);
    for (int step = 0; step < kZOrderLimit && hwnd != nullptr; ++step) {
        if (IsWindowVisible(hwnd) != FALSE && !IsOwnWindow(hwnd)) {
            SnapshotWindow entry;
            entry.hwnd = hwnd;
            entry.frame = FrameOf(hwnd);
            entry.cloaked = IsCloaked(hwnd);
            entry.maximized = IsZoomed(hwnd) != FALSE;
            entry.minimized = IsIconic(hwnd) != FALSE;
            // 空矩形的窗口对遮挡没有贡献，也不该被描边——直接不收，省得下游到处判空。
            // Windows Terminal 的 PseudoConsoleWindow 之类就是 0x0 但「可见」。
            if (entry.frame.right > entry.frame.left &&
                entry.frame.bottom > entry.frame.top) {
                snapshot.windows.push_back(entry);
            }
        }
        hwnd = GetWindow(hwnd, GW_HWNDNEXT);
    }
    return snapshot;
}

} // namespace windowmark::win
