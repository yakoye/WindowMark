#include "WinBorderBackend.h"

#include "PinDiag.h"
#include "WinBorderPlan.h"
#include "WinDesktopSnapshot.h"

namespace windowmark::win {

WinBorderBackend::~WinBorderBackend() { Stop(); }

bool WinBorderBackend::Start(const Settings& settings) {
    if (started_) return true;
    settings_ = settings;
    overlays_.Sync();
    started_ = true;
    PinDiag(L"边框后端启动（overlay 模型）");
    return true;
}

void WinBorderBackend::Apply(const std::vector<BorderModel>& models) {
    if (!started_) return;
    models_ = models;
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
    Redraw();
}

void WinBorderBackend::UpdateSettings(const Settings& settings) {
    settings_ = settings;
    if (started_) Redraw();
}

void WinBorderBackend::Redraw() {
    // 显示器配置可能变了（插拔、改分辨率、改缩放）。没变时 Sync 只做一次
    // EnumDisplayMonitors 加一次矩形比较，代价可以忽略，所以每帧确认一次就行，
    // 不用再接一套监听。
    overlays_.Sync();

    // 一帧只取一份快照，这一帧里所有判断都只认它。以前 Coordinator 记着一份事件驱动
    // 的 active、平台层又实时问 GetForegroundWindow()，两者在焦点切换途中会分家。
    const DesktopSnapshot snapshot = CaptureDesktop();
    overlays_.Render(PlanBorders(snapshot, models_, settings_));
}

void WinBorderBackend::Stop() noexcept {
    overlays_.Destroy();
    models_.clear();
    started_ = false;
}

} // namespace windowmark::win
