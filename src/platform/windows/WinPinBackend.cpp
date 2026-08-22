#include "WinPinBackend.h"

#include "PinDiag.h"

#include <cstdint>
#include <iterator>
#include <utility>

namespace windowmark::win {
namespace {

// The events the system menu path needs, on top of what the window backend already hooks.
//
// MENUSTART rather than MENUPOPUPSTART, which is the pairing that looks obvious and never
// fires. Measured sequence when a title bar is right-clicked:
//
//   SYSTEM_MENUSTART       hwnd=目标窗口   idObject=OBJID_SYSMENU (-1)
//   OBJECT_CREATE          hwnd=菜单窗口
//   SYSTEM_MENUPOPUPSTART  hwnd=菜单窗口   idObject=OBJID_CLIENT (-4)
//
// So MENUPOPUPSTART reports the popup window and OBJID_CLIENT - the combination of it with
// OBJID_SYSMENU is never true. MENUSTART names the window the menu belongs to, and arrives
// before the popup window even exists, which is exactly when the item has to be added.
constexpr DWORD kMenuStart = 0x0004;   // EVENT_SYSTEM_MENUSTART
constexpr DWORD kMenuEnd = 0x0005;     // EVENT_SYSTEM_MENUEND

// The command id for our system menu item. Must stay below 0xF000, where the system SC_*
// commands begin. Collisions with an item the application itself owns are checked for.
constexpr UINT kPinMenuCommand = 0x7E11;

// Marks the item as ours when looking at a menu we may or may not have touched.
constexpr ULONG_PTR kPinMenuTag = 0x574D504Eull;

HWND HwndFromId(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

WindowId IdFromHwnd(HWND hwnd) {
    return static_cast<WindowId>(reinterpret_cast<std::uintptr_t>(hwnd));
}

[[nodiscard]] bool IsTopmost(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// True only when the item carrying our command id is one we put there. An application is
// free to use the same number for something of its own, and overwriting that item - or
// acting on a click meant for it - would be a good deal worse than not offering this.
[[nodiscard]] bool HasOurItem(HMENU menu) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(menu, kPinMenuCommand, FALSE, &info)) return false;
    return info.dwItemData == kPinMenuTag;
}

[[nodiscard]] bool CommandIdTaken(HMENU menu) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(menu, kPinMenuCommand, FALSE, &info)) return false;
    return info.dwItemData != kPinMenuTag;
}

WinPinBackend* g_instance = nullptr;

} // namespace

WinPinBackend::~WinPinBackend() { RemoveHooks(); }

bool WinPinBackend::Start(const Settings& settings, PinCallbacks callbacks) {
    settings_ = settings;
    callbacks_ = std::move(callbacks);
    g_instance = this;
    started_ = true;
    InstallHooks();
    return true;
}

// Setting the always-on-top style on a window in another process is allowed, and was
// measured to be: the style bit really does flip. Worth stating because a neighbouring
// restriction looks like it should apply and does not - Windows silently ignores z-order
// raises requested by a process that does not own the foreground window, returning TRUE and
// changing nothing. That rule governs moving within a band, not entering the topmost one.
std::optional<bool> WinPinBackend::SetTopmost(WindowId id, bool topmost) {
    HWND hwnd = HwndFromId(id);
    if (!IsWindow(hwnd)) {
        PinDiag(L"SetTopmost: hwnd 已失效 id=%llu", static_cast<unsigned long long>(id));
        return std::nullopt;
    }

    const bool was = IsTopmost(hwnd);
    if (was == topmost) return was;

    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    const bool now = IsTopmost(hwnd);
    PinDiag(L"SetTopmost: %ls 目标=%d 之前=%d 之后=%d 最小化=%d", cls, topmost ? 1 : 0,
            was ? 1 : 0, now ? 1 : 0, IsIconic(hwnd) ? 1 : 0);
    return was;
}

void WinPinBackend::Apply(const std::vector<PinRecord>& pinned) {
    pinned_ = pinned;
    // A system menu that is open right now has a stale tick the moment the set changes,
    // which is exactly what happens when the click came from that menu.
    if (menuWindow_ && IsWindow(menuWindow_)) UpdateSystemMenuItem(menuWindow_);
}

void WinPinBackend::UpdateSettings(const Settings& settings) {
    const bool wasShowing = settings_.pin.showInSystemMenu;
    settings_ = settings;
    if (!started_) return;
    if (wasShowing && !settings_.pin.showInSystemMenu && menuWindow_ && IsWindow(menuWindow_)) {
        RemoveSystemMenuItem(menuWindow_);
    }
}

void WinPinBackend::Stop() noexcept {
    RemoveHooks();
    // No unpinning here. The Coordinator owns the registry and restores every window before
    // it stops the backends, because it is the only side that knows what each window was
    // before it was pinned.
    pinned_.clear();
    callbacks_ = {};
    menuWindow_ = nullptr;
    started_ = false;
    if (g_instance == this) g_instance = nullptr;
}

void WinPinBackend::InstallHooks() {
    RemoveHooks();
    // WINEVENT_SKIPOWNPROCESS matters here as much as anywhere: the tray menu of this app
    // is a system menu as far as these events are concerned.
    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    // Spelled out as an array rather than a braced list: EVENT_OBJECT_INVOKED is a plain
    // int macro, so a mixed list gives the loop nothing to deduce a type from.
    const DWORD events[] = {kMenuStart, kMenuEnd,
                            static_cast<DWORD>(EVENT_OBJECT_INVOKED)};
    for (const DWORD event : events) {
        HWINEVENTHOOK hook = SetWinEventHook(event, event, nullptr, HookProc, 0, 0, flags);
        PinDiag(L"挂钩子 event=0x%04X 结果=%llu", event, (unsigned long long)(uintptr_t)hook);
        if (hook) hooks_.push_back(hook);
    }
}

void WinPinBackend::RemoveHooks() noexcept {
    for (HWINEVENTHOOK hook : hooks_) UnhookWinEvent(hook);
    hooks_.clear();
}

void CALLBACK WinPinBackend::HookProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD, DWORD) {
    if (!g_instance || !hwnd) return;
    (void)idObject;
    switch (event) {
    // idObject is deliberately ignored. It looks like it should be OBJID_SYSMENU and
    // sometimes is - but a real Alt+Space reports OBJID_WINDOW (0). It only came out as
    // OBJID_SYSMENU when the menu was opened synthetically with WM_SYSCOMMAND/SC_KEYMENU,
    // which is how an earlier version of this came to require it and never fire once.
    //
    // What the event does name reliably is the window whose menu is opening, so the check
    // moves there: OnMenuOpened takes only top-level windows that have a system menu.
    case kMenuStart:
        g_instance->OnMenuOpened(hwnd);
        return;
    case kMenuEnd:
        if (hwnd == g_instance->menuWindow_) g_instance->menuWindow_ = nullptr;
        return;
    case EVENT_OBJECT_INVOKED:
        g_instance->OnMenuItemInvoked(hwnd, idChild);
        return;
    default:
        return;
    }
}

// The item is added when the menu opens rather than kept there permanently. A menu nobody
// has opened is a menu nobody is looking at, and leaving items in the menus of other
// applications for a whole session is a bigger footprint than this feature is worth.
void WinPinBackend::OnMenuOpened(HWND window) {
    if (!started_ || !settings_.pin.enabled || !settings_.pin.showInSystemMenu) return;
    // MENUSTART fires for every menu, including an application's own menu bar, so the
    // window has to be vetted here rather than by the event's idObject.
    if (GetAncestor(window, GA_ROOT) != window) return;
    if (!GetSystemMenu(window, FALSE)) return;
    menuWindow_ = window;
    UpdateSystemMenuItem(window);
}

void WinPinBackend::UpdateSystemMenuItem(HWND window) const {
    if (!IsWindow(window)) return;
    HMENU menu = GetSystemMenu(window, FALSE);
    if (!menu) return;   // WinUI and UWP windows have none at all; nothing to do for them

    if (CommandIdTaken(menu)) {
        PinDiag(L"系统菜单: 命令 id 0x%X 已被该应用占用，跳过", kPinMenuCommand);
        return;
    }

    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING | MIIM_DATA;
    info.wID = kPinMenuCommand;
    info.fState = IsPinned(IdFromHwnd(window)) ? MFS_CHECKED : MFS_UNCHECKED;
    wchar_t text[] = L"置于顶层";
    info.dwTypeData = text;
    info.dwItemData = kPinMenuTag;

    if (HasOurItem(menu)) {
        // Only the tick can have changed; rewriting the whole item would move it.
        info.fMask = MIIM_STATE | MIIM_STRING;
        SetMenuItemInfoW(menu, kPinMenuCommand, FALSE, &info);
        return;
    }
    // Before SC_CLOSE rather than appended: 关闭 is conventionally last, and an item after
    // it reads as belonging to something else.
    InsertMenuItemW(menu, SC_CLOSE, FALSE, &info);
}

void WinPinBackend::RemoveSystemMenuItem(HWND window) const {
    if (!IsWindow(window)) return;
    HMENU menu = GetSystemMenu(window, FALSE);
    if (menu && HasOurItem(menu)) RemoveMenu(menu, kPinMenuCommand, MF_BYCOMMAND);
}

bool WinPinBackend::IsPinned(WindowId id) const {
    for (const auto& record : pinned_) {
        if (record.windowId == id) return true;
    }
    return false;
}

// WM_SYSCOMMAND for this item goes to the application that owns the window, not here. What
// does arrive is EVENT_OBJECT_INVOKED, and its idChild carries the command id - which is
// the whole reason this works without a DLL inside every target process.
void WinPinBackend::OnMenuItemInvoked(HWND eventWindow, LONG commandId) {
    if (!started_ || !settings_.pin.enabled || !settings_.pin.showInSystemMenu) return;
    if (commandId != static_cast<LONG>(kPinMenuCommand)) return;

    // The event does not reliably name the window whose menu it was, so try the candidates
    // in order of confidence and take the first that actually carries our item.
    const HWND candidates[] = {menuWindow_, eventWindow, GetForegroundWindow()};
    for (HWND candidate : candidates) {
        if (!candidate || !IsWindow(candidate)) continue;
        HMENU menu = GetSystemMenu(candidate, FALSE);
        if (!menu || !HasOurItem(menu)) continue;
        PinDiag(L"系统菜单点击 -> %llu", static_cast<unsigned long long>(IdFromHwnd(candidate)));
        if (callbacks_.onTogglePin) callbacks_.onTogglePin(IdFromHwnd(candidate));
        return;
    }
    PinDiag(L"系统菜单点击但认不出是哪个窗口");
}

} // namespace windowmark::win
