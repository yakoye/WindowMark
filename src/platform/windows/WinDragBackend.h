#pragma once

#include "windowmark/core/DragGeometry.h"
#include "windowmark/core/DragModifiers.h"
#include "windowmark/core/Settings.h"

#include <windows.h>

#include <string>
#include <vector>

namespace windowmark::win {

// 按住修饰键拖动窗口：左键拖=移动，右键拖=按九宫格缩放。
//
// 全局低级鼠标钩子（WH_MOUSE_LL）。钩子是进程级的全局资源，回调又不带用户参数，所以
// 这个类是单实例的——文件级指针把回调转发到实例，和项目里其他 Win32 回调同一个套路。
//
// **性能是这个类的第一约束。** WH_MOUSE_LL 在安装它的线程上同步处理**每一个**鼠标事件，
// 这里慢一点整个系统的鼠标就跟着钝。所以钩子回调的第一件事必须是最便宜的判断，没在
// 拖动也没按修饰键就立刻放行，不查窗口、不算几何、不碰配置。
class WinDragBackend {
public:
    WinDragBackend() = default;
    ~WinDragBackend();

    WinDragBackend(const WinDragBackend&) = delete;
    WinDragBackend& operator=(const WinDragBackend&) = delete;

    // 按配置装/卸钩子。功能关闭、或者一个触发键都没配时不装——不装钩子就是零开销，
    // 比装上再在回调里判断干净。
    void Apply(const DragSettings& settings);

    // 卸掉钩子。退出和「暂停所有」都走这里。
    void Shutdown() noexcept;

private:
    static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);
    // 只为 Win 键而存在，见 .cpp 里的说明。
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] LRESULT HandleMouse(WPARAM message, LPARAM lParam);
    [[nodiscard]] bool AnyModifierDown() const;
    [[nodiscard]] static bool WinKeyDown();
    [[nodiscard]] HWND TargetWindowAt(POINT pt) const;
    [[nodiscard]] bool IsExcluded(HWND hwnd) const;
    void RestoreForDrag(POINT cursor);

    HHOOK mouseHook_{};
    // 只在配置勾了左/右 Win 时才装。键盘钩子比鼠标钩子更敏感——杀毒软件更关注、
    // 出错影响更大——不该让只用 Alt / Ctrl 的用户承担它。
    HHOOK keyboardHook_{};
    // 这一次拖动是靠按住 Win 键触发的，所以它的抬起要被吞掉，否则开始菜单会弹出来。
    bool winConsumed_{false};
    DragModifiers modifiers_;
    std::vector<std::string> excluded_;

    bool dragging_{false};
    HWND target_{};
    POINT startCursor_{};
    Rect startFrame_{};
    DragEdges edges_{};
};

} // namespace windowmark::win
