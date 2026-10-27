#pragma once

#include "windowmark/core/Hotkey.h"
#include "windowmark/core/Types.h"

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace windowmark::win {

class WinControlWindow {
public:
    ~WinControlWindow();

    struct Handlers {
        std::function<void()> onToggleImeGuard;
        std::function<void()> onRestoreImeGuard;
        std::function<void()> onCheckImeGuard;
        std::function<void()> onImeGuardDiagnostics;
        std::function<void()> onToggleImeHints;
        // 菜单每次弹出前调用：把四个功能的开关状态从当前配置重新读一遍（调 Set*State）。
        // 菜单上的对勾以前是各自缓存的，其中「书签」那份启动时从没读过配置、改完设置也不
        // 更新，于是出现「打着勾却没开」。每次开菜单现读，对勾就不可能和实际不符。
        std::function<void()> onMenuOpening;
        // Master switch: turns every feature off, or every one back on.
        std::function<void()> onToggleAll;
        std::function<void()> onToggleBookmarks;
        std::function<void()> onSelection;
        std::function<void()> onBookmarkSettings;
        std::function<void()> onToggleBorders;
        std::function<void()> onBorderSettings;
        // 窗口边框 -> 排除应用. Lives in the menu next to the on/off switch rather than
        // inside the settings window, matching where 书签 keeps its own app picker: both
        // answer "which windows does this feature apply to", which is a different question
        // from the numbers on the settings page.
        std::function<void()> onBorderExcludeApps;
        std::function<void()> onTogglePinning;
        // 窗口拖动。三项和边框那组同构：开关、排除应用、设置。
        std::function<void()> onToggleDrag;
        std::function<void()> onDragSettings;
        std::function<void()> onDragExcludeApps;
        std::function<void(WindowId)> onTogglePinWindow;
        // Crosshair grab. Preview fires as the cursor crosses windows, commit when the
        // user settles on one, cancel on right-click or if capture is taken away.
        //
        // 准星有两个用处（置顶、加入分组），预览和取消两条是共用的；提交按用处分成两条，
        // 因为它们要做的事不一样。
        std::function<void(WindowId)> onGrabPreview;
        std::function<void(WindowId)> onGrabCommit;
        std::function<void()> onGrabCancel;
        // 准星抓来加入分组。groupId == 0 表示新建一个组；返回它最终所在的组 id，托盘据此
        // 查出组名和人数弹气泡——组里只有一个窗口时什么都不会变（不足两个不生效），不说
        // 一声用户就会以为点了没用。
        std::function<int(WindowId, int)> onGrabJoinGroup;
        // 列出现有的手动分组，给「抓取窗口加入分组」那个子菜单用。
        std::function<std::vector<ManualGroupInfo>()> manualGroups;
        // 某个窗口现在在哪个组里（0 = 不在）。只为了把「它已经在这个组里了」和「刚加进去」
        // 分开说——两种情况屏幕上都没动静，混成一句话就等于骗人。
        std::function<int(WindowId)> manualGroupOf;
        std::function<void()> onDisbandAllGroups;
        // Whether a window can be grabbed at all. Without this the crosshair would commit
        // whatever WindowFromPoint returned, and anything that appears under the cursor
        // mid-aim - a tooltip, a floating toolbar, the taskbar sliding up - silently
        // becomes the target and the action does nothing.
        std::function<bool(WindowId)> canGrab;
        std::function<void()> onUnpinAll;
        // The global shortcut. Unlike the tray menu this does not steal the foreground,
        // so the window the user is looking at is still the foreground window when it
        // fires - which is the whole reason a shortcut can pin "the current window" and
        // a menu item cannot.
        std::function<void()> onPinHotkey;
        // 书签条的切换快捷键：跳到当前这一组的第 N 个窗口（index 从 1 数），
        // 以及在最近两个窗口之间来回。
        std::function<void(int)> onSwitchTo;
        std::function<void()> onSwitchPrevious;
        std::function<void()> onPinSettings;
        // 剪贴板守护。开关的是 ClipKeeper 的面板而不是进程——它是独立 exe，有自己的
        // 托盘图标，停止守护在它自己的面板里做。
        std::function<void()> onClipKeeper;
        // 配置文件位置。跟开机启动一样是程序级设置，所以放顶层而不是某个功能的设置页里。
        std::function<void()> onConfigPath;
        // 主面板：托盘左键单击，以及菜单里的「面板...」。右键以前是唯一的入口。
        std::function<void()> onShowPanel;
        // 置顶「刚才那个窗口」。面板上那个按钮用，没有对应的菜单项。
        std::function<void()> onPinLastWindow;
        // 跑一次诊断工具（tools\WindowMarkDiag.exe），它自己把报告存到桌面并复制到剪贴板。
        std::function<void()> onDiagnose;
        // 在桌面上建一个快捷方式。绿色版用得上：解压到哪儿都能在桌面开。
        std::function<void()> onDesktopShortcut;
        // 安装到系统 / 卸载。菜单里同时只出现一个，看当前这份跑在哪儿
        // （SetRunningFromInstallDir）。
        std::function<void()> onInstall;
        std::function<void()> onUninstall;
        std::function<void()> onAbout;
        std::function<void()> onExit;
    };

    bool Start(Handlers handlers);
    void Stop() noexcept;
    // 托盘气泡。标题和正文各有长度上限（szInfoTitle 64、szInfo 256），所以这里统一截断，
    // 不让每个调用点自己小心。
    void ShowBalloon(const wchar_t* title, const std::wstring& text);
    // 决定菜单里出现「安装到系统...」还是「卸载 WindowMark...」。绿色版（解压目录里跑的）
    // 是前者，安装目录里跑的是后者。
    void SetRunningFromInstallDir(bool installed) noexcept { runningInstalled_ = installed; }
    void SetEnabledState(bool enabled);
    void SetBorderState(bool enabled);
    void SetDragState(bool enabled);
    void SetPinState(bool enabled);
    void SetImeGuardState(bool requested, bool active, std::wstring status, bool hints) {
        imeRequested_ = requested; imeActive_ = active; imeStatus_ = std::move(status);imeHints_ = hints;
    }
    // Registers `hotkey`, replacing whatever was registered before. An empty Hotkey just
    // unregisters. Returns false when Windows refused the combination - always because
    // another process already owns it, since RegisterHotKey is first-come, first-served
    // for the whole session. The caller is expected to say so out loud: a shortcut that
    // silently does nothing is worse than not offering one.
    bool SetPinHotkey(const Hotkey& hotkey);
    // 注册 / 注销整组切换快捷键。mods 是 MOD_* 的组合，0 表示关掉。
    // 返回没能注册上的那几个键的说明（空＝全部成功）——全局热键先到先得，被别人占了
    // 必须说出来，默默不响是这类功能最常见的「看上去坏了」。
    std::wstring SetSwitchHotkeys(UINT mods);
    [[nodiscard]] Hotkey CurrentPinHotkey() const noexcept { return pinHotkey_; }
    // The pinned list is read when the menu opens rather than pushed on every change:
    // it changes far more often than the menu is looked at, and a stale copy here would
    // be a second source of truth for something the Coordinator already owns.
    using PinnedProvider = std::function<std::vector<std::pair<WindowId, std::wstring>>()>;
    void SetPinnedProvider(PinnedProvider provider);
    [[nodiscard]] HWND NativeHandle() const noexcept { return hwnd_; }

    // 命令 id 是常量，公开出来让主面板能把动作交回这里执行（RunCommand）：面板和菜单
    // 因此走同一条分发路径，不可能一边对一边错。
    static constexpr UINT kToggleCommand = 1001;
    static constexpr UINT kToggleImeGuardCommand = 1030;
    static constexpr UINT kRestoreImeGuardCommand = 1031;
    static constexpr UINT kCheckImeGuardCommand = 1032;
    static constexpr UINT kImeGuardDiagnosticsCommand = 1033;
    static constexpr UINT kToggleImeHintsCommand = 1034;
    static constexpr UINT kSelectionCommand = 1002;
    static constexpr UINT kSettingsCommand = 1003;
    static constexpr UINT kAboutCommand = 1004;
    static constexpr UINT kExitCommand = 1005;
    static constexpr UINT kToggleBordersCommand = 1006;
    static constexpr UINT kBorderSettingsCommand = 1007;
    static constexpr UINT kBorderExcludeCommand = 1015;
    static constexpr UINT kConfigPathCommand = 1016;
    static constexpr UINT kClipKeeperCommand = 1017;
    static constexpr UINT kAutoStartCommand = 1008;
    static constexpr UINT kToggleAllCommand = 1009;
    static constexpr UINT kTogglePinningCommand = 1010;
    static constexpr UINT kGrabToPinCommand = 1011;
    static constexpr UINT kPinSettingsCommand = 1013;
    static constexpr UINT kUnpinAllCommand = 1014;
    static constexpr UINT kToggleDragCommand = 1018;
    static constexpr UINT kDragSettingsCommand = 1019;
    static constexpr UINT kDesktopShortcutCommand = 1021;
    static constexpr UINT kInstallCommand = 1022;
    static constexpr UINT kUninstallCommand = 1023;
    static constexpr UINT kPanelCommand = 1024;
    static constexpr UINT kPinLastWindowCommand = 1025;
    static constexpr UINT kDiagnoseCommand = 1026;
    static constexpr UINT kDragExcludeCommand = 1020;
    // 抓取窗口加入分组：这一条是「新建分组」，已有的组在下面的动态块里一组一个。
    static constexpr UINT kGrabToGroupCommand = 1027;
    static constexpr UINT kDisbandAllGroupsCommand = 1028;

    // 交回托盘窗口执行一条命令。
    //   RunCommand     投递，当前这层窗口（面板）关掉之后才执行——要开新窗口的动作用它，
    //                  避免模态套模态。
    //   RunCommandNow  同步执行，面板还开着——开关类用它，这样面板能立刻刷新自己的勾。
    void RunCommand(UINT command) const noexcept {
        if (hwnd_) PostMessageW(hwnd_, WM_COMMAND, command, 0);
    }
    void RunCommandNow(UINT command) const noexcept {
        if (hwnd_) SendMessageW(hwnd_, WM_COMMAND, command, 0);
    }

private:
    static constexpr UINT kTrayMessage = WM_APP + 70;
    // Dynamic block: one command per currently pinned window, allocated when the menu is
    // built. Kept well clear of the fixed ids above so adding a fixed item never collides.
    static constexpr UINT kPinnedWindowCommandBase = 1100;
    static constexpr UINT kPinnedWindowCommandLimit = 64;
    // 同理：一条命令对应一个现有的手动分组，开菜单时分配。
    static constexpr UINT kGroupCommandBase = 1200;
    static constexpr UINT kGroupCommandLimit = 32;
    // Safety net for the crosshair grab. While it is active this window holds the mouse
    // capture, so if it ever failed to end the user would be left clicking into nothing
    // with no obvious way out. Fifteen seconds is far longer than aiming at a window takes
    // and far shorter than anyone would spend wondering what broke.
    // Only one hotkey, so a fixed id is enough. Ids are per-window, not global.
    static constexpr int kPinHotkeyId = 1;
    // 切换快捷键占 20..29：20+n 是数字键 n（1..9），29 是反引号。离 kPinHotkeyId 远一点，
    // 以后再加固定项也撞不上。
    static constexpr int kSwitchHotkeyBase = 20;
    static constexpr int kSwitchPreviousId = 29;
    static constexpr UINT_PTR kGrabTimeoutTimer = 90;
    static constexpr UINT kGrabTimeoutMs = 15000;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void ShowMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowAlreadyRunningHint();

    // Crosshair grab, Spy++ style: the menu puts up a draggable crosshair, the user drags
    // it onto a window, and whatever it is released over gets pinned.
    //
    // One way in, on purpose. Dragging off the tray icon was tried and dropped: the icon
    // is often folded into the overflow flyout, and starting a drag there meant an
    // ordinary left click on the icon had to be told apart from the beginning of a
    // gesture. The handle also fixes the real defect - it is a visible window of ours, so
    // pressing on it makes this process foreground and hands it the mouse, and SetCapture
    // cannot fail. The old menu path called SetForegroundWindow on the hidden 0x0 control
    // window instead, which a background process is allowed to lose.
    enum class GrabState { None, Grabbing };
    // 这次准星抓来干什么。整套取窗口的交互是共用的，只有松手那一下不一样。
    enum class GrabPurpose { Pin, JoinGroup };

    void BeginGrabFromMenu(GrabPurpose purpose, int groupId);
    void ShowGrabHandle();
    void DestroyGrabHandle() noexcept;
    void StartGrabFromHandle();
    static LRESULT CALLBACK GrabHandleProc(HWND, UINT, WPARAM, LPARAM);
    void PaintGrabHandle(HWND) const;
    void EndGrab(bool commit);
    // 松手落在分组上：加进去，然后弹气泡说清结果。
    void CommitGrabToGroup(WindowId target);
    void UpdateGrabTarget();
    [[nodiscard]] WindowId WindowUnderCursor() const;

    GrabState grabState_{GrabState::None};
    GrabPurpose grabPurpose_{GrabPurpose::Pin};
    // 加入哪个组；0 = 新建。菜单项的序号只在开菜单那一刻那份列表里有意义，所以在那儿就
    // 解析成组 id 存下来。
    int grabGroupId_{0};
    WindowId grabTarget_{};
    HCURSOR grabCursor_{};
    // The draggable crosshair. Only exists between the menu item and the release.
    HWND grabHandle_{};
    // Whichever window holds the mouse capture for the grab in progress. Stored rather
    // than assumed, so releasing it cannot get out of step with taking it.
    HWND grabCapture_{};

    HWND hwnd_{};
    UINT requestQuitMessage_{};
    UINT secondInstanceMessage_{};
    // explorer 重启后系统广播的「任务栏建好了」。不接这条，图标就永久没了。
    UINT taskbarCreatedMessage_{};
    Handlers handlers_;
    bool enabled_{true};
    bool imeRequested_{}, imeActive_{};
    bool imeHints_{true};
    std::wstring imeStatus_{L"已关闭"};
    bool runningInstalled_{false};
    bool bordersEnabled_{false};
    bool dragEnabled_{false};
    bool pinningEnabled_{true};
    PinnedProvider pinnedProvider_;
    // What is currently registered, so a settings change can unregister the old one before
    // claiming the new one. Empty means nothing is registered.
    Hotkey pinHotkey_{};
    // 当前注册着的那组切换键的修饰键；0＝没注册。
    UINT switchMods_{0};
    // Index -> WindowId for the dynamic block, rebuilt every time the menu opens.
    std::vector<WindowId> pinnedMenuWindows_;
    // 开菜单时按顺序记下的手动分组 id，和 kGroupCommandBase 的偏移一一对应。
    std::vector<int> groupMenuIds_;
};

} // namespace windowmark::win
