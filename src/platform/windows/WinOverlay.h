#pragma once

#include "WinBorderPlan.h"

#include <windows.h>

#include <memory>
#include <vector>

namespace windowmark::win {

// 渲染各段的累计耗时，诊断用。读一次清零。
//
// 分这么细是因为三件事的性质完全不同：fill 是直角边框的整段填充、arc 是圆角那圈带子
// 的逐像素计算、commit 是把位图交给窗口管理器。哪一段是瓶颈决定了要改什么。
struct RenderTrace {
    int frames{};          // 调用 Render 的次数（每块屏各算一次）
    int arcSegments{};     // 画了多少段圆角
    double arcPixels{};    // 圆角一共算了多少个像素
    double fillMs{};
    double arcMs{};
    double commitMs{};
    double dirtyMegapixels{};   // 提交的脏矩形总面积
};

[[nodiscard]] RenderTrace TakeRenderTrace();

// 一块显示器上的透明画布。
//
// **创建时就带 WS_EX_TOPMOST**，此后永远不执行「把普通层窗口提进 topmost 层」——那正是
// 会卡死的那个操作：实测存在这样的窗口，SetWindowPos 返回 TRUE、GetLastError 为 0、
// z 序和 topmost 位纹丝不动，换锚点、加 SWP_FRAMECHANGED、从别的进程调都推不动，而同
// 一线程的其他窗口做同样的调用都正常。生来就在 topmost 层，就不需要这个动作。
class MonitorOverlay {
public:
    MonitorOverlay() = default;
    ~MonitorOverlay();

    MonitorOverlay(const MonitorOverlay&) = delete;
    MonitorOverlay& operator=(const MonitorOverlay&) = delete;

    bool Create(const RECT& monitorRect);
    void Render(const std::vector<BorderStroke>& strokes);
    void Destroy() noexcept;

    [[nodiscard]] const RECT& Bounds() const noexcept { return bounds_; }

private:
    void MoveToBandTail();

    HWND hwnd_{};
    HDC dc_{};
    HBITMAP bitmap_{};
    HBITMAP oldBitmap_{};
    void* bits_{};
    RECT bounds_{};
    // 上一帧在这块屏上画了哪些段。和这一帧比出增删，就知道脏区该有多大——只画一个
    // 包围盒的话，桌面上任何一个窗口动一下都要重贴大半个屏幕。
    std::vector<BorderStroke> lastSegments_;
};

// 每块显示器一个 overlay，显示器配置变了就重建。
class OverlaySet {
public:
    void Sync();
    void Render(const std::vector<BorderStroke>& strokes);
    void Destroy() noexcept;

private:
    std::vector<std::unique_ptr<MonitorOverlay>> overlays_;
};

} // namespace windowmark::win
