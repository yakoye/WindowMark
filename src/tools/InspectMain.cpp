// WindowMarkInspect.exe - answers "why does that thing have an outline?"
//
// Watches for a while, then numbers every window that received an outline: a yellow badge
// pinned to each outline still on screen, and the same numbers in a table with the class
// name next to them. The class name is the answer - it goes into
// `tracking.exclude_classes` and the outline stops.
//
// It reads nothing from WindowMark. It finds the real outline windows on screen and works
// out what each one is wrapped around, so it cannot fall out of step with the app the way
// a second copy of the filtering rules would. That mattered: an earlier script version
// reimplemented the rules, drifted, and confidently named a class that was already
// excluded.
//
// Deliberately console subsystem and deliberately not linked against windowmark_core -
// same reasoning as the installers. It is a diagnostic, not part of the product.

#include "AppIdentity.h"

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <conio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace app = windowmark::app;

constexpr int kTickMs = 150;
constexpr int kDefaultWatchSeconds = 20;
// Total error across all four edges, in pixels, for an outline to be considered a match
// for a window. Small on purpose - a maximized 2560x1400 window and a full-screen 2560x1440
// IME host have centres only 20px apart, and matching loosely picked the wrong one.
constexpr int kRectTolerance = 5;

struct Seen {
    HWND target{};
    std::wstring process;
    std::wstring cls;
    std::wstring title;
    RECT lastBorder{};
    int ticks{};
    int runs{};
    int currentRun{};
    int longestRun{};
    int lastTick{-99};
    int maxConcurrent{};
};

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

// printf's %-Ns pads by bytes, but a CJK glyph is three UTF-8 bytes and two console
// columns, so every row with Chinese in it came out ragged. Pad by column count instead.
int DisplayWidth(const std::wstring& text) {
    int width = 0;
    for (const wchar_t ch : text) {
        // The ranges that are genuinely double-width in a console: CJK, kana, hangul and
        // the fullwidth forms.
        const bool wide = (ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2E80 && ch <= 0xA4CF) ||
                          (ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0xF900 && ch <= 0xFAFF) ||
                          (ch >= 0xFE30 && ch <= 0xFE6F) || (ch >= 0xFF00 && ch <= 0xFF60) ||
                          (ch >= 0xFFE0 && ch <= 0xFFE6);
        width += wide ? 2 : 1;
    }
    return width;
}

// Never truncates. A class name is meant to be copied out of this table and pasted into
// the settings, so an over-long one (PDF-XChange builds its class name out of the full
// executable path) makes its row ragged rather than becoming unusable. The trailing space
// is guaranteed so the next column never runs into it.
std::string Padded(const std::wstring& text, int columns) {
    std::string out = Utf8(text);
    int width = DisplayWidth(text);
    do {
        out += ' ';
        ++width;
    } while (width < columns);
    return out;
}

std::wstring ClassOf(HWND hwnd) {
    wchar_t buffer[256]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

std::wstring TitleOf(HWND hwnd) {
    wchar_t buffer[512]{};
    GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

std::wstring ProcessOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"?";
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) return L"?";
    wchar_t path[MAX_PATH]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring name = L"?";
    if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
        const std::wstring full(path, size);
        const auto slash = full.find_last_of(L'\\');
        name = slash == std::wstring::npos ? full : full.substr(slash + 1);
        const auto dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.erase(dot);
    }
    CloseHandle(handle);
    return name;
}

// The frame WindowMark itself uses, so the arithmetic below lines up exactly.
RECT FrameOf(HWND hwnd) {
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        GetWindowRect(hwnd, &rect);
    }
    return rect;
}

// How far the outline extends past the window: border.width + border.offset, clamped at 0,
// matching WinBorderBackend::Reach(). Read from the live settings file rather than assumed,
// because the whole match depends on it.
int ReadReach() {
    int width = 4;
    int offset = -1;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local) {
        std::wstring path = local;
        path += L"\\";
        path += app::kDataSubdir;
        path += L"\\settings.conf";
        if (FILE* file = _wfopen(path.c_str(), L"r, ccs=UTF-8")) {
            wchar_t line[512]{};
            while (fgetws(line, static_cast<int>(std::size(line)), file)) {
                int value = 0;
                if (swscanf_s(line, L"border.width=%d", &value) == 1) width = value;
                else if (swscanf_s(line, L"border.offset=%d", &value) == 1) offset = value;
            }
            fclose(file);
        }
    }
    return std::max(0, std::max(1, width) + offset);
}

struct Snapshot {
    std::vector<RECT> borders;
    std::vector<HWND> windows;
    std::vector<RECT> frames;
};

BOOL CALLBACK CollectProc(HWND hwnd, LPARAM param) {
    auto* snap = reinterpret_cast<Snapshot*>(param);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (ClassOf(hwnd) == app::kBorderWindowClass) {
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        snap->borders.push_back(rect);
        return TRUE;
    }
    const RECT frame = FrameOf(hwnd);
    if (frame.right - frame.left <= 1) return TRUE;
    snap->windows.push_back(hwnd);
    snap->frames.push_back(frame);
    return TRUE;
}

// Which window is this outline around? Size first; if that fails, ask the desktop what is
// under the outline's centre. The fallback earns its place: the IME candidate bar resizes
// on every keystroke and the outline trails it by tens of pixels, so the sizes genuinely
// disagree while it is being typed into.
HWND ResolveTarget(const RECT& border, const Snapshot& snap, int reach) {
    const RECT want{border.left + reach, border.top + reach,
                    border.right - reach, border.bottom - reach};
    HWND best = nullptr;
    int bestError = kRectTolerance;
    for (std::size_t i = 0; i < snap.windows.size(); ++i) {
        const RECT& frame = snap.frames[i];
        const int error = std::abs(frame.left - want.left) + std::abs(frame.top - want.top) +
                          std::abs(frame.right - want.right) + std::abs(frame.bottom - want.bottom);
        if (error < bestError) {
            bestError = error;
            best = snap.windows[i];
        }
    }
    if (best) return best;

    const POINT centre{(border.left + border.right) / 2, (border.top + border.bottom) / 2};
    HWND under = WindowFromPoint(centre);
    if (!under) return nullptr;
    HWND root = GetAncestor(under, GA_ROOT);
    if (!root) root = under;
    return ClassOf(root) == app::kBorderWindowClass ? nullptr : root;
}

// --- badges -----------------------------------------------------------------

constexpr wchar_t kBadgeClass[] = L"WindowMarkInspect.Badge";

LRESULT CALLBACK BadgeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH fill = CreateSolidBrush(RGB(255, 210, 0));
        FillRect(dc, &client, fill);
        DeleteObject(fill);
        FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        const int number = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        wchar_t text[8]{};
        _snwprintf_s(text, std::size(text), L"%d", number);

        HFONT font = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        DrawTextW(dc, text, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateBadge(int number, const RECT& border) {
    // WS_EX_TOOLWINDOW is not cosmetic here: without it WindowMark would put an outline
    // around the badge, and the tool would be reporting on itself.
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kBadgeClass, L"", WS_POPUP,
        border.left + 4, border.top + 4, 48, 48,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(number));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);

    int watchSeconds = kDefaultWatchSeconds;
    bool wait = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--now") {
            watchSeconds = 0;
        } else if (arg == L"--no-wait") {
            wait = false;
        } else if (arg == L"--watch" && i + 1 < argc) {
            watchSeconds = _wtoi(argv[++i]);
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            std::printf("WindowMarkInspect - 找出是谁被画了边框\n\n"
                        "  WindowMarkInspect.exe              盯 %d 秒\n"
                        "  WindowMarkInspect.exe --watch 40   盯 40 秒\n"
                        "  WindowMarkInspect.exe --now        只看此刻\n"
                        "  WindowMarkInspect.exe --no-wait    只打报告，不贴数字牌也不等按键\n",
                        kDefaultWatchSeconds);
            return 0;
        }
    }

    const int reach = ReadReach();
    std::printf("边框比窗口每边外扩 %d px（读自 settings.conf）\n", reach);

    std::vector<std::wstring> order;
    std::unordered_map<std::wstring, Seen> seen;
    int ticks = 0;

    const auto sampleOnce = [&]() {
        ++ticks;
        Snapshot snap;
        EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&snap));

        std::unordered_map<std::wstring, int> thisTick;
        for (const RECT& border : snap.borders) {
            HWND target = ResolveTarget(border, snap, reach);
            if (!target) continue;
            const std::wstring process = ProcessOf(target);
            const std::wstring cls = ClassOf(target);
            const std::wstring key = process + L"|" + cls;
            auto it = seen.find(key);
            if (it == seen.end()) {
                Seen entry;
                entry.target = target;
                entry.process = process;
                entry.cls = cls;
                it = seen.emplace(key, std::move(entry)).first;
                order.push_back(key);
            }
            it->second.target = target;
            it->second.title = TitleOf(target);
            it->second.lastBorder = border;
            ++thisTick[key];
        }
        for (const auto& [key, count] : thisTick) {
            Seen& entry = seen[key];
            ++entry.ticks;
            entry.maxConcurrent = std::max(entry.maxConcurrent, count);
            // Runs, not a raw count: a window opened halfway through and left open is not
            // a flyout, but a plain tally cannot tell the two apart.
            if (entry.lastTick == ticks - 1) {
                ++entry.currentRun;
            } else {
                ++entry.runs;
                entry.currentRun = 1;
            }
            entry.longestRun = std::max(entry.longestRun, entry.currentRun);
            entry.lastTick = ticks;
        }
    };

    if (watchSeconds <= 0) {
        sampleOnce();
    } else {
        std::printf("\n盯 %d 秒。这段时间把你不想要边框的东西都调出来一遍：\n", watchSeconds);
        std::printf("  打中文让候选框出来、Win+空格、点托盘箭头、开右键菜单……\n\n");
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(watchSeconds) * 1000;
        int reported = 0;
        while (GetTickCount64() < deadline) {
            sampleOnce();
            const int elapsed = watchSeconds - static_cast<int>((deadline - GetTickCount64()) / 1000);
            if (elapsed >= reported + 5) {
                reported = elapsed;
                std::printf("  ...%ds / %ds，已记录 %zu 个\n", elapsed, watchSeconds, seen.size());
            }
            Sleep(kTickMs);
        }
    }

    if (order.empty()) {
        std::printf("\n没有任何窗口被画边框。\n");
        return 0;
    }

    std::printf("\n=== 被画过边框的窗口 ===\n\n");
    std::printf("%s%s%s%s%s%s%s\n",
                Padded(L"编号", 6).c_str(), Padded(L"状态", 10).c_str(),
                Padded(L"段数", 7).c_str(), Padded(L"最长", 8).c_str(),
                Padded(L"进程", 20).c_str(), Padded(L"类名", 38).c_str(),
                Utf8(L"标题").c_str());
    std::vector<std::pair<int, RECT>> alive;
    int number = 0;
    for (const std::wstring& key : order) {
        const Seen& entry = seen[key];
        ++number;
        const bool onScreen = IsWindow(entry.target) && IsWindowVisible(entry.target);
        if (onScreen) alive.emplace_back(number, entry.lastBorder);
        wchar_t longest[16]{};
        _snwprintf_s(longest, std::size(longest), L"%.1fs", entry.longestRun * kTickMs / 1000.0);
        wchar_t numberText[8]{};
        _snwprintf_s(numberText, std::size(numberText), L"%d", number);
        wchar_t runsText[8]{};
        _snwprintf_s(runsText, std::size(runsText), L"%d", entry.runs);
        std::printf("%s%s%s%s%s%s%s\n",
                    Padded(numberText, 6).c_str(),
                    Padded(onScreen ? L"在屏幕上" : L"已消失", 10).c_str(),
                    Padded(runsText, 7).c_str(),
                    Padded(longest, 8).c_str(),
                    Padded(entry.process, 20).c_str(),
                    Padded(entry.cls, 38).c_str(),
                    Utf8(entry.title).c_str());
    }

    std::vector<HWND> badges;
    if (wait) {
        WNDCLASSEXW badgeClass{};
        badgeClass.cbSize = sizeof(badgeClass);
        badgeClass.lpfnWndProc = BadgeProc;
        badgeClass.hInstance = GetModuleHandleW(nullptr);
        badgeClass.lpszClassName = kBadgeClass;
        RegisterClassExW(&badgeClass);
        for (const auto& [badgeNumber, rect] : alive) {
            if (HWND badge = CreateBadge(badgeNumber, rect)) badges.push_back(badge);
        }
    }

    if (wait) {
        std::printf("\n屏幕上已标出 %zu 个编号（黄色数字牌贴在各自边框的左上角）。\n", badges.size());
        if (badges.size() < order.size()) {
            std::printf("另有 %zu 个已经消失（浮出窗口关掉了），只在上面的列表里，没有数字牌。\n",
                        order.size() - badges.size());
        }
    }

    std::printf("\n────────────────────────────────────────────────────────────\n");
    std::printf("怎么去掉不想要的边框\n");
    std::printf("────────────────────────────────────────────────────────────\n\n");
    std::printf("  1. 看屏幕上的黄色数字牌，记住不想要的那个编号。\n");
    std::printf("     （浮出窗口已经消失了的，看上面表格里「已消失」那几行。）\n\n");
    std::printf("  2. 在上面的表格里找到这个编号，抄下【类名】那一列。\n\n");
    std::printf("  3. 填到这里，两种方式任选：\n\n");
    std::printf("     方式一（推荐，改完立即生效）：\n");
    std::printf("       托盘图标右键 → 窗口边框 → 边框设置... → 「排除窗口类名」\n");
    std::printf("       多个类名用英文逗号隔开。\n\n");
    std::printf("     方式二（手改配置文件，改完要重启 WindowMark）：\n");
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    std::printf("       文件：%s\\WindowMark\\settings.conf\n",
                local ? Utf8(local).c_str() : "%LOCALAPPDATA%");
    std::printf("       那一行：tracking.exclude_classes=类名1,类名2\n\n");
    std::printf("  下面是把【所有】上面列出的类名都排除掉的完整一行，需要哪几个就删掉其余的：\n\n");
    std::printf("       tracking.exclude_classes=");
    bool first = true;
    for (const std::wstring& key : order) {
        if (!first) std::printf(",");
        std::printf("%s", Utf8(seen[key].cls).c_str());
        first = false;
    }
    std::printf("\n\n");
    std::printf("  注意：这一行会连普通应用窗口的类名一起列出来（比如资源管理器的\n");
    std::printf("  CabinetWClass）。别整行照抄，只留你真正不想要的那几个。\n\n");
    if (!wait) return 0;

    std::printf("按任意键收掉数字牌并退出（直接关掉这个窗口也行）...\n");

    // Badges are ordinary windows, so they need a pump; _kbhit lets the same loop watch
    // for the keypress instead of blocking on stdin and leaving them unpainted.
    while (!_kbhit()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(30);
    }
    for (HWND badge : badges) DestroyWindow(badge);
    return 0;
}
