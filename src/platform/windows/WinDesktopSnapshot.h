#pragma once

#include "WinUtil.h"

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
    bool topmost{false};   // WS_EX_TOPMOST
    // 用户在 tracking.treat_as_topmost_classes 里点名的窗口。它不带 WS_EX_TOPMOST，
    // 但实际浮在上面（自绘的浮动面板、输入法候选框之类），边框得给它让路。
    bool treatAsTopmost{false};
    // 桌面本身（Progman，或装了动态壁纸时的 WorkerW）。它铺满整个虚拟桌面却谁都不
    // 挡——永远在 z 序最底。用户点一下桌面它就成了前台，那时不能拿它的矩形去裁别人
    // 的边框，一裁就是全屏。
    bool desktop{false};
    HWND owner{};          // GW_OWNER，判断「这是不是某个窗口自己的对话框」
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
//
// shadowInsets 是「这个窗口类自绘的阴影有多厚」，用来把窗口矩形收到它看得见的边缘。
// 这里必须和画边框那一层用同一份值：一个自绘阴影的窗口，矩形比它看得见的部分大一圈，
// 拿没修正的矩形去裁别人的边框，断口就会比它本身宽一截——看着像边框断了，而不是被
// 它挡住。
[[nodiscard]] DesktopSnapshot CaptureDesktop(
    const std::vector<ShadowInset>& shadowInsets,
    const std::vector<std::wstring>& treatAsTopmostClasses);

// 只把某一个窗口的几何刷新一遍，别人照旧。
//
// 拖动时几何事件每秒来上百个，每个都整取一次桌面是纯浪费：动的只有一个窗口。z 序、
// 前台、谁出现谁消失，这些都由别的事件负责让整份快照作废，几何事件管不着。
//
// 返回 false 表示这个窗口不在快照里（刚出现，或刚从隐藏变可见），调用方该整取一次。
bool RefreshWindowFrame(DesktopSnapshot& snapshot, HWND hwnd,
                        const std::vector<ShadowInset>& shadowInsets);

} // namespace windowmark::win
