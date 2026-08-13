#include "WinWindowBackend.h"

#include "WinUtil.h"

#include <algorithm>
#include <utility>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_set>

namespace windowmark::win {

WinWindowBackend* WinWindowBackend::instance_ = nullptr;

namespace {

constexpr wchar_t kDispatcherClass[] = L"WindowMark.EventDispatcher";

std::uint32_t Bit(WindowEventKind kind) {
    return 1U << static_cast<unsigned>(kind);
}

WindowId IdFromHwnd(HWND hwnd) {
    return static_cast<WindowId>(reinterpret_cast<std::uintptr_t>(hwnd));
}

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

WinWindowBackend::WinWindowBackend(int geometryThrottleMs)
    : geometryThrottleMs_(std::clamp(geometryThrottleMs, 8, 250)),
      processId_(GetCurrentProcessId()) {}

WinWindowBackend::~WinWindowBackend() {
    Stop();
}

bool WinWindowBackend::Start(EventSink sink) {
    if (dispatcher_) return true;
    sink_ = std::move(sink);
    instance_ = this;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DispatcherProc;
    wc.lpszClassName = kDispatcherClass;
    RegisterClassExW(&wc);

    dispatcher_ = CreateWindowExW(
        0, kDispatcherClass, L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!dispatcher_) {
        instance_ = nullptr;
        sink_ = {};
        return false;
    }

    if (!InstallHooks()) {
        Stop();
        return false;
    }
    return true;
}

void WinWindowBackend::Stop() noexcept {
    for (HWINEVENTHOOK hook : hooks_) {
        if (hook) UnhookWinEvent(hook);
    }
    hooks_.clear();

    if (dispatcher_) {
        KillTimer(dispatcher_, kGeometryTimerId);
        DestroyWindow(dispatcher_);
        dispatcher_ = nullptr;
    }

    {
        std::lock_guard lock(pendingMutex_);
        pendingBits_.clear();
    }
    wakePosted_ = false;
    geometryTimerArmed_.store(false);
    sink_ = {};
    if (instance_ == this) instance_ = nullptr;
}

bool WinWindowBackend::InstallHooks() {
    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const DWORD events[] = {
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_MINIMIZESTART,
        EVENT_SYSTEM_MINIMIZEEND,
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_HIDE,
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_NAMECHANGE,
    };

    for (DWORD event : events) {
        HWINEVENTHOOK hook = SetWinEventHook(event, event, nullptr, WinEventProc, 0, 0, flags);
        if (!hook) return false;
        hooks_.push_back(hook);
    }
    return true;
}

std::vector<WindowInfo> WinWindowBackend::EnumerateWindows() {
    std::vector<WindowInfo> result;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&result));
    // A full enumeration is the natural point to drop identities for windows that are
    // gone; without it the cache would grow for the lifetime of the process.
    PruneIdentityCache(result);
    return result;
}

const WinWindowBackend::ProcessIdentity* WinWindowBackend::ResolveIdentity(
    HWND hwnd, DWORD processId) const {
    const auto it = identityCache_.find(hwnd);
    if (it != identityCache_.end() && it->second.processId == processId) {
        return &it->second;
    }

    const std::wstring processPath = QueryProcessPath(processId);
    if (processPath.empty()) return nullptr;

    ProcessIdentity identity;
    identity.processId = processId;
    identity.groupKey = LowerAscii(WideToUtf8(processPath));
    identity.appName = FileStemUtf8(processPath);

    auto [slot, _] = identityCache_.insert_or_assign(hwnd, std::move(identity));
    return &slot->second;
}

void WinWindowBackend::PruneIdentityCache(const std::vector<WindowInfo>& live) const {
    if (identityCache_.size() <= live.size()) return;

    std::unordered_set<HWND> alive;
    alive.reserve(live.size());
    for (const auto& info : live) alive.insert(HwndFromId(info.id));

    for (auto it = identityCache_.begin(); it != identityCache_.end();) {
        it = alive.contains(it->first) ? std::next(it) : identityCache_.erase(it);
    }
}

BOOL CALLBACK WinWindowBackend::EnumProc(HWND hwnd, LPARAM param) {
    if (!instance_) return TRUE;
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(param);
    auto info = instance_->BuildWindowInfo(hwnd);
    if (info.has_value()) out->push_back(std::move(*info));
    return TRUE;
}

std::optional<WindowInfo> WinWindowBackend::QueryWindow(WindowId id) {
    return BuildWindowInfo(HwndFromId(id));
}

std::optional<WindowInfo> WinWindowBackend::BuildWindowInfo(HWND hwnd) const {
    if (!IsEligibleTopLevelWindow(hwnd) || IsOwnWindow(hwnd)) {
        return std::nullopt;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == processId_) return std::nullopt;

    const ProcessIdentity* identity = ResolveIdentity(hwnd, pid);
    if (!identity) return std::nullopt;

    const int titleLength = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (titleLength > 0) {
        title.resize(static_cast<std::size_t>(titleLength + 1), L'\0');
        const int copied = GetWindowTextW(hwnd, title.data(), titleLength + 1);
        if (copied > 0) title.resize(static_cast<std::size_t>(copied));
        else title.clear();
    }

    WindowInfo info;
    info.id = IdFromHwnd(hwnd);
    info.groupKey = identity->groupKey;
    info.appName = identity->appName;
    info.title = title.empty() ? info.appName : WideToUtf8(title);
    info.frame = ExtendedFrame(hwnd);
    info.workArea = WorkAreaFor(hwnd);
    info.visible = IsWindowVisible(hwnd) != FALSE && !IsCloaked(hwnd);
    info.minimized = IsIconic(hwnd) != FALSE;
    info.maximized = IsZoomed(hwnd) != FALSE;
    info.active = GetForegroundWindow() == hwnd;
    return info;
}

bool WinWindowBackend::ActivateWindow(WindowId id) {
    HWND hwnd = HwndFromId(id);
    if (!IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    return SetForegroundWindow(hwnd) != FALSE;
}

bool WinWindowBackend::IsOwnWindow(HWND hwnd) const {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == processId_;
}

void CALLBACK WinWindowBackend::WinEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG objectId,
    LONG childId,
    DWORD,
    DWORD) {

    // Order matters for cost. EVENT_OBJECT_LOCATIONCHANGE fires for every mouse move
    // (OBJID_CURSOR) and for every child object of every window on the desktop, so the
    // overwhelming majority of callbacks are rejects. Discarding them must cost nothing
    // but a couple of integer compares - no cross-process calls before this point.
    // Every event this hook subscribes to reports OBJID_WINDOW/CHILDID_SELF for the
    // top-level windows we care about.
    if (objectId != OBJID_WINDOW || childId != CHILDID_SELF) return;

    auto* self = instance_;
    if (!self || !hwnd) return;
    // No IsOwnWindow() check: the hooks are installed with WINEVENT_SKIPOWNPROCESS, so
    // our own windows never reach this callback, and asking would cost a system call
    // on every surviving event.

    if (event >= EVENT_OBJECT_CREATE && event <= EVENT_OBJECT_NAMECHANGE) {
        if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0) return;
    }

    WindowEventKind kind = WindowEventKind::StructureChanged;
    switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
        kind = WindowEventKind::ActiveChanged;
        break;
    case EVENT_OBJECT_LOCATIONCHANGE:
        kind = WindowEventKind::GeometryChanged;
        break;
    case EVENT_OBJECT_NAMECHANGE:
        kind = WindowEventKind::TitleChanged;
        break;
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
        kind = WindowEventKind::VisibilityChanged;
        break;
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    default:
        kind = WindowEventKind::StructureChanged;
        break;
    }

    self->EnqueueEvent(IdFromHwnd(hwnd), kind);
}

void WinWindowBackend::EnqueueEvent(WindowId id, WindowEventKind kind) {
    {
        std::lock_guard lock(pendingMutex_);
        pendingBits_[id] |= Bit(kind);
    }

    // Location-change can fire hundreds of times per second while dragging.
    // Once a geometry timer is armed, only coalesce the latest state; do not flood the message queue.
    if (kind == WindowEventKind::GeometryChanged && geometryTimerArmed_.load()) {
        return;
    }
    if (!wakePosted_.exchange(true) && dispatcher_) {
        PostMessageW(dispatcher_, kWakeMessage, 0, 0);
    }
}

void WinWindowBackend::HandleWake() {
    wakePosted_ = false;
    const ULONGLONG now = GetTickCount64();
    const bool canDispatchGeometry = (now - lastGeometryDispatch_) >= static_cast<ULONGLONG>(geometryThrottleMs_);
    EmitPending(canDispatchGeometry);

    bool hasGeometry = false;
    {
        std::lock_guard lock(pendingMutex_);
        for (const auto& [_, bits] : pendingBits_) {
            if ((bits & Bit(WindowEventKind::GeometryChanged)) != 0U) {
                hasGeometry = true;
                break;
            }
        }
    }

    if (hasGeometry && !geometryTimerArmed_.load() && dispatcher_) {
        const ULONGLONG elapsed = now - lastGeometryDispatch_;
        const UINT delay = elapsed >= static_cast<ULONGLONG>(geometryThrottleMs_)
            ? 1U
            : static_cast<UINT>(geometryThrottleMs_ - elapsed);
        SetTimer(dispatcher_, kGeometryTimerId, std::max<UINT>(1U, delay), nullptr);
        geometryTimerArmed_.store(true);
    }
}

void WinWindowBackend::HandleGeometryTimer() {
    if (dispatcher_) KillTimer(dispatcher_, kGeometryTimerId);
    geometryTimerArmed_.store(false);
    EmitPending(true);
}

void WinWindowBackend::EmitPending(bool includeGeometry) {
    std::vector<WindowEvent> events;
    {
        std::lock_guard lock(pendingMutex_);
        for (auto it = pendingBits_.begin(); it != pendingBits_.end();) {
            std::uint32_t& bits = it->second;
            const WindowId id = it->first;

            const WindowEventKind ordered[] = {
                WindowEventKind::StructureChanged,
                WindowEventKind::VisibilityChanged,
                WindowEventKind::TitleChanged,
                WindowEventKind::ActiveChanged,
            };
            for (WindowEventKind kind : ordered) {
                if ((bits & Bit(kind)) != 0U) {
                    events.push_back({kind, id});
                    bits &= ~Bit(kind);
                }
            }

            if (includeGeometry && (bits & Bit(WindowEventKind::GeometryChanged)) != 0U) {
                events.push_back({WindowEventKind::GeometryChanged, id});
                bits &= ~Bit(WindowEventKind::GeometryChanged);
            }

            if (bits == 0U) it = pendingBits_.erase(it);
            else ++it;
        }
    }

    if (includeGeometry) lastGeometryDispatch_ = GetTickCount64();
    if (!sink_) return;
    for (const auto& event : events) sink_(event);
}

LRESULT CALLBACK WinWindowBackend::DispatcherProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<WinWindowBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WinWindowBackend*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case kWakeMessage:
        self->HandleWake();
        return 0;
    case WM_TIMER:
        if (wParam == kGeometryTimerId) {
            self->HandleGeometryTimer();
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace windowmark::win
