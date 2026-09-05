#pragma once

#include "windowmark/core/Interfaces.h"

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;

namespace windowmark::win {

// Draws a coloured outline hugging each tracked window. Deliberately independent of the
// bookmark overlay: it has its own enable switch and covers every top-level window,
// including apps that never get a bookmark strip. The two only share window tracking.
class WinBorderBackend final : public IBorderBackend {
public:
    WinBorderBackend();
    ~WinBorderBackend() override;

    bool Start(const Settings& settings) override;
    void Apply(const std::vector<BorderModel>& models) override;
    void MoveBorder(WindowId id, const Rect& frame) override;
    void UpdateSettings(const Settings& settings) override;
    void Stop() noexcept override;

private:
    class BorderWindow;
    class LayeredSurface;

    // 一个边框的创建或更新。抽出来是为了让 Apply 能先处理活动窗口那一个，
    // 而不必把整段逻辑写两遍。
    void ApplyOne(const BorderModel& model);

    // 把 z 序冻住的边框窗口换成新的。由守望轮询和 RequestStuckRecheck 调用。
    void RecreateStuckBorders();

    // 边框自己发现 z 序推不动时调用，请后端在下一个消息循环重建它。
    void RequestStuckRecheck();

    // 同一个窗口最多重建几次。重建也救不回来时就放弃，免得变成每帧建一个窗口。
    static constexpr int kMaxRecreateTries = 3;
    std::unordered_map<WindowId, int> recreateTries_;
    std::atomic<bool> stuckRecheckPosted_{false};

    bool EnsureFactory();
    bool EnsureWindowClass();
    // One render target for every border. Borders are drawn one at a time on the UI
    // thread and the target is re-bound to each window's memory DC, so a target per
    // window would just multiply GPU-side resources by the number of open windows.
    bool EnsureRenderTarget();

    // One scratch bitmap for the whole process, grown to the largest outline seen.
    // UpdateLayeredWindow copies what it is handed, so every border can render into the
    // same buffer in turn - which is what makes a per-window bitmap affordable and lets
    // each outline stay a single window (one SetWindowPos per move).
    std::unique_ptr<LayeredSurface> surface_;

    Settings settings_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    std::unordered_map<WindowId, std::unique_ptr<BorderWindow>> windows_;

    // 前台窗口专用的边框窗口：创建时就带 WS_EX_TOPMOST，此后只移动、只上色，永远
    // 不做 z 序调整——「把普通层窗口提进 topmost 层」正是会卡死的那个操作。
    std::unique_ptr<BorderWindow> activeBorder_;
    bool started_{false};

    // z 序守望。纯 z 序变化（置于顶层、压到最底）**不产生任何 WinEvent**——逐一试过
    // FOREGROUND / REORDER / LOCATIONCHANGE / SHOW / HIDE，目标窗口一条都不发。所以
    // 边框漂到错误深度这件事只能靠轮询发现，没有事件可依赖。
    //
    // 一个 message-only 窗口托着定时器。每次滴答对每个边框只做几次 GetWindow 的只读
    // 检查，位置正确就立刻返回。
    void StartZOrderWatch();
    void StopZOrderWatch() noexcept;
    static LRESULT CALLBACK WatchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static void CALLBACK ForegroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                        LONG idObject, LONG idChild, DWORD thread,
                                        DWORD time);

    static constexpr UINT_PTR kZOrderWatchTimerId = 1;
    static constexpr UINT kZOrderWatchIntervalMs = 500;
    // 前台切换时用的自定义消息，走 PostMessage 把活儿挪出钩子回调。
    static constexpr UINT kZOrderRecheckMsg = WM_APP + 1;
    HWND watchWindow_{};
    HWINEVENTHOOK foregroundHook_{};
};

} // namespace windowmark::win
