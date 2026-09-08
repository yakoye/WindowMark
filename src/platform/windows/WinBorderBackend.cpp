#include "WinBorderBackend.h"

#include "PinDiag.h"
#include "WinBorderPlan.h"
#include "WinDesktopSnapshot.h"

#include <string>

namespace windowmark::win {
namespace {

// 拖动时 Redraw 每秒被调几百次。哪一段吃掉的时间最多，光看代码猜不出来。
struct RedrawTrace {
    bool on{false};
    ULONGLONG checkedAt{};
    ULONGLONG since{};
    int calls{};
    int skipped{};
    int fromMove{};
    int fullSnapshots{};
    double sync{};
    double snap{};
    double plan{};
    double render{};
};
RedrawTrace g_trace;

// 最近一次用来规划的快照，只给诊断报告读。
DesktopSnapshot g_lastSnapshot;

[[nodiscard]] std::vector<std::wstring> ToWideList(const std::vector<std::string>& items) {
    std::vector<std::wstring> out;
    out.reserve(items.size());
    for (const auto& one : items) {
        if (!one.empty()) out.push_back(Utf8ToWide(one));
    }
    return out;
}

[[nodiscard]] LONGLONG Ticks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

[[nodiscard]] double MsSince(LONGLONG start) {
    static const double perMs = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(freq.QuadPart) / 1000.0;
    }();
    return static_cast<double>(Ticks() - start) / perMs;
}

// 每秒问一次诊断开关够了。PinDiagOn() 每次都查文件存在与否，放在每帧都走的路上，
// 那一个系统调用本身就比要测的东西还贵。
[[nodiscard]] bool TraceOn() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_trace.checkedAt >= 1000) {
        g_trace.checkedAt = now;
        g_trace.on = PinDiagOn();
    }
    return g_trace.on;
}

void ReportIfDue() {
    const ULONGLONG now = GetTickCount64();
    if (g_trace.since == 0) {
        g_trace.since = now;
        return;
    }
    if (now - g_trace.since < 1000) return;
    const double secs = static_cast<double>(now - g_trace.since) / 1000.0;
    const double total = g_trace.sync + g_trace.snap + g_trace.plan + g_trace.render;
    const RenderTrace render = TakeRenderTrace();
    PinDiag(L"Redraw %d 次 / %.1fs（%.0f 次每秒），几何事件触发 %d，内容没变跳过 %d，"
            L"整取快照 %d 次"
            L" | 各段累计 ms：sync %.1f  snapshot %.1f  plan %.1f  render %.1f"
            L" | 合计 %.1f ms，占这一秒的 %.0f%%",
            g_trace.calls, secs, g_trace.calls / secs, g_trace.fromMove, g_trace.skipped,
            g_trace.fullSnapshots,
            g_trace.sync, g_trace.snap, g_trace.plan, g_trace.render,
            total, 100.0 * total / (secs * 1000.0));
    // 快照摘要：前台是谁、它在 z 序里排第几、上面压着谁。
    //
    // 「边框穿过了排在它上面的窗口」这类问题，光看系统此刻的 z 序判断不了——里面用的
    // 是快照，而快照有缓存。两份对不上才是根因。
    {
        const auto name = [](HWND hwnd) {
            static wchar_t buffer[64];
            buffer[0] = L'\0';
            if (hwnd != nullptr) GetClassNameW(hwnd, buffer, 64);
            return buffer;
        };
        std::wstring head;
        int index = 0;
        int foregroundIndex = -1;
        for (const auto& entry : g_lastSnapshot.windows) {
            if (entry.hwnd == g_lastSnapshot.foreground) foregroundIndex = index;
            if (index < 8) {
                head += L"  ";
                head += name(entry.hwnd);
                if (entry.hwnd == g_lastSnapshot.foreground) head += L"[前台]";
                if (entry.cloaked) head += L"[cloaked]";
                if (entry.minimized) head += L"[最小化]";
            }
            ++index;
        }
        PinDiag(L"  快照：共 %d 个窗口，前台是 %s，在快照里排第 %d%s | 头几个：%s",
                static_cast<int>(g_lastSnapshot.windows.size()),
                name(g_lastSnapshot.foreground), foregroundIndex,
                foregroundIndex < 0 ? L"（**不在快照里，减去前台矩形那道保险失效**）" : L"",
                head.c_str());
    }
    PinDiag(L"  render 拆开：%d 帧（每屏各算一帧），圆角段 %d 个"
            L" | 填像素 %.1f ms  画弧 %.1f ms  提交 %.1f ms"
            L" | 圆角共算了 %.2f 百万像素 | 脏区共 %.1f 百万像素，平均每帧 %.2f",
            render.frames, render.arcSegments, render.fillMs, render.arcMs,
            render.commitMs, render.arcPixels / 1000000.0, render.dirtyMegapixels,
            render.frames > 0 ? render.dirtyMegapixels / render.frames : 0.0);
    g_trace.since = now;
    g_trace.calls = 0;
    g_trace.skipped = 0;
    g_trace.fromMove = 0;
    g_trace.fullSnapshots = 0;
    g_trace.sync = 0;
    g_trace.snap = 0;
    g_trace.plan = 0;
    g_trace.render = 0;
}

} // namespace

WinBorderBackend::~WinBorderBackend() { Stop(); }

bool WinBorderBackend::Start(const Settings& settings) {
    if (started_) return true;
    settings_ = settings;
    shadowInsets_ = ParseShadowInsets(settings.tracking.shadowInsets);
    treatAsTopmostClasses_ = ToWideList(settings.tracking.treatAsTopmostClasses);
    overlays_.Sync();
    started_ = true;
    PinDiag(L"边框后端启动（overlay 模型）");
    return true;
}

void WinBorderBackend::Apply(const std::vector<BorderModel>& models) {
    if (!started_) return;
    models_ = models;
    // 走到 Apply 的都是几何以外的事件：谁出现了、谁没了、焦点换了、z 序动了。这些都
    // 可能改变快照里除几何以外的任何一项，整份重取。
    snapshotValid_ = false;
    Redraw();
}

void WinBorderBackend::MoveBorder(WindowId id, const Rect& frame) {
    if (!started_) return;
    // 几何事件的快速路径：只有一个窗口动了。位置本身会在 Redraw 里从快照现取，这里
    // 只需要把模型里的 frame 跟上，免得下一次 Apply 用到旧值。
    for (auto& model : models_) {
        if (model.windowId == id) {
            model.frame = frame;
            break;
        }
    }
    // 只刷新这一个窗口的几何。frame 参数没直接用：快照里的矩形要和 CaptureDesktop
    // 同一个口径（DWM 扩展边界），混用两个来源会让边框错位一两像素。
    if (snapshotValid_ &&
        !RefreshWindowFrame(snapshot_,
                            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id)),
                            shadowInsets_)) {
        snapshotValid_ = false;
    }
    Redraw(true);
}

void WinBorderBackend::UpdateSettings(const Settings& settings) {
    settings_ = settings;
    shadowInsets_ = ParseShadowInsets(settings.tracking.shadowInsets);
    treatAsTopmostClasses_ = ToWideList(settings.tracking.treatAsTopmostClasses);
    // 清掉缓存：颜色或线宽变了而几何没变时，线段矩形可能完全一样，比较会误判成
    // 「没变」而跳过渲染。
    lastStrokes_.clear();
    snapshotValid_ = false;
    if (started_) Redraw();
}

void WinBorderBackend::Redraw(bool fromMove) {
    const bool trace = TraceOn();
    LONGLONG mark = trace ? Ticks() : 0;
    if (trace) {
        ++g_trace.calls;
        if (fromMove) ++g_trace.fromMove;
    }

    // 显示器配置可能变了（插拔、改分辨率、改缩放）。没变时 Sync 只做一次
    // EnumDisplayMonitors 加一次矩形比较，代价可以忽略，所以每帧确认一次就行，
    // 不用再接一套监听。
    overlays_.Sync();
    if (trace) {
        g_trace.sync += MsSince(mark);
        mark = Ticks();
    }

    // 一帧只认一份快照，这一帧里所有判断都只看它。以前 Coordinator 记着一份事件驱动
    // 的 active、平台层又实时问 GetForegroundWindow()，两者在焦点切换途中会分家。
    if (!snapshotValid_) {
        snapshot_ = CaptureDesktop(shadowInsets_, treatAsTopmostClasses_);
        snapshotValid_ = true;
        if (trace) ++g_trace.fullSnapshots;
    }
    if (trace) {
        g_trace.snap += MsSince(mark);
        mark = Ticks();
    }

    if (trace) g_lastSnapshot = snapshot_;
    std::vector<BorderStroke> strokes = PlanBorders(snapshot_, models_, settings_);
    if (trace) {
        g_trace.plan += MsSince(mark);
        mark = Ticks();
    }

    // 和上一帧一模一样就什么都不做：同样的线不必再提交一次。
    if (strokes == lastStrokes_) {
        if (trace) {
            ++g_trace.skipped;
            ReportIfDue();
        }
        return;
    }
    lastStrokes_ = strokes;
    overlays_.Render(strokes);
    if (trace) {
        g_trace.render += MsSince(mark);
        ReportIfDue();
    }
}

void WinBorderBackend::Stop() noexcept {
    overlays_.Destroy();
    models_.clear();
    lastStrokes_.clear();
    snapshotValid_ = false;
    snapshot_ = DesktopSnapshot{};
    started_ = false;
}

} // namespace windowmark::win
