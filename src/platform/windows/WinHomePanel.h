#pragma once

// 托盘左键点出来的主面板。
//
// 托盘菜单对会用的人够快，对第一次见它的人不行：四个功能叫什么、各自干什么、怎么用、现在开
// 着没开，全藏在三层子菜单里，而菜单看不下任何一句说明。面板把这些摊开在一个窗口里，顺便做
// 所有设置的入口。
//
// 它不自己干活：按钮按下去就把对应的托盘命令交回托盘窗口执行（RunCommand），菜单和面板因此
// 走的是同一条路，不可能一边对一边错。只有「开 / 关某个功能」是当场切、当场刷新，因为那几条
// 不开窗口，面板留着更顺手。
//
// 要开另一个窗口的动作一律先关面板再执行：模态套模态在这套代码里本来就被 dialogOpen 挡着，
// 而且「点了设置，面板让位给设置窗口」本身就是用户预期的样子。想回面板再左键点一下图标。

#include "windowmark/core/Settings.h"

#include <windows.h>

#include <functional>
#include <string>

namespace windowmark::win {

// 面板关掉时要执行的动作。None = 用户只是关掉了面板。
enum class HomeAction {
    None,
    BookmarkSettings,
    BookmarkApps,
    BorderSettings,
    BorderApps,
    PinSettings,
    PinGrab,
    DragSettings,
    DragApps,
    PinLastWindow,
    ConfigPath,
    DesktopShortcut,
    ClipKeeper,
    Diagnose,
    Install,
    Uninstall,
    About,
    Exit,
};

enum class HomeFeature { Bookmarks, Borders, Pinning, Drag };

struct HomePanelContext {
    // 每次打开都现读，不传快照：开关可能刚在托盘菜单里被改过。
    std::function<Settings()> settings;
    // 开 / 关某个功能。面板调完会重新读一遍 settings 刷新自己。
    std::function<void(HomeFeature)> toggleFeature;
    // 开机启动的真实状态在注册表里（而且用户可能在任务管理器里关掉过），所以也是现读。
    std::function<bool()> autoStartEnabled;
    std::function<void()> toggleAutoStart;
    // 「置顶刚才那个窗口」按钮上要写的标题；空 = 当前没有可置顶的窗口，按钮灰着。
    std::function<std::wstring()> lastWindowTitle;
    std::function<bool()> lastWindowPinned;
    // 决定出现「安装到系统」还是「卸载」。
    bool runningFromInstallDir{false};
};

class WinHomePanel {
public:
    static HomeAction ShowModal(HWND owner, const HomePanelContext& context);
};

} // namespace windowmark::win
