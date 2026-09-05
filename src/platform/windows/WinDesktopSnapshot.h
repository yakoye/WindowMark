#pragma once

#include <windows.h>

#include <vector>

namespace windowmark::win {

// 一个窗口在某一瞬间的样子。
struct SnapshotWindow {
    HWND hwnd{};
    RECT frame{};          // DWMWA_EXTENDED_FRAME_BOUNDS，取不到才退回 GetWindowRect
    bool cloaked{false};   // 被 DWM 藏起来，多半是在别的虚拟桌面
    bool maximized{false};
    bool minimized{false};
};

// 整个桌面在某一瞬间的样子。windows 按 z 序**从上到下**排列。
//
// 存在的理由是消灭 split-brain。以前 Coordinator 记着一份事件驱动的 active，平台层
// 又实时调 GetForegroundWindow()，两者在焦点切换途中会分家——同一个窗口在相邻两次
// 调用里走不同分支，一次提升一次降级，来回拉扯。现在一帧只取一次快照，那一帧里所有
// 判断都只认这一份。
//
// WinEvent 钩子的语义也跟着变了：它只负责喊「桌面脏了，重新看一眼」，而不是「事件里
// 带的值就是真相」。
struct DesktopSnapshot {
    HWND foreground{};
    std::vector<SnapshotWindow> windows;
};

// 现场取一份快照。
[[nodiscard]] DesktopSnapshot CaptureDesktop();

} // namespace windowmark::win
