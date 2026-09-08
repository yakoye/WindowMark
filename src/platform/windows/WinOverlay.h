#pragma once

#include "WinBorderPlan.h"

#include <d2d1.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

namespace windowmark::win {

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
    // 圆角才需要 D2D。直角一路像素填充，连 render target 都不建。
    bool EnsureRenderTarget();

    HWND hwnd_{};
    HDC dc_{};
    HBITMAP bitmap_{};
    HBITMAP oldBitmap_{};
    void* bits_{};
    RECT bounds_{};
    // 上一帧画过的范围。这一帧要提交的脏区 = 它 ∪ 这一帧要画的范围。
    RECT lastPainted_{};
    bool hasLastPainted_{false};
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
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
