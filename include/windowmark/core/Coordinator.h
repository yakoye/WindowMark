#pragma once

#include "windowmark/core/Interfaces.h"
#include "windowmark/core/PinRegistry.h"
#include "windowmark/core/Settings.h"
#include "windowmark/core/Types.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace windowmark {

class Coordinator {
public:
    Coordinator(
        Settings settings,
        IWindowBackend& windowBackend,
        IOverlayBackend& overlayBackend,
        IPreviewBackend& previewBackend,
        IBorderBackend* borderBackend = nullptr,
        IPinBackend* pinBackend = nullptr);

    bool Start();
    void Stop() noexcept;
    // Backed by the setting rather than a separate runtime flag, so the tray toggle and
    // the settings checkbox cannot disagree and the choice survives a restart.
    void SetOverlayEnabled(bool enabled);
    [[nodiscard]] bool OverlayEnabled() const noexcept { return settings_.drawer.enabled; }

    // Pinning. Every entry point in the platform layer routes here, so there is exactly
    // one place that decides whether a window is pinned.
    void TogglePin(WindowId id);
    void UnpinAll();
    [[nodiscard]] std::vector<PinRecord> PinnedWindows() const { return pins_.Snapshot(); }
    [[nodiscard]] bool IsPinned(WindowId id) const { return pins_.Contains(id); }
    // Whether this window is one of the ones being tracked at all. Pinning refuses windows
    // that are not, so this is the first thing to check when a pin appears to do nothing.
    [[nodiscard]] bool IsTracked(WindowId id) const { return windows_.contains(id); }
    // 用户最后在用的那个窗口——**只认跟踪得到的窗口**。
    //
    // 不是 GetForegroundWindow，也不是「最后一条前台事件」：点任务栏、点托盘图标、打开
    // WindowMark 自己的面板或设置窗口，前台确实都变了，但用户在用的那个窗口没变。照着前台
    // 走的话，这些时候没有任何一个窗口匹配得上「活动」，屏幕上一个活动边框都没有，书签条
    // 也跟着消失（drawer.active_window_only）——用户 2026-10-02 报的正是这个。
    //
    // 面板上的「置顶刚才那个窗口」也是靠它：菜单项做不到这件事（菜单弹出时前台已经是我们
    // 自己，这正是准星当初存在的理由）。
    [[nodiscard]] WindowId ActiveWindow() const noexcept { return activeWindow_; }
    // 置顶预览：画得和真的置顶一模一样（同色同宽），因为要回答的正是「松手之后长什么样」。
    // 传 0 清掉。
    void SetPinPreview(WindowId id);
    // Title for the tray submenu. Empty when the window is no longer tracked.
    [[nodiscard]] std::string PinnedTitle(WindowId id) const;

    // Selection is intentionally exposed as platform-neutral data so each OS can
    // build its own native settings UI without pulling platform types into Core.
    [[nodiscard]] std::vector<AppSelectionModel> SelectionSnapshot() const;
    void ApplySelection(const std::vector<AppSelectionModel>& selection);
    // The same pair again for borders. Two lists rather than one shared one: an app can be
    // worth a bookmark and not worth an outline, and the reverse.
    [[nodiscard]] std::vector<AppSelectionModel> BorderSelectionSnapshot() const;
    void ApplyBorderSelection(const std::vector<AppSelectionModel>& selection);

    // 拖动的排除名单。只有 app 级粒度：拖动按「鼠标底下那个窗口属于哪个应用」判断，
    // 没有「同一个应用的这个窗口能拖、那个不能」这种说法。窗口那一层照样填出来，
    // 但都跟着 app 走——选择面板因此不用为它长一套单独的界面。
    [[nodiscard]] std::vector<AppSelectionModel> DragSelectionSnapshot() const;
    void ApplyDragSelection(const std::vector<AppSelectionModel>& selection);
    [[nodiscard]] const Settings& CurrentSettings() const noexcept { return settings_; }

    // Pushes edited settings to the backends and redraws, so the settings UI does not
    // have to restart anything.
    void UpdateSettings(Settings settings);

    // Custom bookmark labels. Deliberately session-only, for the same reason per-window
    // selection is: a generic OS window has no reliable cross-session identity, so a
    // persisted name would eventually attach itself to the wrong window.
    void SetCustomLabel(WindowId id, std::string label);
    [[nodiscard]] std::string CustomLabel(WindowId id) const;
    [[nodiscard]] std::string DefaultLabel(WindowId id) const;

    // Actions raised from a bookmark's context menu. The platform layer owns the input
    // and settings UI; set these before Start().
    void SetMenuHandlers(std::function<void(WindowId)> onRename, std::function<void()> onOpenSettings);

    // 看门狗。平台层每隔几秒调一次。
    //
    // 要防的是一类从外面看一模一样的故障：窗口变了，程序毫无反应，重启才好（用户 9/17 和
    // 9/21 各报过一次）。原因可能是 Windows 把太慢的钩子摘了、explorer 重启、会话切换之后
    // 事件流断了，也可能是边框挂起之后再没收到解除挂起的那条会话事件。共同点是**程序自己
    // 以为一切正常**——它的状态不会变，所以没有任何一处会发现不对。
    //
    // 判据不看钩子（WinEvent 钩子没有「还活着吗」这种查法），而是对账：现在系统上真实存在
    // 的窗口和自己记着的那份对不上，就说明漏事件了。连续两次都对不上才算——一次可能只是
    // 事件还在队列里，而两次之间隔着好几秒，那就不是时序问题了。
    void WatchdogTick();
    // 看门狗一共恢复过几次。诊断用，也是测试能看的唯一结果。
    [[nodiscard]] unsigned WatchdogRecoveries() const noexcept { return watchdogRecoveries_; }

private:
    void OnWindowEvent(const WindowEvent& event);
    void RefreshAll();
    void RefreshOne(WindowId id);
    // Cheap path for location events: updates the frame and nothing else.
    void RefreshGeometry(WindowId id);
    void ApplyModels();
    void ApplyBorders();
    void ApplyPins();
    // Drops pins for windows that are gone, restoring nothing - they took their state
    // with them.
    void PrunePins();
    [[nodiscard]] std::vector<OverlayModel> BuildModels();
    // DockMaxGrowth 要把鼠标位置扫一遍，宿主每挪一下 BuildModels 都要用到它，所以按
    // （是否侧边、标签个数、激活下标）记下来。只和设置有关，设置一变就清空。
    [[nodiscard]] float DockGrowthFor(bool side, std::size_t count, int activeIndex);
    // Borders cover every tracked top-level window, with no grouping: a single-window app
    // gets a border even though it never gets a bookmark strip.
    [[nodiscard]] std::vector<BorderModel> BuildBorderModels() const;
    [[nodiscard]] Color ColorFor(WindowId id);
    [[nodiscard]] bool IsAppEnabled(const std::string& groupKey) const;
    [[nodiscard]] bool IsBorderAppEnabled(const std::string& groupKey) const;
    [[nodiscard]] bool IsBorderWindowEnabled(WindowId id) const;
    [[nodiscard]] bool IsWindowEnabled(WindowId id) const;
    void PruneTransientState();

    Settings settings_;
    IWindowBackend& windowsBackend_;
    IOverlayBackend& overlaysBackend_;
    IPreviewBackend& previewBackend_;
    IBorderBackend* borderBackend_{};
    IPinBackend* pinBackend_{};

    std::unordered_map<WindowId, WindowInfo> windows_;
    std::unordered_map<WindowId, std::size_t> stableOrder_;
    std::unordered_map<WindowId, std::size_t> colorSlots_;
    std::unordered_set<WindowId> disabledWindowIds_;
    // Session-only, like its bookmark counterpart: an HWND is not the same window after a
    // restart, so persisting one would silence whatever inherited the number.
    std::unordered_set<WindowId> borderDisabledWindowIds_;
    std::unordered_map<WindowId, std::string> customLabels_;
    std::map<std::tuple<bool, std::size_t, int>, float> dockGrowth_;
    PinRegistry pins_;
    std::function<void(WindowId)> onRename_;
    std::function<void()> onOpenSettings_;
    std::size_t nextStableOrder_{0};
    std::size_t nextColorSlot_{0};
    // 上一次看门狗对账时发现对不上的窗口。两次都在里面才算真漏了。
    std::unordered_set<WindowId> watchdogSuspects_;
    unsigned watchdogRecoveries_{0};
    // 见 ActiveWindow：只在确实跟踪得到的窗口上更新。
    WindowId activeWindow_{0};
    WindowId pinPreview_{0};
    bool started_{false};
};

} // namespace windowmark
