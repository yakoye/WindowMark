#include "WinDesktopSnapshot.h"

#include "windowmark/core/BorderOcclusion.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace windowmark::win {
namespace {

// 遍历 z 序的上限。桌面上挂着数百个隐藏的顶级窗口（实测单个 Excel 窗口上方就压着
// 144 个），这个上限只是防止病态 z 序把线程转死。
constexpr int kZOrderLimit = 4096;

// 判断窗口有没有被 DWM 藏起来，用的是 WinUtil 里那个 IsCloaked。
//
// 切到别的虚拟桌面时，那边的窗口 IsWindowVisible 仍然返回 true——只看它会让 z 序和
// 遮挡计算全乱（实测切到空桌面时枚举结果完全对不上）。DWMWA_CLOAKED 才问得出「DWM
// 到底画不画它」。

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

[[nodiscard]] RECT FrameOf(HWND hwnd, const std::vector<ShadowInset>& shadowInsets) {
    RECT frame{};
    // GetWindowRect 会把不可见的 resize border 算进去（本机 125% 缩放下实测 8px），
    // 还有 DPI virtualization 掺一脚。DWM 的扩展边界才是屏幕上实际画出来的那一圈。
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                     sizeof(frame)))) {
        GetWindowRect(hwnd, &frame);
    }
    // 自绘阴影的窗口，DWM 报的边界里含着它自己画的那圈阴影。最大化时没地方画阴影，
    // 也就不该收——这和画边框那一层的口径保持一致。
    if (!shadowInsets.empty() && IsZoomed(hwnd) == FALSE) {
        wchar_t className[128]{};
        if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) > 0) {
            ApplyShadowInset(frame, className, shadowInsets);
        }
    }
    return frame;
}

} // namespace

namespace {

// 读一个窗口的各项属性。收不收由调用方判断。
// 桌面窗口。GetShellWindow() 是系统给出的答案，不用猜类名；WorkerW 另算——装了
// 动态壁纸时桌面图标那层会变成它，它不是 shell window，但同样铺满整个桌面、同样
// 谁都不挡。
[[nodiscard]] bool IsDesktopWindow(HWND hwnd) {
    if (hwnd == nullptr) return false;
    if (hwnd == GetShellWindow()) return true;
    wchar_t name[16]{};
    if (GetClassNameW(hwnd, name, static_cast<int>(std::size(name))) == 0) return false;
    return std::wcscmp(name, L"Progman") == 0 || std::wcscmp(name, L"WorkerW") == 0;
}

[[nodiscard]] SnapshotWindow DescribeWindow(
    HWND hwnd, const std::vector<ShadowInset>& shadowInsets,
    const std::vector<std::wstring>& treatAsTopmostClasses) {
    SnapshotWindow entry;
    entry.hwnd = hwnd;
    entry.desktop = IsDesktopWindow(hwnd);
    entry.frame = FrameOf(hwnd, shadowInsets);
    entry.cloaked = IsCloaked(hwnd);
    entry.maximized = IsZoomed(hwnd) != FALSE;
    entry.minimized = IsIconic(hwnd) != FALSE;
    entry.topmost = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!treatAsTopmostClasses.empty()) {
        wchar_t probe[128]{};
        if (GetClassNameW(hwnd, probe, static_cast<int>(std::size(probe))) > 0) {
            for (const auto& name : treatAsTopmostClasses) {
                if (name == probe) {
                    entry.treatAsTopmost = true;
                    break;
                }
            }
        }
    }
    entry.owner = GetWindow(hwnd, GW_OWNER);
    return entry;
}

// 空矩形的窗口对遮挡没有贡献，也不该被描边——不收，省得下游到处判空。
// Windows Terminal 的 PseudoConsoleWindow 之类就是 0x0 但「可见」。
[[nodiscard]] bool WorthCollecting(const SnapshotWindow& entry) {
    return entry.frame.right > entry.frame.left && entry.frame.bottom > entry.frame.top;
}

// 在窗口里取五个点做命中测试：正中，加四个象限的中心。
//
// 一个点不够：中间透明、四周一圈可见框的窗口（录屏软件的选区框）正中会穿过去，四个象限
// 的点才打得到那圈框。
//
// WindowFromPoint 只给同一线程的窗口发 WM_NCHITTEST，别的进程的窗口按系统自己记着的
// 形状和透明度判断，所以对面程序卡死也不会把这里卡住。
[[nodiscard]] std::vector<ProbeHit> ProbeWindow(const DesktopSnapshot& snapshot, size_t index) {
    const SnapshotWindow& entry = snapshot.windows[index];
    const RECT& f = entry.frame;
    const LONG w = f.right - f.left;
    const LONG h = f.bottom - f.top;
    const POINT points[] = {
        {f.left + w / 2, f.top + h / 2},
        {f.left + w / 4, f.top + h / 4},
        {f.left + w * 3 / 4, f.top + h / 4},
        {f.left + w / 4, f.top + h * 3 / 4},
        {f.left + w * 3 / 4, f.top + h * 3 / 4},
    };

    std::vector<ProbeHit> hits;
    hits.reserve(std::size(points));
    for (const POINT& pt : points) {
        const HWND hit = WindowFromPoint(pt);
        const HWND root = hit != nullptr ? GetAncestor(hit, GA_ROOT) : nullptr;
        if (root == entry.hwnd) {
            hits.push_back(ProbeHit::Self);
            continue;
        }
        if (root == nullptr) {
            hits.push_back(ProbeHit::Below);
            continue;
        }
        const auto it = std::find_if(snapshot.windows.begin(), snapshot.windows.end(),
                                     [root](const SnapshotWindow& one) { return one.hwnd == root; });
        if (it != snapshot.windows.end()) {
            const auto at = static_cast<size_t>(it - snapshot.windows.begin());
            hits.push_back(at < index ? ProbeHit::Above : ProbeHit::Below);
            continue;
        }
        // 不在快照里又打得中的，最常见的是 WindowMark 自己的书签条：它接得住鼠标，而且
        // 在上面。当成「穿过去了」会把书签条压着的那个分层窗口误判成透明。
        hits.push_back(IsOwnWindow(root) ? ProbeHit::Above : ProbeHit::Below);
    }
    return hits;
}

// 给快照里的窗口标上「鼠标能不能穿过去」。
//
// 要等整份快照收齐再做：判断探测点打中的窗口在不在它上面，得知道 z 序。
//
// 只有分层窗口才需要做命中测试（跨进程穿透必须是分层窗口），普通窗口一次系统调用都不多。
void MarkPassThrough(DesktopSnapshot& snapshot) {
    for (size_t i = 0; i < snapshot.windows.size(); ++i) {
        SnapshotWindow& entry = snapshot.windows[i];
        const LONG_PTR style = GetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE);
        const bool layered = (style & WS_EX_LAYERED) != 0;
        if (!layered || entry.desktop || entry.cloaked || entry.minimized) continue;

        const bool transparent = (style & WS_EX_TRANSPARENT) != 0;
        const bool enabled = IsWindowEnabled(entry.hwnd) != FALSE;
        std::vector<ProbeHit> probes;
        if (!transparent && enabled && !entry.treatAsTopmost) probes = ProbeWindow(snapshot, i);
        entry.passThrough =
            PassesMouseThrough(layered, transparent, enabled, entry.treatAsTopmost, probes);
    }
}

} // namespace

DesktopSnapshot CaptureDesktop(const std::vector<ShadowInset>& shadowInsets,
                              const std::vector<std::wstring>& treatAsTopmostClasses) {
    DesktopSnapshot snapshot;
    snapshot.foreground = GetForegroundWindow();

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi) != FALSE) {
                reinterpret_cast<std::vector<MonitorArea>*>(param)->push_back(
                    MonitorArea{mi.rcMonitor, mi.rcWork});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&snapshot.monitors));

    HWND hwnd = GetTopWindow(nullptr);
    for (int step = 0; step < kZOrderLimit && hwnd != nullptr; ++step) {
        if (IsWindowVisible(hwnd) != FALSE && !IsOwnWindow(hwnd)) {
            const SnapshotWindow entry =
                DescribeWindow(hwnd, shadowInsets, treatAsTopmostClasses);
            if (WorthCollecting(entry)) snapshot.windows.push_back(entry);
        }
        hwnd = GetWindow(hwnd, GW_HWNDNEXT);
    }

    // 上面这趟是在**活的** z 序链表上走的：走到一半有窗口被提到链表前面，那个窗口就
    // 再也遇不上了。快速来回切两个窗口时这正在发生，实测切七八次就能让前台窗口从快照
    // 里整个消失。
    //
    // 漏掉前台是致命的：PlanBorders 的两道保险——「排在它上面的算遮挡」和「其余窗口
    // 一律再减去前台矩形」——都要在快照里按 hwnd 找到它。找不到，两道全失效，别的
    // 窗口的边框会整条画出来盖在前台上面。
    //
    // 补的位置有依据，不是猜的：前台窗口排在所有非 topmost 窗口之前，这是定义。
    if (snapshot.foreground != nullptr &&
        IsWindowVisible(snapshot.foreground) != FALSE &&
        !IsOwnWindow(snapshot.foreground)) {
        const bool present =
            std::any_of(snapshot.windows.begin(), snapshot.windows.end(),
                        [&snapshot](const SnapshotWindow& one) {
                            return one.hwnd == snapshot.foreground;
                        });
        if (!present) {
            const SnapshotWindow entry =
                DescribeWindow(snapshot.foreground, shadowInsets, treatAsTopmostClasses);
            if (WorthCollecting(entry)) {
                const auto slot =
                    std::find_if(snapshot.windows.begin(), snapshot.windows.end(),
                                 [](const SnapshotWindow& one) { return !one.topmost; });
                snapshot.windows.insert(slot, entry);
            }
        }
    }
    MarkPassThrough(snapshot);
    return snapshot;
}

bool RefreshWindowFrame(DesktopSnapshot& snapshot, HWND hwnd,
                        const std::vector<ShadowInset>& shadowInsets) {
    for (auto& entry : snapshot.windows) {
        if (entry.hwnd != hwnd) continue;
        // 只刷新会跟着移动/缩放变的那几项。cloaked、topmost、owner 改变都伴随别的
        // 事件，那些事件会让整份快照作废，轮不到这里操心。
        entry.frame = FrameOf(hwnd, shadowInsets);
        entry.maximized = IsZoomed(hwnd) != FALSE;
        entry.minimized = IsIconic(hwnd) != FALSE;
        return entry.frame.right > entry.frame.left &&
               entry.frame.bottom > entry.frame.top;
    }
    return false;
}

} // namespace windowmark::win
