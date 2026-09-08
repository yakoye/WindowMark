#pragma once

#include "WinOverlay.h"

#include "windowmark/core/Interfaces.h"

#include <vector>
#include <windows.h>

namespace windowmark::win {

// 边框后端：不再给每个窗口配一个 HWND 去抢 z 序，而是每块显示器一个透明画布，按当前
// 桌面快照算出该画哪几段线，一次画完。
//
// **Z 序只读，不写。** 这是整个设计的一句话。以前十几个边框窗口各自调 SetWindowPos
// 去精确插进 Windows 的全局 z 序链表，而那条链是全局共享的、任何进程都能改的、调整还
// 会静默失败的（返回 TRUE、GetLastError 为 0、状态纹丝不动）。后一个边框的插入打乱前
// 一个刚排好的位置，窗口越多越乱——「边框不全」「切换时闪一下」都出在这里。
//
// 现在「边框排第几」这个时序问题变成了「哪几段该画」的几何问题：非激活窗口的边框环
// 减去排在它上面的所有窗口，剩下的才画；激活窗口完整画、最后画。几何计算不会失败，
// 也没有竞争。
//
// 唯一还会动 z 序的是 overlay 自己在 topmost 层内挪到末尾（好让菜单、候选框、任务栏
// 压在它上面），那是层内换位，不是「提进层」——后者才是会卡死的那个操作，而 overlay
// 创建时就带 WS_EX_TOPMOST，根本不需要它。
class WinBorderBackend final : public IBorderBackend {
public:
    ~WinBorderBackend() override;

    bool Start(const Settings& settings) override;
    void Apply(const std::vector<BorderModel>& models) override;
    void MoveBorder(WindowId id, const Rect& frame) override;
    void UpdateSettings(const Settings& settings) override;
    void Stop() noexcept override;

private:
    void Redraw();

    Settings settings_;
    std::vector<BorderModel> models_;
    OverlaySet overlays_;
    bool started_{false};
};

} // namespace windowmark::win
