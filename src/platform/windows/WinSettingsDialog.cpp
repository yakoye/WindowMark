#include "WinSettingsDialog.h"

#include "AppIdentity.h"
#include "WinUtil.h"

#include <commctrl.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace windowmark::win {
namespace {

constexpr wchar_t kSettingsClass[] = L"WindowMark.SettingsDialog";
constexpr int kFirstFieldId = 3000;
constexpr int kOkId = 3900;
constexpr int kCancelId = 3901;
constexpr int kResetId = 3902;

enum class FieldKind { Int, Bool, Choice };

// The whole dialog is generated from this table. Accessors are captureless lambdas so
// they decay to plain function pointers and the table stays a static array.
struct Field {
    FieldKind kind{};
    const wchar_t* group{};
    const wchar_t* label{};
    int lo{};
    int hi{};
    int (*get)(const Settings&){};
    void (*set)(Settings&, int){};
    const wchar_t* hint{};
};

const Field kFields[] = {
    // --- 书签外观 ---
    {FieldKind::Int, L"书签外观", L"折叠长度", 24, 160,
     [](const Settings& s) { return s.drawer.collapsedExtent; },
     [](Settings& s, int v) { s.drawer.collapsedExtent = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"展开长度", 24, 480,
     [](const Settings& s) { return s.drawer.expandedExtent; },
     [](Settings& s, int v) { s.drawer.expandedExtent = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"厚度", 20, 80,
     [](const Settings& s) { return s.drawer.thickness; },
     [](Settings& s, int v) { s.drawer.thickness = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"间距", 0, 32,
     [](const Settings& s) { return s.drawer.gap; },
     [](Settings& s, int v) { s.drawer.gap = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"圆角半径", 0, 32,
     [](const Settings& s) { return s.drawer.cornerRadius; },
     [](Settings& s, int v) { s.drawer.cornerRadius = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"透明度", 0, 90,
     [](const Settings& s) { return s.drawer.transparency; },
     [](Settings& s, int v) { s.drawer.transparency = v; }, L"%  0=不透明"},
    {FieldKind::Int, L"书签外观", L"折叠显示字数", 1, 16,
     [](const Settings& s) { return s.drawer.shortNameChars; },
     [](Settings& s, int v) { s.drawer.shortNameChars = v; }, L"字"},
    {FieldKind::Int, L"书签外观", L"动画时长", 0, 1000,
     [](const Settings& s) { return s.drawer.animationMs; },
     [](Settings& s, int v) { s.drawer.animationMs = v; }, L"ms  0=不动画"},
    {FieldKind::Int, L"书签外观", L"顶部偏移", 0, 800,
     [](const Settings& s) { return s.drawer.topOffset; },
     [](Settings& s, int v) { s.drawer.topOffset = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"贴合重叠", 0, 24,
     [](const Settings& s) { return s.drawer.attachOverlap; },
     [](Settings& s, int v) { s.drawer.attachOverlap = v; }, L"px"},
    {FieldKind::Int, L"书签外观", L"激活额外长度", 0, 80,
     [](const Settings& s) { return s.drawer.activeExtraExtent; },
     [](Settings& s, int v) { s.drawer.activeExtraExtent = v; }, L"px"},

    // --- 底部横排 ---
    {FieldKind::Int, L"底部横排（窗口最大化时）", L"折叠宽度", 24, 240,
     [](const Settings& s) { return s.drawer.bottomCollapsedExtent; },
     [](Settings& s, int v) { s.drawer.bottomCollapsedExtent = v; }, L"px"},
    {FieldKind::Int, L"底部横排（窗口最大化时）", L"展开宽度", 24, 480,
     [](const Settings& s) { return s.drawer.bottomExpandedExtent; },
     [](Settings& s, int v) { s.drawer.bottomExpandedExtent = v; }, L"px"},
    {FieldKind::Int, L"底部横排（窗口最大化时）", L"平时高度", 0, 80,
     [](const Settings& s) { return s.drawer.bottomCollapsedThickness; },
     [](Settings& s, int v) { s.drawer.bottomCollapsedThickness = v; }, L"px  0=厚度一半"},

    // --- 悬停预览 ---
    {FieldKind::Bool, L"悬停预览", L"启用预览", 0, 1,
     [](const Settings& s) { return s.preview.enabled ? 1 : 0; },
     [](Settings& s, int v) { s.preview.enabled = v != 0; }, nullptr},
    {FieldKind::Int, L"悬停预览", L"延迟", 0, 5000,
     [](const Settings& s) { return s.preview.delayMs; },
     [](Settings& s, int v) { s.preview.delayMs = v; }, L"ms"},
    {FieldKind::Int, L"悬停预览", L"宽度", 160, 1600,
     [](const Settings& s) { return s.preview.width; },
     [](Settings& s, int v) { s.preview.width = v; }, L"px"},
    {FieldKind::Int, L"悬停预览", L"高度", 100, 1200,
     [](const Settings& s) { return s.preview.height; },
     [](Settings& s, int v) { s.preview.height = v; }, L"px"},
    {FieldKind::Int, L"悬停预览", L"圆角半径", 0, 32,
     [](const Settings& s) { return s.preview.cornerRadius; },
     [](Settings& s, int v) { s.preview.cornerRadius = v; }, L"px"},

    // --- 行为 ---
    {FieldKind::Choice, L"行为", L"书签位置", 0, 4,
     [](const Settings& s) { return static_cast<int>(s.drawer.placement); },
     [](Settings& s, int v) { s.drawer.placement = static_cast<Placement>(v); }, nullptr},
    {FieldKind::Bool, L"行为", L"仅在当前窗口显示", 0, 1,
     [](const Settings& s) { return s.drawer.activeWindowOnly ? 1 : 0; },
     [](Settings& s, int v) { s.drawer.activeWindowOnly = v != 0; }, nullptr},

    // --- 性能 ---
    {FieldKind::Int, L"性能", L"几何事件节流", 8, 250,
     [](const Settings& s) { return s.performance.geometryThrottleMs; },
     [](Settings& s, int v) { s.performance.geometryThrottleMs = v; }, L"ms"},
};

constexpr std::array<const wchar_t*, 5> kPlacementNames{
    L"自动", L"左侧", L"右侧", L"顶部", L"底部",
};

// Column assignment is by group, chosen so the two columns end at roughly the same
// height. Appearance alone is 11 rows, so it pairs with the 2-row behaviour block;
// everything else stacks on the right. Getting this wrong pushes the taller column
// down into the button row.
bool IsLeftColumn(const Field& field) {
    return wcscmp(field.group, L"书签外观") == 0 || wcscmp(field.group, L"行为") == 0;
}

struct Metrics {
    int dpi{96};
    int Scale(int value) const { return MulDiv(value, dpi, 96); }
};

HFONT CreateUiFont(int dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(dpi))) {
        return nullptr;
    }
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

class Dialog {
public:
    explicit Dialog(Settings& settings) : working_(settings) {}

    bool Run(HWND owner) {
        owner_ = owner;
        if (!Register()) return false;
        if (!Create()) return false;

        // Modal: disable the owner and pump until this window closes.
        const bool ownerWasEnabled = owner_ && IsWindowEnabled(owner_);
        if (ownerWasEnabled) EnableWindow(owner_, FALSE);

        MSG msg{};
        while (hwnd_) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                // The app is quitting underneath us; put WM_QUIT back for the outer loop.
                if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (hwnd_ && IsDialogMessageW(hwnd_, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (ownerWasEnabled) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        if (font_) DeleteObject(font_);
        return accepted_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Dialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case kOkId:
                if (Collect()) Close(true);
                return 0;
            case kCancelId:
                Close(false);
                return 0;
            case kResetId:
                Load(Settings{});
                return 0;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            Close(false);
            return 0;
        case WM_DESTROY:
            // No PostQuitMessage here: this runs nested inside the application's message
            // loop, and a WM_QUIT would tear down the whole tray app. Clearing hwnd_ is
            // what ends the modal loop below.
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool Register() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kSettingsClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND control = CreateWindowExW(
            0, cls, text, WS_CHILD | WS_VISIBLE | style,
            m_.Scale(x), m_.Scale(y), m_.Scale(w), m_.Scale(h),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (control && font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return control;
    }

    bool Create() {
        // Centre on the monitor the user is actually looking at - the one under the
        // pointer, since this is opened from a menu they just clicked.
        //
        // Deliberately NOT relative to owner_: that is the hidden tray control window,
        // a 0x0 window parked at (0,0), so centring on it puts the dialog at negative
        // coordinates, off the top-left of the screen.
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;

        // Per-monitor DPI, so the dialog is sized for the display it lands on rather
        // than for whichever one happens to host the tray window.
        m_.dpi = 96;
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
            m_.dpi = static_cast<int>(dpiX);
        }

        RECT bounds{0, 0, m_.Scale(kWidth), m_.Scale(kHeight)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
        const int outerW = bounds.right - bounds.left;
        const int outerH = bounds.bottom - bounds.top;

        // RECT members are LONG; keep everything int so the clamps below deduce cleanly.
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);

        // Clamp as well as centre: on a short display the dialog would otherwise start
        // above the work area and put its title bar out of reach.
        int x = workLeft + ((workRight - workLeft) - outerW) / 2;
        int y = workTop + ((workBottom - workTop) - outerH) / 2;
        x = std::clamp(x, workLeft, std::max(workLeft, workRight - outerW));
        y = std::clamp(y, workTop, std::max(workTop, workBottom - outerH));

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME, kSettingsClass, L"WindowMark 设置",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            x, y, outerW, outerH,
            owner_, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        font_ = CreateUiFont(m_.dpi);
        BuildControls();
        Load(working_);
        ShowWindow(hwnd_, SW_SHOW);
        // The owner is the hidden tray window, so nothing brings this to the front on its
        // own - without this the dialog can open behind whatever the user was looking at,
        // which reads as "the settings menu item did nothing".
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        UpdateWindow(hwnd_);
        return true;
    }

    void BuildControls() {
        int leftY = kPad;
        int rightY = kPad;
        const wchar_t* currentGroup = nullptr;
        int id = kFirstFieldId;

        for (const auto& field : kFields) {
            const bool left = IsLeftColumn(field);
            int& cursor = left ? leftY : rightY;
            const int columnX = left ? kPad : kPad + kColumnW + kPad;

            if (!currentGroup || wcscmp(currentGroup, field.group) != 0) {
                currentGroup = field.group;
                cursor += kGroupGap;
                Add(L"STATIC", field.group, SS_LEFTNOWORDWRAP,
                    columnX, cursor, kColumnW, kRowH, -1);
                cursor += kRowH + 2;
                Add(L"STATIC", L"", SS_ETCHEDHORZ, columnX, cursor, kColumnW, 1, -1);
                cursor += 6;
            }

            Add(L"STATIC", field.label, SS_LEFTNOWORDWRAP,
                columnX + kIndent, cursor + 3, kLabelW, kRowH, -1);

            const int controlX = columnX + kIndent + kLabelW;
            switch (field.kind) {
            case FieldKind::Int:
                controls_.push_back(Add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT,
                                        controlX, cursor, kEditW, kRowH, id));
                if (field.hint) {
                    Add(L"STATIC", field.hint, SS_LEFTNOWORDWRAP,
                        controlX + kEditW + 6, cursor + 3, kHintW, kRowH, -1);
                }
                break;
            case FieldKind::Bool:
                controls_.push_back(Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX,
                                        controlX, cursor + 2, kEditW, kRowH, id));
                break;
            case FieldKind::Choice: {
                HWND combo = Add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                 controlX, cursor, kEditW + kHintW, kRowH * 8, id);
                for (const wchar_t* name : kPlacementNames) {
                    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
                }
                controls_.push_back(combo);
                break;
            }
            }
            ++id;
            cursor += kRowH + kRowGap;
        }

        // Guard the invariant this layout depends on: neither column may reach the button
        // row. If a future field pushes it over, fail loudly here instead of silently
        // drawing controls on top of the buttons.
        const int buttonY = kHeight - kPad - kButtonH;
        const int tallest = std::max(leftY, rightY);
        if (tallest > buttonY - kRowGap) {
            OutputDebugStringW(L"[WindowMark] settings dialog: fields overflow the button row; "
                               L"raise kHeight or rebalance IsLeftColumn.\n");
        }

        Add(L"BUTTON", L"恢复默认值", WS_TABSTOP, kPad, buttonY, kButtonW, kButtonH, kResetId);
        Add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON,
            kWidth - kPad - kButtonW * 2 - 8, buttonY, kButtonW, kButtonH, kOkId);
        Add(L"BUTTON", L"取消", WS_TABSTOP, kWidth - kPad - kButtonW, buttonY, kButtonW, kButtonH, kCancelId);

        const std::wstring footer =
            std::wstring(L"WindowMark ") + app::kProductVersion + L" — 改动立即生效并写入 settings.conf";
        const int footerX = kPad + kButtonW + 12;
        Add(L"STATIC", footer.c_str(), SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            footerX, buttonY + 6, kWidth - kPad - kButtonW * 2 - 8 - footerX - 8, kRowH, -1);
    }

    void Load(const Settings& source) {
        for (std::size_t i = 0; i < std::size(kFields) && i < controls_.size(); ++i) {
            const auto& field = kFields[i];
            HWND control = controls_[i];
            if (!control) continue;
            const int value = field.get(source);
            switch (field.kind) {
            case FieldKind::Int:
                SetWindowTextW(control, std::to_wstring(value).c_str());
                break;
            case FieldKind::Bool:
                SendMessageW(control, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
                break;
            case FieldKind::Choice:
                SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(value), 0);
                break;
            }
        }
    }

    // Reads every control back into working_. Out-of-range numbers are reported rather
    // than silently clamped, so a typo does not quietly become a different setting.
    bool Collect() {
        Settings draft = working_;
        for (std::size_t i = 0; i < std::size(kFields) && i < controls_.size(); ++i) {
            const auto& field = kFields[i];
            HWND control = controls_[i];
            if (!control) continue;

            int value = 0;
            switch (field.kind) {
            case FieldKind::Int: {
                wchar_t buffer[32]{};
                GetWindowTextW(control, buffer, static_cast<int>(std::size(buffer)));
                if (buffer[0] == L'\0') {
                    Complain(field, control);
                    return false;
                }
                value = _wtoi(buffer);
                if (value < field.lo || value > field.hi) {
                    Complain(field, control);
                    return false;
                }
                break;
            }
            case FieldKind::Bool:
                value = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
                break;
            case FieldKind::Choice:
                value = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
                if (value < 0) value = 0;
                break;
            }
            field.set(draft, value);
        }

        if (draft.drawer.expandedExtent < draft.drawer.collapsedExtent) {
            draft.drawer.expandedExtent = draft.drawer.collapsedExtent;
        }
        if (draft.drawer.bottomExpandedExtent < draft.drawer.bottomCollapsedExtent) {
            draft.drawer.bottomExpandedExtent = draft.drawer.bottomCollapsedExtent;
        }
        working_ = draft;
        return true;
    }

    void Complain(const Field& field, HWND control) {
        const std::wstring text = std::wstring(L"「") + field.label + L"」需要在 " +
                                  std::to_wstring(field.lo) + L" 到 " +
                                  std::to_wstring(field.hi) + L" 之间。";
        MessageBoxW(hwnd_, text.c_str(), L"WindowMark 设置", MB_OK | MB_ICONWARNING);
        SetFocus(control);
    }

    static constexpr int kWidth = 700;
    // Tall enough for the longer column (appearance + behaviour, ~434px) plus the button
    // row. See the overflow guard in BuildControls.
    static constexpr int kHeight = 500;
    static constexpr int kPad = 14;
    static constexpr int kColumnW = 330;
    static constexpr int kIndent = 10;
    static constexpr int kLabelW = 130;
    static constexpr int kEditW = 70;
    static constexpr int kHintW = 110;
    static constexpr int kRowH = 22;
    static constexpr int kRowGap = 4;
    static constexpr int kGroupGap = 10;
    static constexpr int kButtonW = 96;
    static constexpr int kButtonH = 28;

    Settings& working_;
    Metrics m_;
    HWND owner_{};
    HWND hwnd_{};
    HFONT font_{};
    std::vector<HWND> controls_;
    bool accepted_{false};
};

} // namespace

bool WinSettingsDialog::ShowModal(HWND owner, Settings& settings) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Dialog dialog(settings);
    return dialog.Run(owner);
}

} // namespace windowmark::win
