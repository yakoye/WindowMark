#include "WinDragBackend.h"

#include "PinDiag.h"
#include "WinUtil.h"

#include <algorithm>

namespace windowmark::win {
namespace {

// 钩子回调不带用户参数，只能靠这个。钩子本身是进程级的全局资源，同时存在两个实例
// 本来也没有意义。
WinDragBackend* g_instance = nullptr;

[[nodiscard]] RECT WindowRectOf(HWND hwnd) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    return rect;
}

} // namespace

WinDragBackend::~WinDragBackend() { Shutdown(); }

void WinDragBackend::Apply(const DragSettings& settings) {
    modifiers_ = ParseDragModifiers(settings.modifiers);
    excluded_.clear();
    excluded_.reserve(settings.excludedAppKeys.size());
    for (const auto& key : settings.excludedAppKeys) {
        if (!key.empty()) excluded_.push_back(LowerAscii(key));
    }

    // 一个触发键都没有时不装：那样装上去也只是白白让每一个鼠标事件多绕一圈。
    const bool want = settings.enabled && !modifiers_.Empty();
    if (!want) {
        Shutdown();
        return;
    }

    // 键盘钩子只为 Win 键而装：Win 键按下再松开、中间什么都没发生时会弹出开始菜单，
    // 要压掉那一次抬起只能靠 WH_KEYBOARD_LL。配置里没勾 Win 就不装——键盘钩子比鼠标
    // 钩子更敏感（杀毒软件更关注、出错影响更大），不该让所有人默认承担。
    const bool needsKeyboard = modifiers_.Contains(0x5B) || modifiers_.Contains(0x5C);
    if (!needsKeyboard && keyboardHook_ != nullptr) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
        winConsumed_ = false;
        PinDiag(L"拖动：键盘钩子已卸（配置里没有 Win 键）");
    }

    if (mouseHook_ != nullptr) {
        // 鼠标钩子已经装着，但配置可能刚勾上 Win 键，键盘钩子还得补装。
        if (needsKeyboard && keyboardHook_ == nullptr) {
            keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WinDragBackend::KeyboardProc,
                                              GetModuleHandleW(nullptr), 0);
            PinDiag(L"拖动：键盘钩子%s（配置含 Win 键）",
                    keyboardHook_ != nullptr ? L"已装" : L"装不上");
        }
        return;
    }

    g_instance = this;
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &WinDragBackend::MouseProc,
                                   GetModuleHandleW(nullptr), 0);
    if (mouseHook_ == nullptr) {
        g_instance = nullptr;
        PinDiag(L"拖动：鼠标钩子装不上，错误 %lu", GetLastError());
        return;
    }
    PinDiag(L"拖动：鼠标钩子已装，触发键 %d 个", static_cast<int>(modifiers_.keys.size()));

    if (needsKeyboard) {
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WinDragBackend::KeyboardProc,
                                          GetModuleHandleW(nullptr), 0);
        PinDiag(L"拖动：键盘钩子%s（配置含 Win 键）",
                keyboardHook_ != nullptr ? L"已装" : L"装不上");
    }
}

void WinDragBackend::Shutdown() noexcept {
    if (keyboardHook_ != nullptr) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
        PinDiag(L"拖动：键盘钩子已卸");
    }
    winConsumed_ = false;
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
        PinDiag(L"拖动：鼠标钩子已卸");
    }
    if (g_instance == this) g_instance = nullptr;
    dragging_ = false;
    pendingRestore_ = false;
    target_ = nullptr;
}

LRESULT CALLBACK WinDragBackend::MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    // 快速路径：绝大多数时候这里什么都不该做。先看有没有正在拖动，再看修饰键——两个
    // 都是纯内存/纯寄存器判断，不查窗口、不算几何、不碰配置。
    //
    // 这是性能的全部要害：WH_MOUSE_LL 在安装它的线程上同步处理每一个鼠标事件，这里
    // 慢一点，整个系统的鼠标就跟着钝。
    if (code != HC_ACTION || g_instance == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 这两行就是空闲路径的全部。这里曾经插过一段耗时统计，量出来才发现插桩自己
    // （两次 QPC、三个原子操作、一个 CAS 循环，外加偶尔一次同步写日志）比被测的
    // 代码还贵，峰值 708µs 全是它贡献的。开销从外面比较「装/不装钩子」更准，
    // 也不用让每个用户为一次验证长期买单。见 tools/bench-drag-hook.py。
    if (!g_instance->dragging_ && !g_instance->AnyModifierDown()) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    return g_instance->HandleMouse(wParam, lParam);
}

LRESULT CALLBACK WinDragBackend::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION || g_instance == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (info == nullptr) return CallNextHookEx(nullptr, code, wParam, lParam);

    const bool isWin = info->vkCode == VK_LWIN || info->vkCode == VK_RWIN;
    const bool isUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;

    // 只吞「确实被手势用掉」的那一次抬起。winConsumed_ 在拖动真正开始、且当时按住的
    // 是 Win 键时才置位。
    //
    // 这个「只吞一次」是这段代码的全部要点：吞过头会把 Win 键整个废掉——单独按一下
    // Win 键再也弹不出开始菜单——那比不做这个功能更糟。
    if (g_instance->winConsumed_ && isWin && isUp) {
        g_instance->winConsumed_ = false;
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool WinDragBackend::WinKeyDown() {
    return (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
}

bool WinDragBackend::AnyModifierDown() const {
    // GetAsyncKeyState 的高位表示「当前按下」。对每个配置的键查一次，量级几十纳秒。
    for (const unsigned vk : modifiers_.keys) {
        if ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0) return true;
    }
    return false;
}

HWND WinDragBackend::TargetWindowAt(POINT pt) const {
    HWND hit = WindowFromPoint(pt);
    if (hit == nullptr) return nullptr;
    // WindowFromPoint 给的是鼠标底下那个子控件，要一路上溯到顶级窗口。
    HWND top = GetAncestor(hit, GA_ROOT);
    if (top == nullptr) return nullptr;
    // 桌面、任务栏这类不能拖。复用书签/边框那套资格判断，标准一致。
    if (!IsEligibleTopLevelWindow(top)) return nullptr;
    if (IsOwnProcessWindow(top)) return nullptr;
    if (IsExcluded(top)) return nullptr;
    return top;
}

bool WinDragBackend::IsExcluded(HWND hwnd) const {
    if (excluded_.empty()) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // key 的算法必须和 WinWindowBackend 算 groupKey 的那份完全一致（现在共用
    // WinUtil 里的 LowerAscii），否则排除名单会静默失效——看起来一切正常，就是不生效。
    const std::string key = LowerAscii(WideToUtf8(QueryProcessPath(pid)));
    return std::find(excluded_.begin(), excluded_.end(), key) != excluded_.end();
}

void WinDragBackend::RestoreForDrag(POINT cursor) {
    // 最大化的窗口先还原，否则 SetWindowPos 对它无效——和拖标题栏的行为一致。
    //
    // 还原之后要让窗口跟着光标走，而不是跳回它上次的还原位置：把光标在最大化窗口里的
    // 横向比例，映射到还原后的宽度上。否则手一动窗口就窜到别处，很难接着拖。
    const RECT before = WindowRectOf(target_);
    const double ratioX =
        before.right > before.left
            ? static_cast<double>(cursor.x - before.left) / (before.right - before.left)
            : 0.5;

    ShowWindow(target_, SW_RESTORE);

    const RECT after = WindowRectOf(target_);
    const int width = after.right - after.left;
    const int height = after.bottom - after.top;
    const int left = cursor.x - static_cast<int>(ratioX * width);
    // 纵向不按比例：还原后窗口通常比原来矮很多，按比例算会把标题栏送到光标上方，
    // 手感是「窗口从手里掉下去」。让光标落在标题栏高度的位置更自然。
    const int top = cursor.y - std::min(height / 2, GetSystemMetrics(SM_CYCAPTION));
    SetWindowPos(target_, nullptr, left, top, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER);

    startCursor_ = cursor;
    startFrame_ = ToCoreRect(WindowRectOf(target_));
}

void WinDragBackend::TraceReset() {
    // 有没有开诊断只在按下的那一刻查一次。每个事件都去查文件属性的话，光这一下就够
    // 让钩子变慢。
    traceOn_ = PinDiagOn();
    traceCount_ = 0;
}

void WinDragBackend::TraceAdd(WPARAM message, POINT pt, RECT applied, long long micros,
                              BOOL ok) {
    if (!traceOn_ || traceCount_ >= kTraceCap) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    trace_[traceCount_++] =
        TraceEntry{now.QuadPart, message, pt, applied, micros, ok};
}

void WinDragBackend::TraceDump(const wchar_t* why) const {
    if (!traceOn_ || traceCount_ == 0) return;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const double tick = freq.QuadPart > 0 ? 1000.0 / static_cast<double>(freq.QuadPart) : 0.0;
    const long long base = trace_[0].qpc;
    PinDiag(L"拖动跟踪（%s）：%d 条", why, traceCount_);
    for (int i = 0; i < traceCount_; ++i) {
        const TraceEntry& e = trace_[i];
        PinDiag(L"  +%7.1fms  msg=%llu  pt=(%ld,%ld)  ->(%ld,%ld,%ld,%ld)  "
                L"SetWindowPos %lldus %s",
                static_cast<double>(e.qpc - base) * tick,
                static_cast<unsigned long long>(e.message), e.pt.x, e.pt.y, e.applied.left,
                e.applied.top, e.applied.right, e.applied.bottom, e.setPosMicros,
                e.setPosOk != FALSE ? L"ok" : L"失败");
    }
}

void WinDragBackend::CenterOnCursorMonitor(POINT cursor) {
    // 按光标所在的那块屏，不是窗口现在大部分身体所在的那块。这个手势最主要的用途是
    // 把跑到屏幕外的窗口找回来，那时用户正盯着自己点的地方，窗口就该回到眼前这块屏。
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) return;

    // 用工作区而不是整块屏：任务栏占掉的那一条不该算进居中的范围。
    const Rect centered =
        CenterInWorkArea(ToCoreRect(WindowRectOf(target_)), ToCoreRect(info.rcWork));
    SetWindowPos(target_, nullptr, centered.left, centered.top, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
}

LRESULT WinDragBackend::HandleMouse(WPARAM message, LPARAM lParam) {
    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (info == nullptr) return CallNextHookEx(nullptr, HC_ACTION, message, lParam);

    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        if (dragging_) break;
        HWND target = TargetWindowAt(info->pt);
        if (target == nullptr) break;

        target_ = target;
        startCursor_ = info->pt;
        startFrame_ = ToCoreRect(WindowRectOf(target));
        // 左键整窗移动，右键按九宫格决定拉哪几条边。
        leftButton_ = message == WM_LBUTTONDOWN;
        edges_ = leftButton_ ? DragEdges{true, true, true, true}
                             : HitZone(startFrame_, info->pt.x, info->pt.y);
        dragging_ = true;
        TraceReset();
        TraceAdd(message, info->pt, ToWinRect(startFrame_), 0, TRUE);
        // 这次拖动是按着 Win 键触发的话，它的抬起就归这次手势了。只有装了键盘钩子
        // 才谈得上吞——没装的话置位也没人读。
        if (keyboardHook_ != nullptr && WinKeyDown()) winConsumed_ = true;
        pendingRestore_ = IsZoomed(target_) != FALSE;
        return 1;   // 吞掉：这一次按下已经被手势用掉了
    }

    case WM_MOUSEMOVE: {
        if (!dragging_) break;
        if (pendingRestore_) {
            RestoreForDrag(info->pt);
            pendingRestore_ = false;
        }
        const Rect next = ApplyDrag(startFrame_, edges_, info->pt.x - startCursor_.x,
                                    info->pt.y - startCursor_.y,
                                    GetSystemMetrics(SM_CXMINTRACK),
                                    GetSystemMetrics(SM_CYMINTRACK));
        const RECT r = ToWinRect(next);
        // SWP_NOACTIVATE：拖背景窗口不该抢焦点，这正是这个手势好用的地方。
        // 诊断关着的时候（默认如此）这里只有一次 SetWindowPos，一个多余的时钟调用都没有。
        if (!traceOn_) {
            SetWindowPos(target_, nullptr, r.left, r.top, r.right - r.left,
                         r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
            return 1;
        }
        LARGE_INTEGER t0{};
        LARGE_INTEGER t1{};
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const BOOL ok =
            SetWindowPos(target_, nullptr, r.left, r.top, r.right - r.left,
                         r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
        QueryPerformanceCounter(&t1);
        const long long micros =
            freq.QuadPart > 0 ? (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart : 0;
        TraceAdd(message, info->pt, r, micros, ok);
        return 1;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        if (!dragging_) break;
        // 按下到松开几乎没动 = 单击，把窗口居中。这一次点击本来就已经被按下那一步吞掉了，
        // 应用永远收不到它——不接这个手势，它就是白白浪费的一个事件。
        //
        // 只认左键：右键的单击也被吞了，但右键管缩放，让它改位置会很意外。
        // 最大化的窗口不动：它已经占满屏幕，居中没有意义。
        if (leftButton_ && target_ != nullptr && IsZoomed(target_) == FALSE &&
            IsTap(info->pt.x - startCursor_.x, info->pt.y - startCursor_.y, kTapSlop)) {
            CenterOnCursorMonitor(info->pt);
            TraceAdd(message, info->pt, WindowRectOf(target_), 0, TRUE);
        }
        TraceAdd(message, info->pt, RECT{}, 0, TRUE);
        TraceDump(L"松开");
        traceOn_ = false;
        dragging_ = false;
        pendingRestore_ = false;
        target_ = nullptr;
        // 两个键的抬起都要吞，因为对应的按下已经被吞掉了——放行一个没有配对按下的
        // 抬起，应用的状态机会错乱。右键这一次尤其关键：放行它，应用会当成一次完整
        // 右击并弹出上下文菜单。
        return 1;
    }

    default:
        break;
    }
    return CallNextHookEx(nullptr, HC_ACTION, message, lParam);
}

} // namespace windowmark::win
