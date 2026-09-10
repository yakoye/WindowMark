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
    // 这个窗口能不能当拖动目标。
    [[nodiscard]] bool CanDrag(HWND hwnd) const;
    void RestoreForDrag(POINT cursor);
    // 把窗口摆到光标所在那块屏的正中。单击（按下几乎没动就松开）走这里。
    void CenterOnCursorMonitor(POINT cursor);
    // 移动事件放行（不能吞，否则光标被钉死、位移累加不起来）。
    static LRESULT PassMoveThrough(WPARAM message, LPARAM lParam);

    // 单击和拖动的分界，像素。4 够容下按键时手上的抖动，又远小于任何有意的拖动。
    static constexpr int kTapSlop = 4;

    HHOOK mouseHook_{};
    // 只在配置勾了左/右 Win 时才装。键盘钩子比鼠标钩子更敏感——杀毒软件更关注、
    // 出错影响更大——不该让只用 Alt / Ctrl 的用户承担它。
    HHOOK keyboardHook_{};
    // 这一次拖动是靠按住 Win 键触发的，所以它的抬起要被吞掉，否则开始菜单会弹出来。
    bool winConsumed_{false};
    DragModifiers modifiers_;
    std::vector<std::string> excluded_;

    // 一次拖动的逐事件记录。拖动中只往数组里写，松手时才落盘——理由见 .cpp。
    struct TraceEntry {
        long long qpc;
        WPARAM message;
        POINT pt;
        RECT applied;
        long long setPosMicros;
        BOOL setPosOk;
    };
    void TraceReset();
    void TraceAdd(WPARAM message, POINT pt, RECT applied, long long micros, BOOL ok);
    void TraceDump(const wchar_t* why) const;

    static constexpr int kTraceCap = 512;
    TraceEntry trace_[kTraceCap]{};
    int traceCount_{0};
    bool traceOn_{false};

    bool dragging_{false};
    // 这次拖动是左键起的。右键在正中那一格也会得到「移动」，光看 edges_ 分不出来，
    // 而居中只归左键。
    bool leftButton_{false};
    // 目标是最大化的，等真的开始移动了再还原。放在按下时做的话，按住修饰键点一下
    // 最大化窗口就会把它还原并挪走——用户什么都没拖，窗口却变了。
    bool pendingRestore_{false};
    HWND target_{};
    POINT startCursor_{};
    Rect startFrame_{};
    DragEdges edges_{};
};

} // namespace windowmark::win
