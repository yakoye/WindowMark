#include "WinDesktopSnapshot.h"

#include <dwmapi.h>

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
        if (IsWindowVisible(hwnd) != FALSE) {
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
