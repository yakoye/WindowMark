#pragma once

#include "windowmark/core/Interfaces.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace windowmark::win {

class WinWindowBackend final : public IWindowBackend {
public:
    explicit WinWindowBackend(int geometryThrottleMs);
    ~WinWindowBackend() override;

    bool Start(EventSink sink) override;
    void SetGeometrySink(GeometrySink sink) override;
    void SetExcludedClasses(const std::vector<std::string>& classes) override;
    void Stop() noexcept override;
    [[nodiscard]] std::vector<WindowInfo> EnumerateWindows() override;
    [[nodiscard]] std::optional<WindowInfo> QueryWindow(WindowId id) override;
    [[nodiscard]] std::optional<Rect> QueryFrame(WindowId id) override;
    bool ActivateWindow(WindowId id) override;

private:
    static constexpr UINT kWakeMessage = WM_APP + 41;
    static constexpr UINT_PTR kGeometryTimerId = 41;

    static WinWindowBackend* instance_;
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static LRESULT CALLBACK DispatcherProc(HWND, UINT, WPARAM, LPARAM);
    static BOOL CALLBACK EnumProc(HWND, LPARAM);

    void EnqueueEvent(WindowId id, WindowEventKind kind);
    void HandleWake();
    void HandleGeometryTimer();
    void EmitPending(bool includeGeometry);
    bool InstallHooks();
    [[nodiscard]] std::optional<WindowInfo> BuildWindowInfo(HWND hwnd) const;
    [[nodiscard]] bool IsOwnWindow(HWND hwnd) const;

    // An executable path never changes for a live process, but resolving it costs an
    // OpenProcess round trip. Dragging a window re-queries its info ~30 times a second,
    // so the answer is cached per HWND and only re-resolved if the pid behind it changed.
    struct ProcessIdentity {
        DWORD processId{};
        std::string groupKey;
        std::string appName;
    };
    [[nodiscard]] const ProcessIdentity* ResolveIdentity(HWND hwnd, DWORD processId) const;
    void PruneIdentityCache(const std::vector<WindowInfo>& live) const;

    mutable std::unordered_map<HWND, ProcessIdentity> identityCache_;

    // DWM's extended frame differs from GetWindowRect by a constant inset for a given
    // window state, but querying it crosses into the DWM process. A drag fires ~100
    // location events a second, so the inset is measured once and reapplied to the cheap
    // local GetWindowRect, recalibrating only when the window's size actually changes.
    struct FrameInset {
        LONG left{}, top{}, right{}, bottom{};
        LONG width{}, height{};   // window rect size the inset was measured at
    };
    [[nodiscard]] Rect FrameFor(HWND hwnd) const;
    mutable std::unordered_map<HWND, FrameInset> frameInsets_;

    // Whether a window is top-level never changes, but asking costs a cross-process
    // GetWindowLongPtr. That question is asked for every location event - hundreds a
    // second while dragging - so the answer is remembered per window.
    [[nodiscard]] bool IsTopLevel(HWND hwnd) const;
    mutable std::unordered_map<HWND, bool> topLevelCache_;

    HWND dispatcher_{};
    EventSink sink_;
    GeometrySink geometrySink_;
    std::vector<HWINEVENTHOOK> hooks_;
    std::mutex pendingMutex_;
    std::unordered_map<WindowId, std::uint32_t> pendingBits_;
    std::atomic_bool wakePosted_{false};
    ULONGLONG lastGeometryDispatch_{0};
    int geometryThrottleMs_{33};
    DWORD processId_{0};
    // The user's own exclusions, kept as wide strings so the per-window check is a
    // straight comparison against the class name rather than a conversion each time.
    std::vector<std::wstring> excludedClasses_;
    std::atomic_bool geometryTimerArmed_{false};
};

} // namespace windowmark::win
