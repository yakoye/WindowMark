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

void WinWindowBackend::SetGeometrySink(GeometrySink sink) {
    geometrySink_ = std::move(sink);
}

namespace {

// "class:left,top,right,bottom" -> four ints. Returns false on anything malformed, which
// is treated as "no entry" rather than an error: the settings file is hand-edited, and a
// typo should cost the correction, not the app.
bool ParseInsetValues(const std::string& text, int (&out)[4]) {
    std::size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos > text.size()) return false;
        const std::size_t comma = text.find(',', pos);
        const std::string piece =
            text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (piece.empty()) return false;
        int value = 0;
        std::size_t digit = 0;
        const bool negative = piece[0] == '-';
        if (negative) digit = 1;
        if (digit >= piece.size()) return false;
        for (; digit < piece.size(); ++digit) {
            const char ch = piece[digit];
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + (ch - '0');
            if (value > 10000) return false;
        }
        // Negative would grow the outline outwards, which no toolkit needs and a stray
        // minus sign should not be able to do.
        out[i] = negative ? 0 : value;
        if (comma == std::string::npos) return i == 3;
        pos = comma + 1;
    }
    return false;
}

} // namespace

void WinWindowBackend::SetShadowInsets(const std::vector<std::string>& entries) {
    shadowInsets_.clear();
    for (const auto& entry : entries) {
        // rfind: a class name cannot contain ':' but a future value form might, so the
        // last colon is the separator.
        const std::size_t colon = entry.rfind(':');
        if (colon == std::string::npos || colon == 0) continue;
        int values[4]{};
        if (!ParseInsetValues(entry.substr(colon + 1), values)) continue;
        if (values[0] == 0 && values[1] == 0 && values[2] == 0 && values[3] == 0) continue;
        shadowInsets_.push_back(ShadowInset{Utf8ToWide(entry.substr(0, colon)), values[0],
                                            values[1], values[2], values[3]});
    }
    // The per-window cache has the old inset baked into it.
    frameInsets_.clear();
}

void WinWindowBackend::SetExcludedClasses(const std::vector<std::string>& classes) {
    excludedClasses_.clear();
    excludedClasses_.reserve(classes.size());
    for (const auto& name : classes) {
        if (name.empty()) continue;
        excludedClasses_.push_back(Utf8ToWide(name));
    }
    // A class that just became excluded may already be in the identity cache; drop it so
    // the next enumeration does not hand back a stale entry for a window we now ignore.
    identityCache_.clear();
    topLevelCache_.clear();
}

bool WinWindowBackend::IsTopLevel(HWND hwnd) const {
    const auto it = topLevelCache_.find(hwnd);
    if (it != topLevelCache_.end()) return it->second;
    const bool topLevel = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) == 0;
    topLevelCache_[hwnd] = topLevel;
    return topLevel;
}

namespace {

// Is this frame-inset reading one worth remembering?
//
// Three ways it is not:
//   - DWM did not answer, so ExtendedFrame fell back to the raw window rect.
//   - The window is minimized; its bounds are a placeholder, not where it will be.
//   - It has WS_THICKFRAME but measured a zero inset. Windows always keeps an invisible
//     resize border on such a window - 8,0,8,8 on every one of them measured here - so a
//     zero reading means DWM has not settled on this window yet. A zero inset *is* genuine
//     for a client-side-decorated window, and those have no thick frame, which is what
//     lets the two cases be told apart without guessing by class name.
[[nodiscard]] bool InsetLooksSettled(HWND hwnd, bool fromDwm, const auto& inset) {
    if (!fromDwm) return false;
    if (IsIconic(hwnd)) return false;
    const bool zero =
        inset.left == 0 && inset.top == 0 && inset.right == 0 && inset.bottom == 0;
    if (!zero) return true;
    return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) == 0;
}

} // namespace

Rect WinWindowBackend::FrameFor(HWND hwnd) const {
    RECT window{};
    if (!GetWindowRect(hwnd, &window)) return ExtendedFrame(hwnd);

    const LONG width = window.right - window.left;
    const LONG height = window.bottom - window.top;

    auto it = frameInsets_.find(hwnd);
    // Recalibrate when the size changes: maximizing or restoring changes the inset.
    if (it == frameInsets_.end() || it->second.width != width || it->second.height != height) {
        bool fromDwm = false;
        const Rect frame = ExtendedFrame(hwnd, fromDwm);
        FrameInset inset;
        inset.left = frame.left - window.left;
        inset.top = frame.top - window.top;
        inset.right = frame.right - window.right;
        inset.bottom = frame.bottom - window.bottom;

        // Whether this reading is worth remembering. It is cached until the window changes
        // size, so a bad one is not a glitch for a frame - it is a border sitting 8px out
        // for as long as the window keeps its size. Measured: a YeImageViewer window stuck
        // at 11,3,11,11 instead of 3,3,3,3, and one pixel of resize snapped it back.
        const bool settled = InsetLooksSettled(hwnd, fromDwm, inset);

        // A client-side-decorated window paints its shadow inside its own rect, and no API
        // reports where that stops - so the margin comes from settings. Baked into the
        // cached inset here, which means the class lookup happens on first sight and on
        // resize, never on a move.
        //
        // Skipped while maximized: there is nowhere to draw a shadow then, and GTK does
        // not. The cache already recalibrates on size change, so maximizing and restoring
        // both pick up the right answer on their own.
        if (!shadowInsets_.empty() && !IsZoomed(hwnd)) {
            wchar_t className[128]{};
            GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
            for (const auto& shadow : shadowInsets_) {
                if (shadow.className != className) continue;
                // Guarded against a value large enough to invert the rectangle: a typo
                // should look wrong, not make the outline vanish or draw backwards.
                if (shadow.left + shadow.right < width && shadow.top + shadow.bottom < height) {
                    inset.left += shadow.left;
                    inset.top += shadow.top;
                    inset.right -= shadow.right;
                    inset.bottom -= shadow.bottom;
                }
                break;
            }
        }

        inset.width = width;
        inset.height = height;
        // Not cached when the reading cannot be trusted, so the next geometry update tries
        // again. The outline is momentarily off by the frame inset, which is one tick at
        // most, rather than wrong until the user happens to resize the window.
        if (settled) frameInsets_[hwnd] = inset;
        return Rect{window.left + inset.left, window.top + inset.top,
                    window.right + inset.right, window.bottom + inset.bottom};
    }

    const auto& inset = it->second;
    return Rect{window.left + inset.left, window.top + inset.top,
                window.right + inset.right, window.bottom + inset.bottom};
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
    geometrySink_ = {};
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
    if (identityCache_.size() <= live.size() && frameInsets_.size() <= live.size()) return;

    std::unordered_set<HWND> alive;
    alive.reserve(live.size());
    for (const auto& info : live) alive.insert(HwndFromId(info.id));

    for (auto it = identityCache_.begin(); it != identityCache_.end();) {
        it = alive.contains(it->first) ? std::next(it) : identityCache_.erase(it);
    }
    for (auto it = frameInsets_.begin(); it != frameInsets_.end();) {
        it = alive.contains(it->first) ? std::next(it) : frameInsets_.erase(it);
    }
    // This one also collects windows we rejected, so it is pruned on size alone.
    if (topLevelCache_.size() > live.size() * 4 + 64) topLevelCache_.clear();
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

std::optional<Rect> WinWindowBackend::QueryFrame(WindowId id) {
    HWND hwnd = HwndFromId(id);
    // Only the two checks that can change from under us mid-drag. Eligibility, class,
    // process and title are all re-derived by QueryWindow, and none of them can change
    // because a window moved.
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return std::nullopt;
    return FrameFor(hwnd);
}

std::optional<WindowInfo> WinWindowBackend::BuildWindowInfo(HWND hwnd) const {
    if (!IsEligibleTopLevelWindow(hwnd, excludedClasses_) || IsOwnWindow(hwnd)) {
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
    // FrameFor, not ExtendedFrame: it is the one that applies the shadow inset and caches
    // the result. These were two different sources for the same number - QueryFrame used
    // FrameFor while this used the raw one - so a correction applied mid-drag was absent
    // from the geometry every window started with.
    info.frame = FrameFor(hwnd);
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
        if (!self->IsTopLevel(hwnd)) return;
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

    // Borders hug the window edge and would visibly trail a drag if they waited for the
    // throttled queue, so geometry is handed over immediately. This runs on the UI thread
    // during event dispatch: the handler only repositions a window, it does not repaint.
    if (kind == WindowEventKind::GeometryChanged && self->geometrySink_) {
        self->geometrySink_(IdFromHwnd(hwnd), self->FrameFor(hwnd));
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

            // Every kind must be listed. One that is missing has its bit set and never
            // cleared: the event is silently dropped and the map entry never goes away.
            // That is exactly what happened when DragStarted/DragEnded were added and
            // hiding-while-dragging quietly did nothing. The static_assert makes the next
            // addition a compile error instead of a silent no-op.
            //
            // Order is deliberate: DragStarted first so the rest of the batch finds the
            // overlay already suspended, DragEnded last so its rebuild wins.
            static constexpr WindowEventKind kOrder[] = {
                WindowEventKind::StructureChanged,
                WindowEventKind::VisibilityChanged,
                WindowEventKind::TitleChanged,
                WindowEventKind::ActiveChanged,
                WindowEventKind::GeometryChanged,
            };
            static_assert(std::size(kOrder) ==
                              static_cast<std::size_t>(WindowEventKind::VisibilityChanged) + 1,
                          "every WindowEventKind must appear in kOrder");

            for (WindowEventKind kind : kOrder) {
                if ((bits & Bit(kind)) == 0U) continue;
                if (kind == WindowEventKind::GeometryChanged && !includeGeometry) continue;
                events.push_back({kind, id});
                bits &= ~Bit(kind);
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
