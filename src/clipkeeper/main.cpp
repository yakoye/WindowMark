#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <deque>
#include <string>
#include <iterator>

#include "clipboard.h"
#include "resource.h"

#include "ClipKeeperIdentity.h"

namespace ck = windowmark::clipkeeper;

namespace {

constexpr wchar_t kAppName[] = L"ClipKeeper 剪贴板守护";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTimerRescue = 1;
constexpr UINT kHotkeyRestore = 1;
constexpr ULONGLONG kRescueWindowMs = 3500;
constexpr int kMaxHistory = 20;
constexpr size_t kMaxHistoryBytes = 128ull * 1024ull * 1024ull;
constexpr int kMaxRescueAttempts = 5;

constexpr int IDC_LIST = 1001;
constexpr int IDC_RESTORE = 1002;
constexpr int IDC_CLEAR = 1003;
constexpr int IDC_AUTO = 1004;
constexpr int IDC_STARTUP = 1005;
constexpr int IDC_STATUS = 1006;
constexpr int IDC_HIDE = 1007;

constexpr int IDM_OPEN = 2001;
constexpr int IDM_RESTORE = 2002;
constexpr int IDM_AUTO = 2003;
constexpr int IDM_EXIT = 2004;

HMENU ControlId(int id) noexcept {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

HWND g_hwnd{};
HWND g_list{};
HWND g_status{};
HWND g_autoCheck{};
HWND g_startupCheck{};
HWND g_nextViewer{};
NOTIFYICONDATAW g_nid{};
std::deque<ClipboardSnapshot> g_history;
DWORD g_ignoreSequence{};
DWORD g_lastCapturedSequence{};
int g_rescueAttempts{};
bool g_exiting{};

bool IsAutoRescueEnabled() {
    return ::SendMessageW(g_autoCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetStatus(const std::wstring& text) {
    if (g_status) ::SetWindowTextW(g_status, text.c_str());
}

std::wstring ExePath() {
    wchar_t path[32768]{};
    DWORD len = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return std::wstring(path, len);
}

bool IsStartupEnabled() {
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t value[32768]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    LONG rc = ::RegQueryValueExW(key, L"ClipKeeper", nullptr, &type,
                                 reinterpret_cast<BYTE*>(value), &bytes);
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool SetStartupEnabled(bool enable) {
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG rc = ERROR_SUCCESS;
    if (enable) {
        std::wstring value = L"\"" + ExePath() + L"\"";
        rc = ::RegSetValueExW(key, L"ClipKeeper", 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = ::RegDeleteValueW(key, L"ClipKeeper");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

void RefreshList() {
    ::SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& snap : g_history) {
        std::wstring label = FormatSnapshotLabel(snap);
        ::SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
}

void AddSnapshot(ClipboardSnapshot&& snap) {
    if (snap.sequence == g_lastCapturedSequence) return;
    g_lastCapturedSequence = snap.sequence;
    g_rescueAttempts = 0;

    g_history.push_front(std::move(snap));
    auto totalBytes = []() {
        size_t total = 0;
        for (const auto& item : g_history)
            for (const auto& blob : item.blobs) total += blob.bytes.size();
        return total;
    };
    while (g_history.size() > 1 &&
           (static_cast<int>(g_history.size()) > kMaxHistory || totalBytes() > kMaxHistoryBytes)) {
        g_history.pop_back();
    }
    RefreshList();

    if (!g_history.empty()) {
        SetStatus(L"已保护：" + g_history.front().summary + L"   |   Ctrl+Alt+V 可随时恢复最近一条");
    }
}

bool RestoreIndex(size_t index, bool isAuto) {
    if (index >= g_history.size()) return false;
    DWORD seq = 0;
    if (!RestoreClipboardSnapshot(g_hwnd, g_history[index], &seq)) {
        SetStatus(L"恢复失败：当前剪贴板可能正被其他程序占用。可再按一次 Ctrl+Alt+V。");
        return false;
    }
    g_ignoreSequence = seq;
    if (!isAuto) {
        SetStatus(L"已重新放回系统剪贴板：" + g_history[index].summary);
    }
    return true;
}

void ScheduleRescue(const ClipboardState& state) {
    if (!IsAutoRescueEnabled() || g_history.empty()) return;
    ULONGLONG age = ::GetTickCount64() - g_history.front().tick;
    if (age > kRescueWindowMs || g_rescueAttempts >= kMaxRescueAttempts) return;
    if (state.hasText || state.hasImage || state.hasFiles) return;
    // Within the short rescue window, any clipboard state that has neither
    // text, image nor files is suspicious. ToDesk may leave metadata/private formats behind.

    std::wstring who = state.ownerProcess.empty() ? L"未知进程" : state.ownerProcess;
    SetStatus(L"检测到截图/文字刚复制后剪贴板异常（" + who + L"），准备自动恢复…");
    ::SetTimer(g_hwnd, kTimerRescue, 90, nullptr);
}

void HandleClipboardUpdate() {
    DWORD seq = ::GetClipboardSequenceNumber();
    if (seq != 0 && seq == g_ignoreSequence) {
        g_ignoreSequence = 0;
        return;
    }

    ClipboardSnapshot snap{};
    if (CaptureClipboardSnapshot(g_hwnd, snap)) {
        AddSnapshot(std::move(snap));
        return;
    }

    ClipboardState state = InspectClipboard(g_hwnd);
    ScheduleRescue(state);
}

void DoRescueTimer() {
    ::KillTimer(g_hwnd, kTimerRescue);
    if (!IsAutoRescueEnabled() || g_history.empty()) return;

    ULONGLONG age = ::GetTickCount64() - g_history.front().tick;
    if (age > kRescueWindowMs || g_rescueAttempts >= kMaxRescueAttempts) return;

    ClipboardState state = InspectClipboard(g_hwnd);
    if (state.hasText || state.hasImage || state.hasFiles) return;
    ++g_rescueAttempts;
    std::wstring who = state.ownerProcess.empty() ? L"未知进程" : state.ownerProcess;
    if (RestoreIndex(0, true)) {
        SetStatus(L"自动救援成功：已恢复 " + g_history.front().summary +
                  L"   |   异常前剪贴板所有者：" + who +
                  L"   |   第 " + std::to_wstring(g_rescueAttempts) + L" 次");
    }
}

void ShowMainWindow() {
    ::ShowWindow(g_hwnd, SW_SHOWNORMAL);
    ::SetForegroundWindow(g_hwnd);
}

void AddTrayIcon() {
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                                  IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!g_nid.hIcon) g_nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"ClipKeeper - 剪贴板守护");
    ::Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    if (g_nid.hWnd) ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, IDM_OPEN, L"打开 ClipKeeper");
    ::AppendMenuW(menu, MF_STRING, IDM_RESTORE, L"恢复最近一条\tCtrl+Alt+V");
    ::AppendMenuW(menu, MF_STRING | (IsAutoRescueEnabled() ? MF_CHECKED : 0), IDM_AUTO, L"自动救援");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(g_hwnd);
    ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    ::DestroyMenu(menu);
}

HFONT CreateUiFont(int pt, int weight = FW_NORMAL) {
    HDC dc = ::GetDC(nullptr);
    int height = -MulDiv(pt, ::GetDeviceCaps(dc, LOGPIXELSY), 72);
    ::ReleaseDC(nullptr, dc);
    return ::CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

HFONT g_font{};
HFONT g_titleFont{};

void ApplyFont(HWND h, HFONT font = nullptr) {
    ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
}

void Layout(HWND hwnd) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    const int m = 18;

    HWND title = ::GetDlgItem(hwnd, 1100);
    HWND desc = ::GetDlgItem(hwnd, 1101);
    HWND restore = ::GetDlgItem(hwnd, IDC_RESTORE);
    HWND clear = ::GetDlgItem(hwnd, IDC_CLEAR);
    HWND hide = ::GetDlgItem(hwnd, IDC_HIDE);

    ::MoveWindow(title, m, 14, w - 2*m, 30, TRUE);
    ::MoveWindow(desc, m, 46, w - 2*m, 42, TRUE);
    ::MoveWindow(g_autoCheck, m, 92, 150, 26, TRUE);
    ::MoveWindow(g_startupCheck, m + 160, 92, 150, 26, TRUE);
    ::MoveWindow(g_list, m, 126, w - 2*m, h - 224, TRUE);
    ::MoveWindow(restore, m, h - 86, 126, 34, TRUE);
    ::MoveWindow(clear, m + 136, h - 86, 100, 34, TRUE);
    ::MoveWindow(hide, w - m - 112, h - 86, 112, 34, TRUE);
    ::MoveWindow(g_status, m, h - 44, w - 2*m, 28, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 注册消息的值要运行时才知道，进不了 switch 的 case，所以在前面拦。
    // 函数级 static 保证只注册一次。
    static const UINT kShowPanel = ::RegisterWindowMessageW(ck::kShowPanelMessage);
    static const UINT kRequestQuit = ::RegisterWindowMessageW(ck::kRequestQuitMessage);

    // RegisterWindowMessageW 失败返回 0，而 0 就是 WM_NULL——不挡掉的话每个 WM_NULL
    // 都会被当成这两个命令执行。
    if (kShowPanel != 0 && msg == kShowPanel) {
        ShowMainWindow();
        return 0;
    }
    if (kRequestQuit != 0 && msg == kRequestQuit) {
        // g_exiting 是既有标志：置位后 WM_CLOSE 不再收起到托盘，而是走到 DefWindowProc
        // -> WM_DESTROY，那里会 ChangeClipboardChain 干净地退出监听链。
        g_exiting = true;
        ::DestroyWindow(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_font = CreateUiFont(10);
        g_titleFont = CreateUiFont(16, FW_SEMIBOLD);

        HWND title = ::CreateWindowW(L"STATIC", L"ClipKeeper  剪贴板守护", WS_CHILD | WS_VISIBLE,
                                     0, 0, 0, 0, hwnd, ControlId(1100), nullptr, nullptr);
        HWND desc = ::CreateWindowW(L"STATIC",
            L"针对 ToDesk 远程会话：先缓存截图/文字；若当前剪贴板随后被清空，则自动恢复。",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, ControlId(1101), nullptr, nullptr);
        g_autoCheck = ::CreateWindowW(L"BUTTON", L"自动救援", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     0, 0, 0, 0, hwnd, ControlId(IDC_AUTO), nullptr, nullptr);
        g_startupCheck = ::CreateWindowW(L"BUTTON", L"开机启动", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        0, 0, 0, 0, hwnd, ControlId(IDC_STARTUP), nullptr, nullptr);
        g_list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 0, 0, hwnd, ControlId(IDC_LIST), nullptr, nullptr);
        HWND restore = ::CreateWindowW(L"BUTTON", L"恢复选中项", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 0, 0, 0, hwnd, ControlId(IDC_RESTORE), nullptr, nullptr);
        HWND clear = ::CreateWindowW(L"BUTTON", L"清空缓存", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 0, 0, hwnd, ControlId(IDC_CLEAR), nullptr, nullptr);
        HWND hide = ::CreateWindowW(L"BUTTON", L"隐藏到托盘", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, hwnd, ControlId(IDC_HIDE), nullptr, nullptr);
        g_status = ::CreateWindowW(L"STATIC", L"等待复制文字或截图…", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, hwnd, ControlId(IDC_STATUS), nullptr, nullptr);

        ApplyFont(title, g_titleFont);
        ApplyFont(desc);
        ApplyFont(g_autoCheck);
        ApplyFont(g_startupCheck);
        ApplyFont(g_list);
        ApplyFont(restore);
        ApplyFont(clear);
        ApplyFont(hide);
        ApplyFont(g_status);

        ::SendMessageW(g_autoCheck, BM_SETCHECK, BST_CHECKED, 0);
        ::SendMessageW(g_startupCheck, BM_SETCHECK, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

        // Legacy viewer chain is intentional: the newest viewer is inserted at the front.
        // Starting ClipKeeper after ToDesk therefore gives us a chance to snapshot data first.
        g_nextViewer = ::SetClipboardViewer(hwnd);
        ::RegisterHotKey(hwnd, kHotkeyRestore, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');
        AddTrayIcon();
        return 0;
    }

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_DRAWCLIPBOARD:
        // Capture before forwarding to the next viewer (e.g. ToDesk).
        HandleClipboardUpdate();
        if (g_nextViewer && ::IsWindow(g_nextViewer)) {
            DWORD_PTR ignored = 0;
            ::SendMessageTimeoutW(g_nextViewer, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
        }
        return 0;

    case WM_CHANGECBCHAIN:
        if (reinterpret_cast<HWND>(wp) == g_nextViewer) {
            g_nextViewer = reinterpret_cast<HWND>(lp);
        } else if (g_nextViewer && ::IsWindow(g_nextViewer)) {
            DWORD_PTR ignored = 0;
            ::SendMessageTimeoutW(g_nextViewer, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
        }
        return 0;

    case WM_TIMER:
        if (wp == kTimerRescue) DoRescueTimer();
        return 0;

    case WM_HOTKEY:
        if (wp == kHotkeyRestore) RestoreIndex(0, false);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == IDC_RESTORE || (id == IDC_LIST && code == LBN_DBLCLK)) {
            LRESULT sel = ::SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) sel = 0;
            RestoreIndex(static_cast<size_t>(sel), false);
        } else if (id == IDC_CLEAR) {
            g_history.clear();
            RefreshList();
            SetStatus(L"本地缓存已清空。系统 Win+V 历史不会被修改。");
        } else if (id == IDC_HIDE) {
            ::ShowWindow(hwnd, SW_HIDE);
        } else if (id == IDC_STARTUP && code == BN_CLICKED) {
            bool enable = ::SendMessageW(g_startupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!SetStartupEnabled(enable)) {
                ::SendMessageW(g_startupCheck, BM_SETCHECK, enable ? BST_UNCHECKED : BST_CHECKED, 0);
                SetStatus(L"修改开机启动失败。");
            }
        } else if (id == IDM_OPEN) {
            ShowMainWindow();
        } else if (id == IDM_RESTORE) {
            RestoreIndex(0, false);
        } else if (id == IDM_AUTO) {
            bool enabled = IsAutoRescueEnabled();
            ::SendMessageW(g_autoCheck, BM_SETCHECK, enabled ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (id == IDM_EXIT) {
            g_exiting = true;
            ::DestroyWindow(hwnd);
        }
        return 0;
    }

    case kTrayMessage:
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
            ShowMainWindow();
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;

    case WM_CLOSE:
        if (!g_exiting) {
            ::ShowWindow(hwnd, SW_HIDE);
            SetStatus(L"ClipKeeper 正在托盘中继续保护剪贴板。");
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_nextViewer) ::ChangeClipboardChain(hwnd, g_nextViewer);
        ::UnregisterHotKey(hwnd, kHotkeyRestore);
        RemoveTrayIcon();
        if (g_font) ::DeleteObject(g_font);
        if (g_titleFont) ::DeleteObject(g_titleFont);
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    auto setDpi = reinterpret_cast<SetDpiAwarenessContextFn>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setDpi) setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 起两个实例会有两份都插进 Clipboard Viewer Chain：链结构乱掉，两份缓存互相抢救援。
    HANDLE singleton = ::CreateMutexW(nullptr, TRUE, ck::kSingletonMutex);
    if (singleton && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有一个在跑：把它的面板叫出来，自己退场。不用 SetForegroundWindow 抢前台
        // ——本机实测不可靠（前台锁），让已有实例自己 ShowWindow 就够了。
        if (const UINT showPanel = ::RegisterWindowMessageW(ck::kShowPanelMessage)) {
            if (HWND existing = ::FindWindowW(ck::kWindowClass, nullptr)) {
                ::PostMessageW(existing, showPanel, 0, 0);
            }
        }
        ::CloseHandle(singleton);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    ::InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = ck::kWindowClass;
    if (!::RegisterClassExW(&wc)) return 1;

    HWND hwnd = ::CreateWindowExW(0, ck::kWindowClass, kAppName,
                                  WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 720, 520,
                                  nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 2;

    ::ShowWindow(hwnd, show);
    ::UpdateWindow(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    if (singleton) ::CloseHandle(singleton);
    return static_cast<int>(msg.wParam);
}
