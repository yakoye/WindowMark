#include "WinSettingsDialog.h"

#include "windowmark/core/Hotkey.h"

#include "AppIdentity.h"
#include "AutoStart.h"
#include "Resource.h"
#include "WinUtil.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace windowmark::win {
namespace {

constexpr wchar_t kSettingsClass[] = L"WindowMark.SettingsDialog";
constexpr int kFirstFieldId = 3000;
constexpr int kOkId = 3900;
constexpr int kCancelId = 3901;
constexpr int kResetId = 3902;

enum class FieldKind { Int, Bool, Choice, Palette, Text, Hotkey };

// The whole dialog is generated from this table. Accessors are captureless lambdas so
// they decay to plain function pointers and the table stays a static array.
struct Field {
    FieldKind kind{};
    SettingsPage page{};
    const wchar_t* group{};
    const wchar_t* label{};
    int lo{};
    int hi{};
    int (*get)(const Settings&){};
    void (*set)(Settings&, int){};
    const wchar_t* hint{};
    const wchar_t* const* choices{};
    int choiceCount{};
    // Only for FieldKind::Text. Kept separate rather than shoehorned into the int
    // accessors so the numeric fields stay as simple as they were.
    std::wstring (*getText)(const Settings&){};
    void (*setText)(Settings&, const std::wstring&){};
    // For settings that are not part of Settings at all. Start-with-Windows is one: it
    // lives in the registry, where Windows itself lets the user change it, so keeping a
    // copy in settings.conf would mean two records that disagree. When these are set,
    // get/set are ignored.
    int (*getExternal)(){};
    void (*setExternal)(int){};
    // Only for FieldKind::Palette: the six presets shown as swatches. A seventh cell,
    // 自定义, is always appended by the control itself.
    const unsigned* swatches{};
    int swatchCount{};
};

// Nothing uses the external channel today - start-with-Windows lives in the tray menu,
// where a program-wide switch sits better than on either feature's page. The channel stays
// because the reason it exists is not going away: any setting Windows also lets the user
// change from outside has to be read from where Windows keeps it, not mirrored into
// settings.conf where the two copies would disagree.

// "A,B,C" <-> the list form the settings file and the backend both use.
std::wstring JoinClasses(const std::vector<std::string>& values) {
    std::wstring out;
    for (const auto& value : values) {
        if (!out.empty()) out += L',';
        out += Utf8ToWide(value);
    }
    return out;
}

std::vector<std::string> SplitClasses(const std::wstring& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(L',', start);
        const std::size_t end = comma == std::wstring::npos ? text.size() : comma;
        std::wstring piece = text.substr(start, end - start);
        // Class names never have leading or trailing spaces, and typing "A, B" is the
        // natural thing to do.
        const auto first = piece.find_first_not_of(L" \t");
        const auto last = piece.find_last_not_of(L" \t");
        if (first != std::wstring::npos) {
            piece = piece.substr(first, last - first + 1);
            if (!piece.empty()) out.push_back(WideToUtf8(piece));
        }
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return out;
}

// The shortcut box records a key combination instead of accepting typed text. Subclassing
// an EDIT rather than using the stock hotkey control: that one insists on its own idea of
// which combinations are legal and cannot be cleared, and it renders modifiers in an order
// this app does not use.
bool IsModifierKey(unsigned vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

LRESULT CALLBACK HotkeyEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_GETDLGCODE:
        // Without this the dialog manager swallows Tab, Enter, Escape and the arrow keys
        // before the control sees them - and every one of those is bindable.
        return DLGC_WANTALLKEYS;
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;   // nothing is ever typed in here
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const unsigned vk = static_cast<unsigned>(wParam);
        // Backspace and Delete clear the box. Both are bindable in principle, but a field
        // that cannot be emptied is worse than two keys that cannot be bound.
        if (vk == VK_BACK || vk == VK_DELETE) {
            SetWindowTextW(hwnd, L"");
            return 0;
        }
        if (IsModifierKey(vk)) return 0;   // hold; the real key has not arrived yet
        windowmark::Hotkey hotkey{};
        if (GetKeyState(VK_CONTROL) < 0) hotkey.mods |= windowmark::Hotkey::kCtrl;
        if (GetKeyState(VK_MENU) < 0) hotkey.mods |= windowmark::Hotkey::kAlt;
        if (GetKeyState(VK_SHIFT) < 0) hotkey.mods |= windowmark::Hotkey::kShift;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) {
            hotkey.mods |= windowmark::Hotkey::kWin;
        }
        hotkey.key = vk;
        // An empty result means no modifier was held, or the key has no name this can
        // round-trip. Both are refused by leaving the box alone rather than showing
        // something that would not survive a save.
        const std::wstring text = windowmark::FormatHotkeyWide(hotkey);
        if (!text.empty()) SetWindowTextW(hwnd, text.c_str());
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return 0;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 色卡条：六个预设 + 一个「自定义」。
//
// 取代原来的 #RRGGBB 输入框。输入框有两个问题：要用户自己想出一个颜色值，而且
// 「跟随系统强调色」内部存的是 0，画出来就成了 #00000000 —— 用户看到的是黑色，
// 而实际生效的是强调色。色卡把实际在用的颜色直接画出来，旁边写上真实色值。
// ---------------------------------------------------------------------------

// Defined further down with the other colour helpers; the swatch strip needs it here.
std::wstring ColorToText(int argb);

constexpr wchar_t kSwatchClass[] = L"WindowMark.Swatches";

struct SwatchData {
    unsigned value{};
    const unsigned* presets{};
    int presetCount{};
    // True when presets[0] is PinSettings::kAccentColor, i.e. this field understands
    // "follow the system". Border colours do not, so a stored 0 there is just black.
    bool accentFirst{};
    int focus{};
    // Physical pixels, computed by the dialog which is the side that knows the DPI.
    int cell{16};
    int gap{7};
    // Distance from the cell edge to the selection ring, in physical pixels. Hardcoding it
    // made the ring hug the swatch at 125% DPI and read as a black border on the colour
    // rather than a ring around it.
    int ring{4};
    int textX{};
    HFONT font{};
};

SwatchData* SwatchOf(HWND hwnd) {
    return reinterpret_cast<SwatchData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// What to actually paint for a stored value.
unsigned ResolveSwatch(const SwatchData& data, unsigned value) {
    if (data.accentFirst && value == PinSettings::kAccentColor) return SystemAccentColor();
    return value;
}

COLORREF ToColorRef(unsigned argb) {
    return RGB((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu);
}

// -1 when the value is not one of the presets, which is what puts it in the 自定义 cell.
int SwatchIndexOf(const SwatchData& data, unsigned value) {
    for (int i = 0; i < data.presetCount; ++i) {
        if (data.presets[i] == value) return i;
    }
    return -1;
}

std::wstring SwatchCaption(const SwatchData& data) {
    const unsigned shown = ResolveSwatch(data, data.value);
    const std::wstring hex = ColorToText(static_cast<int>(shown));
    if (data.accentFirst && data.value == PinSettings::kAccentColor) {
        // The whole point of showing the hex here: "跟随系统" alone leaves the user
        // guessing which colour that actually is right now.
        return L"跟随系统 " + hex;
    }
    return hex;
}

void SwatchPaint(HWND hwnd, SwatchData& data) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    // Painted into a memory DC first: the cells are redrawn on every arrow key and the
    // dialog background shows through the gaps, so drawing straight to the screen flickers.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH back = GetSysColorBrush(COLOR_3DFACE);
    FillRect(mem, &client, back);

    const int selected = SwatchIndexOf(data, data.value);
    const int cells = data.presetCount + 1;   // +1 for 自定义
    const int top = (client.bottom - data.cell) / 2;

    HPEN edgePen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
    HPEN ringPen = CreatePen(PS_SOLID, 2, GetSysColor(COLOR_WINDOWTEXT));

    for (int i = 0; i < cells; ++i) {
        const int x = i * (data.cell + data.gap);
        const RECT box{x, top, x + data.cell, top + data.cell};
        const bool isCustom = (i == data.presetCount);
        const bool chosen = isCustom ? (selected < 0) : (i == selected);

        if (isCustom && selected >= 0) {
            // Nothing custom is in force, so this cell is a door rather than a value:
            // a neutral tile with an ellipsis, matching how "更多" reads elsewhere.
            HBRUSH fill = GetSysColorBrush(COLOR_WINDOW);
            FillRect(mem, &box, fill);
            HGDIOBJ oldPen = SelectObject(mem, edgePen);
            HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, box.left, box.top, box.right, box.bottom);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
            HGDIOBJ oldFont = data.font ? SelectObject(mem, data.font) : nullptr;
            RECT text = box;
            DrawTextW(mem, L"\u00b7\u00b7\u00b7", -1, &text,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (oldFont) SelectObject(mem, oldFont);
        } else {
            const unsigned argb =
                isCustom ? ResolveSwatch(data, data.value)
                         : ResolveSwatch(data, data.presets[i]);
            HBRUSH fill = CreateSolidBrush(ToColorRef(argb));
            HGDIOBJ oldPen = SelectObject(mem, edgePen);
            HGDIOBJ oldBrush = SelectObject(mem, fill);
            Rectangle(mem, box.left, box.top, box.right, box.bottom);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
            DeleteObject(fill);
        }

        if (chosen) {
            // Ring outside the cell rather than a border on it: a border would change the
            // apparent size of the colour and make two adjacent swatches look different.
            RECT ring{box.left - data.ring, box.top - data.ring,
                      box.right + data.ring, box.bottom + data.ring};
            HGDIOBJ oldPen = SelectObject(mem, ringPen);
            HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, ring.left, ring.top, ring.right, ring.bottom);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
        }
        if (GetFocus() == hwnd && i == data.focus) {
            const int inset = data.ring - 2;
            RECT focus{box.left - inset, box.top - inset,
                       box.right + inset, box.bottom + inset};
            DrawFocusRect(mem, &focus);
        }
    }

    DeleteObject(edgePen);
    DeleteObject(ringPen);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
    HGDIOBJ oldFont = data.font ? SelectObject(mem, data.font) : nullptr;
    RECT caption{data.textX, 0, client.right, client.bottom};
    const std::wstring text = SwatchCaption(data);
    DrawTextW(mem, text.c_str(), -1, &caption,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(mem, oldFont);

    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// The 16 slots the common dialog remembers between openings. Static because the dialog
// has nowhere to keep them and losing them every time would be its own annoyance.
COLORREF g_customColors[16] = {
    RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
    RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
    RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
    RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
};

void SwatchPickCustom(HWND hwnd, SwatchData& data) {
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = GetParent(hwnd);
    cc.lpCustColors = g_customColors;
    cc.rgbResult = ToColorRef(ResolveSwatch(data, data.value));
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;   // cancelled; leave the value alone
    // Alpha is not something the common dialog offers, and every preset is opaque, so the
    // result is stored opaque. A translucent value can still be hand-written in the file.
    data.value = 0xFF000000u | (static_cast<unsigned>(GetRValue(cc.rgbResult)) << 16) |
                 (static_cast<unsigned>(GetGValue(cc.rgbResult)) << 8) |
                 static_cast<unsigned>(GetBValue(cc.rgbResult));
    data.focus = data.presetCount;
    InvalidateRect(hwnd, nullptr, TRUE);
}

void SwatchActivate(HWND hwnd, SwatchData& data, int index) {
    if (index < 0 || index > data.presetCount) return;
    data.focus = index;
    if (index == data.presetCount) {
        SwatchPickCustom(hwnd, data);
        return;
    }
    data.value = data.presets[index];
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK SwatchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* data = SwatchOf(hwnd);
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_DESTROY:
        delete data;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_PAINT:
        if (data) { SwatchPaint(hwnd, *data); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT fills the whole client area itself
    case WM_GETDLGCODE:
        // The arrows move between swatches, so the dialog manager must not take them to
        // move between controls.
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONDOWN: {
        if (!data) break;
        SetFocus(hwnd);
        const int x = GET_X_LPARAM(lParam);
        const int stride = data->cell + data->gap;
        const int index = stride > 0 ? x / stride : -1;
        // Reject the gap between two cells rather than rounding to a neighbour: a click
        // that lands between swatches should do nothing, not pick the one on the left.
        if (index >= 0 && index <= data->presetCount && (x % stride) < data->cell) {
            SwatchActivate(hwnd, *data, index);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!data) break;
        const int last = data->presetCount;
        switch (wParam) {
        case VK_LEFT:
        case VK_UP:
            if (data->focus > 0) { --data->focus; InvalidateRect(hwnd, nullptr, TRUE); }
            return 0;
        case VK_RIGHT:
        case VK_DOWN:
            if (data->focus < last) { ++data->focus; InvalidateRect(hwnd, nullptr, TRUE); }
            return 0;
        case VK_HOME:
            data->focus = 0; InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case VK_END:
            data->focus = last; InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case VK_SPACE:
        case VK_RETURN:
            SwatchActivate(hwnd, *data, data->focus);
            return 0;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterSwatchClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SwatchProc;
    wc.lpszClassName = kSwatchClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// 六个预设，白色不在其中：白色边框在浅色桌面上等于没画。
// 第一格是各自原来的默认值，升级上来的配置不会突然在色卡里找不到自己。
constexpr unsigned kSwatchRed = 0xFFE81123u;
constexpr unsigned kSwatchOrange = 0xFFF7630Cu;
constexpr unsigned kSwatchYellow = 0xFFFFB900u;
constexpr unsigned kSwatchGreen = 0xFF16C60Cu;
constexpr unsigned kSwatchPurple = 0xFF8E4EC6u;

constexpr unsigned kBorderActiveSwatches[] = {
    0xFF6274E7u, kSwatchRed, kSwatchOrange, kSwatchYellow, kSwatchGreen, kSwatchPurple,
};
constexpr unsigned kBorderInactiveSwatches[] = {
    0xFF7080AAu, kSwatchRed, kSwatchOrange, kSwatchYellow, kSwatchGreen, kSwatchPurple,
};
// The first cell here is 跟随系统强调色, stored as 0. It is painted with whatever the
// accent is right now, which is also what the caption spells out.
constexpr unsigned kPinSwatches[] = {
    PinSettings::kAccentColor, kSwatchRed, kSwatchOrange, kSwatchYellow, kSwatchGreen,
    kSwatchPurple,
};

constexpr const wchar_t* kPlacementChoices[] = {L"自动", L"左侧", L"右侧", L"顶部", L"底部"};
constexpr const wchar_t* kCornerChoices[] = {L"跟随系统", L"直角", L"圆角", L"小圆角", L"自定义"};

const Field kFields[] = {
    // --- 书签 ---
    // The on/off switch leads, mirroring 启用边框 on the border page.
    {FieldKind::Bool, SettingsPage::Bookmarks, L"书签", L"启用书签", 0, 1,
     [](const Settings& s) { return s.drawer.enabled ? 1 : 0; },
     [](Settings& s, int v) { s.drawer.enabled = v != 0; }, nullptr},
    {FieldKind::Choice, SettingsPage::Bookmarks, L"书签", L"书签位置", 0, 4,
     [](const Settings& s) { return static_cast<int>(s.drawer.placement); },
     [](Settings& s, int v) { s.drawer.placement = static_cast<Placement>(v); }, nullptr,
     kPlacementChoices, static_cast<int>(std::size(kPlacementChoices))},
    // Shortened from 「仅在当前窗口显示」: at eight glyphs it was the one label forcing
    // the whole page's label column 26px wider than anything else needed.
    {FieldKind::Bool, SettingsPage::Bookmarks, L"书签", L"仅当前窗口", 0, 1,
     [](const Settings& s) { return s.drawer.activeWindowOnly ? 1 : 0; },
     [](Settings& s, int v) { s.drawer.activeWindowOnly = v != 0; }, nullptr},

    // --- 书签外观 ---
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"折叠长度", 24, 160,
     [](const Settings& s) { return s.drawer.collapsedExtent; },
     [](Settings& s, int v) { s.drawer.collapsedExtent = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"展开长度", 24, 480,
     [](const Settings& s) { return s.drawer.expandedExtent; },
     [](Settings& s, int v) { s.drawer.expandedExtent = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"厚度", 20, 80,
     [](const Settings& s) { return s.drawer.thickness; },
     [](Settings& s, int v) { s.drawer.thickness = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"间距", 0, 32,
     [](const Settings& s) { return s.drawer.gap; },
     [](Settings& s, int v) { s.drawer.gap = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"圆角半径", 0, 32,
     [](const Settings& s) { return s.drawer.cornerRadius; },
     [](Settings& s, int v) { s.drawer.cornerRadius = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"透明度", 0, 90,
     [](const Settings& s) { return s.drawer.transparency; },
     [](Settings& s, int v) { s.drawer.transparency = v; }, L"%  0=不透明"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"折叠显示字数", 1, 16,
     [](const Settings& s) { return s.drawer.shortNameChars; },
     [](Settings& s, int v) { s.drawer.shortNameChars = v; }, L"字"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"动画时长", 0, 1000,
     [](const Settings& s) { return s.drawer.animationMs; },
     [](Settings& s, int v) { s.drawer.animationMs = v; }, L"ms  0=不动画"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"顶部偏移", 0, 800,
     [](const Settings& s) { return s.drawer.topOffset; },
     [](Settings& s, int v) { s.drawer.topOffset = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"贴合重叠", 0, 24,
     [](const Settings& s) { return s.drawer.attachOverlap; },
     [](Settings& s, int v) { s.drawer.attachOverlap = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"书签外观", L"激活额外长度", 0, 80,
     [](const Settings& s) { return s.drawer.activeExtraExtent; },
     [](Settings& s, int v) { s.drawer.activeExtraExtent = v; }, L"px"},

    // --- 底部横排 ---
    {FieldKind::Int, SettingsPage::Bookmarks, L"底部横排（窗口最大化时）", L"折叠宽度", 24, 240,
     [](const Settings& s) { return s.drawer.bottomCollapsedExtent; },
     [](Settings& s, int v) { s.drawer.bottomCollapsedExtent = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"底部横排（窗口最大化时）", L"展开宽度", 24, 480,
     [](const Settings& s) { return s.drawer.bottomExpandedExtent; },
     [](Settings& s, int v) { s.drawer.bottomExpandedExtent = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"底部横排（窗口最大化时）", L"平时高度", 0, 80,
     [](const Settings& s) { return s.drawer.bottomCollapsedThickness; },
     [](Settings& s, int v) { s.drawer.bottomCollapsedThickness = v; }, L"px  0=厚度一半"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"底部横排（窗口最大化时）", L"激活高度", 0, 80,
     [](const Settings& s) { return s.drawer.bottomActiveThickness; },
     [](Settings& s, int v) { s.drawer.bottomActiveThickness = v; }, L"px  0=同厚度"},

    // --- 悬停预览 ---
    {FieldKind::Bool, SettingsPage::Bookmarks, L"悬停预览", L"启用预览", 0, 1,
     [](const Settings& s) { return s.preview.enabled ? 1 : 0; },
     [](Settings& s, int v) { s.preview.enabled = v != 0; }, nullptr},
    {FieldKind::Int, SettingsPage::Bookmarks, L"悬停预览", L"延迟", 0, 5000,
     [](const Settings& s) { return s.preview.delayMs; },
     [](Settings& s, int v) { s.preview.delayMs = v; }, L"ms"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"悬停预览", L"宽度", 160, 1600,
     [](const Settings& s) { return s.preview.width; },
     [](Settings& s, int v) { s.preview.width = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"悬停预览", L"高度", 100, 1200,
     [](const Settings& s) { return s.preview.height; },
     [](Settings& s, int v) { s.preview.height = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Bookmarks, L"悬停预览", L"圆角半径", 0, 32,
     [](const Settings& s) { return s.preview.cornerRadius; },
     [](Settings& s, int v) { s.preview.cornerRadius = v; }, L"px"},

    // --- 性能 ---
    {FieldKind::Int, SettingsPage::Bookmarks, L"性能", L"几何事件节流", 8, 250,
     [](const Settings& s) { return s.performance.geometryThrottleMs; },
     [](Settings& s, int v) { s.performance.geometryThrottleMs = v; }, L"ms"},

    // --- 边框（独立页面）---
    {FieldKind::Bool, SettingsPage::Borders, L"窗口边框", L"启用边框", 0, 1,
     [](const Settings& s) { return s.border.enabled ? 1 : 0; },
     [](Settings& s, int v) { s.border.enabled = v != 0; }, nullptr},
    {FieldKind::Int, SettingsPage::Borders, L"窗口边框", L"线宽", 1, 20,
     [](const Settings& s) { return s.border.width; },
     [](Settings& s, int v) { s.border.width = v; }, L"px"},
    {FieldKind::Int, SettingsPage::Borders, L"窗口边框", L"偏移", -20, 20,
     [](const Settings& s) { return s.border.offset; },
     [](Settings& s, int v) { s.border.offset = v; }, L"px  负=向内"},
    {FieldKind::Choice, SettingsPage::Borders, L"窗口边框", L"圆角", 0, 4,
     [](const Settings& s) { return static_cast<int>(s.border.corners); },
     [](Settings& s, int v) { s.border.corners = static_cast<BorderCorners>(v); }, nullptr,
     kCornerChoices, static_cast<int>(std::size(kCornerChoices))},
    {FieldKind::Int, SettingsPage::Borders, L"窗口边框", L"自定义圆角", 0, 64,
     [](const Settings& s) { return s.border.cornerRadius; },
     [](Settings& s, int v) { s.border.cornerRadius = v; }, L"px  仅「自定义」时"},

    {FieldKind::Palette, SettingsPage::Borders, L"颜色", L"活动窗口", 0, 0,
     [](const Settings& s) { return static_cast<int>(s.border.activeColor); },
     [](Settings& s, int v) { s.border.activeColor = static_cast<unsigned>(v); },
     nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
     kBorderActiveSwatches, static_cast<int>(std::size(kBorderActiveSwatches))},
    {FieldKind::Palette, SettingsPage::Borders, L"颜色", L"非活动窗口", 0, 0,
     [](const Settings& s) { return static_cast<int>(s.border.inactiveColor); },
     [](Settings& s, int v) { s.border.inactiveColor = static_cast<unsigned>(v); },
     nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
     kBorderInactiveSwatches, static_cast<int>(std::size(kBorderInactiveSwatches))},

    // --- 排除窗口 ---
    // Lives on the border page because that is where an unwanted outline is noticed, but
    // it excludes the window from bookmarks too - the shell chrome that needs silencing
    // never wanted either.
    {FieldKind::Text, SettingsPage::Borders, L"排除窗口（用 WindowMarkInspect.exe 查类名）",
     L"类名", 0, 0, nullptr, nullptr, nullptr, nullptr, 0,
     [](const Settings& s) { return JoinClasses(s.tracking.excludeClasses); },
     [](Settings& s, const std::wstring& v) { s.tracking.excludeClasses = SplitClasses(v); }},

    // --- 窗口置顶 ---
    {FieldKind::Bool, SettingsPage::Pinning, L"窗口置顶", L"启用置顶", 0, 1,
     [](const Settings& s) { return s.pin.enabled ? 1 : 0; },
     [](Settings& s, int v) { s.pin.enabled = v != 0; }, nullptr},
    {FieldKind::Palette, SettingsPage::Pinning, L"窗口置顶", L"高亮颜色", 0, 0,
     [](const Settings& s) { return static_cast<int>(s.pin.color); },
     [](Settings& s, int v) { s.pin.color = static_cast<unsigned>(v); },
     nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
     kPinSwatches, static_cast<int>(std::size(kPinSwatches))},
    {FieldKind::Int, SettingsPage::Pinning, L"窗口置顶", L"线宽", 1, 20,
     [](const Settings& s) { return s.pin.width; },
     [](Settings& s, int v) { s.pin.width = v; }, L"px  比普通边框粗"},
    // Empty by default. RegisterHotKey is first-come, first-served for the whole session,
    // so this app does not take a combination away from anything else unless asked to.
    {FieldKind::Hotkey, SettingsPage::Pinning, L"窗口置顶", L"快捷键", 0, 0,
     nullptr, nullptr, L"退格键清除", nullptr, 0,
     [](const Settings& s) { return FormatHotkeyWide(ParseHotkey(s.pin.hotkey)); },
     [](Settings& s, const std::wstring& v) {
         // Normalised on the way in so the settings file never records a spelling the
         // parser would later read back differently.
         s.pin.hotkey = FormatHotkey(ParseHotkey(WideToUtf8(v)));
     }},
};

// Column assignment is by group, chosen so the two columns end at roughly the same
// height. Appearance alone is 11 rows, so it pairs with the 2-row behaviour block;
// everything else stacks on the right. Getting this wrong pushes the taller column
// down into the button row.
bool IsLeftColumn(const Field& field) {
    if (field.page != SettingsPage::Bookmarks) {
        return true;  // single column; every group stacks
    }
    return wcscmp(field.group, L"书签") == 0 || wcscmp(field.group, L"书签外观") == 0;
}

// 0xAARRGGBB in, "#RRGGBB" or "#RRGGBBAA" out - alpha is only spelled out when it matters.
std::wstring ColorToText(int argb) {
    const unsigned value = static_cast<unsigned>(argb);
    wchar_t buffer[16]{};
    if (((value >> 24) & 0xFFu) == 0xFFu) {
        swprintf_s(buffer, L"#%06X", value & 0xFFFFFFu);
    } else {
        swprintf_s(buffer, L"#%06X%02X", value & 0xFFFFFFu, (value >> 24) & 0xFFu);
    }
    return buffer;
}

// Accepts #RGB, #RGBA, #RRGGBB and #RRGGBBAA, with or without the '#'. Returns -1 when
// unusable so Collect can complain instead of silently producing a wrong colour.
int ColorFromText(const std::wstring& text) {
    std::wstring digits = text;
    digits.erase(0, digits.find_first_not_of(L" \t"));
    const auto tail = digits.find_last_not_of(L" \t");
    if (tail != std::wstring::npos) digits.erase(tail + 1);
    if (!digits.empty() && digits.front() == L'#') digits.erase(digits.begin());

    std::vector<int> n;
    n.reserve(digits.size());
    for (wchar_t c : digits) {
        if (c >= L'0' && c <= L'9') n.push_back(c - L'0');
        else if (c >= L'a' && c <= L'f') n.push_back(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') n.push_back(c - L'A' + 10);
        else return -1;
    }

    int r = 0, g = 0, b = 0, a = 255;
    switch (n.size()) {
    case 3: r = n[0]*17; g = n[1]*17; b = n[2]*17; break;
    case 4: r = n[0]*17; g = n[1]*17; b = n[2]*17; a = n[3]*17; break;
    case 6: r = n[0]*16+n[1]; g = n[2]*16+n[3]; b = n[4]*16+n[5]; break;
    case 8: r = n[0]*16+n[1]; g = n[2]*16+n[3]; b = n[4]*16+n[5]; a = n[6]*16+n[7]; break;
    default: return -1;
    }
    return static_cast<int>((static_cast<unsigned>(a) << 24) | (static_cast<unsigned>(r) << 16) |
                            (static_cast<unsigned>(g) << 8) | static_cast<unsigned>(b));
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
    Dialog(Settings& settings, SettingsPage page) : working_(settings), page_(page) {}

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
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        return RegisterSwatchClass();
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

        // Each page is sized to its own content; sizing both the same would leave most of
        // the border window empty.
        const PageMetrics pm = MetricsFor(page_);
        RECT bounds{0, 0, m_.Scale(pm.width), m_.Scale(pm.height)};
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

        // WS_EX_TOPMOST because this app can put other windows in the topmost band.
        // A normal window can never be raised above a topmost one, so once anything
        // is pinned the settings window opens underneath it and reads as "the menu
        // item did nothing". The rename dialog already carries this for the same reason.
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kSettingsClass,
            page_ == SettingsPage::Pinning  ? L"WindowMark - 窗口置顶设置"
            : page_ == SettingsPage::Borders ? L"WindowMark - 窗口边框设置"
                                             : L"WindowMark - 书签设置",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            x, y, outerW, outerH,
            owner_, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        // Title bar and Alt+Tab. Two sizes, because Windows asks for them separately and
        // takes the wrong one from the .ico if only one is set.
        // Not named `small`/`large`: rpcndr.h defines `small` as a macro for char.
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (HICON smallIcon = static_cast<HICON>(LoadImageW(
                instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR))) {
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        }
        if (HICON bigIcon = static_cast<HICON>(LoadImageW(
                instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR))) {
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
        }

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
        const PageMetrics pm = MetricsFor(page_);
        int leftY = kPad;
        int rightY = kPad;
        const wchar_t* currentGroup = nullptr;
        int id = kFirstFieldId;

        for (const auto& field : kFields) {
            if (field.page != page_) {
                // Keep controls_ index-aligned with kFields so Load/Collect can walk both.
                controls_.push_back(nullptr);
                ++id;
                continue;
            }
            const int columnW = pm.columnW;
            const bool left = pm.columns == 1 || IsLeftColumn(field);
            int& cursor = left ? leftY : rightY;
            const int columnX = left ? kPad : kPad + columnW + kColumnGap;

            if (!currentGroup || wcscmp(currentGroup, field.group) != 0) {
                currentGroup = field.group;
                cursor += kGroupGap;
                Add(L"STATIC", field.group, SS_LEFTNOWORDWRAP,
                    columnX, cursor, columnW, kRowH, -1);
                cursor += kRowH + 2;
                Add(L"STATIC", L"", SS_ETCHEDHORZ, columnX, cursor, columnW, 1, -1);
                cursor += 6;
            }

            Add(L"STATIC", field.label, SS_LEFTNOWORDWRAP,
                columnX + kIndent, cursor + 3, pm.labelW, kRowH, -1);

            const int controlX = columnX + kIndent + pm.labelW;
            switch (field.kind) {
            case FieldKind::Int:
                controls_.push_back(Add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT,
                                        controlX, cursor, kEditW, kRowH, id));
                if (field.hint) {
                    Add(L"STATIC", field.hint, SS_LEFTNOWORDWRAP,
                        controlX + kEditW + kFieldGap, cursor + 3, pm.hintW, kRowH, -1);
                }
                break;
            case FieldKind::Palette: {
                // The strip paints its own caption, so there is no separate hint control
                // to keep in sync with it.
                auto* data = new SwatchData{};
                data->presets = field.swatches;
                data->presetCount = field.swatchCount;
                data->accentFirst = field.swatchCount > 0 &&
                                    field.swatches[0] == PinSettings::kAccentColor;
                data->cell = m_.Scale(kSwatchCell);
                data->gap = m_.Scale(kSwatchGap);
                data->ring = m_.Scale(kSwatchRing);
                data->textX = m_.Scale(kSwatchStripW + kFieldGap);
                data->font = font_;
                HWND strip = CreateWindowExW(
                    0, kSwatchClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    m_.Scale(controlX), m_.Scale(cursor),
                    m_.Scale(kSwatchStripW + kFieldGap + kSwatchTextW),
                    m_.Scale(kPaletteRowH),
                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), data);
                if (!strip) delete data;   // no WM_DESTROY will arrive to free it
                controls_.push_back(strip);
                break;
            }
            case FieldKind::Bool:
                controls_.push_back(Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX,
                                        controlX, cursor + 2, kEditW, kRowH, id));
                break;
            case FieldKind::Text:
                // Spans the whole control area: a comma-separated list of window class
                // names is long, and there is no hint to sit beside it.
                controls_.push_back(Add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                        controlX, cursor, pm.controlSpan, kRowH, id));
                break;
            case FieldKind::Hotkey: {
                HWND edit = Add(L"EDIT", L"", WS_TABSTOP | WS_BORDER, controlX, cursor,
                                kHotkeyEditW, kRowH, id);
                if (edit) {
                    SetWindowSubclass(edit, HotkeyEditProc, 0, 0);
                    // Grey placeholder while empty, so the box explains itself without
                    // spending a hint column on it. EM_SETCUEBANNER, spelled out because
                    // the macro needs a comctl32 version guard this file does not set.
                    constexpr UINT kSetCueBanner = 0x1500 + 1;
                    SendMessageW(edit, kSetCueBanner, TRUE,
                                 reinterpret_cast<LPARAM>(L"按下组合键"));
                }
                controls_.push_back(edit);
                if (field.hint) {
                    Add(L"STATIC", field.hint, SS_LEFTNOWORDWRAP,
                        controlX + kHotkeyEditW + kFieldGap, cursor + 3, kHotkeyHintW,
                        kRowH, -1);
                }
                break;
            }
            case FieldKind::Choice: {
                HWND combo = Add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                 controlX, cursor, kComboW, kRowH * 8, id);
                for (int c = 0; c < field.choiceCount; ++c) {
                    SendMessageW(combo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(field.choices[c]));
                }
                controls_.push_back(combo);
                break;
            }
            }
            ++id;
            cursor += (field.kind == FieldKind::Palette ? kPaletteRowH : kRowH) + kRowGap;
        }

        // Guard the invariant this layout depends on: neither column may reach the footer.
        // If a future field pushes it over, fail loudly here instead of silently drawing
        // controls on top of the buttons.
        const int width = pm.width;
        const int buttonY = pm.height - kPad - kButtonH;
        const int footerY = buttonY - kFooterH;
        const int tallest = std::max(leftY, rightY);
        if (tallest > footerY - kRowGap) {
            OutputDebugStringW(L"[WindowMark] settings dialog: fields overflow the footer; "
                               L"raise PageHeight or rebalance IsLeftColumn.\n");
        }

        // Footer on its own line rather than squeezed between the buttons: the border page
        // is only wide enough for the three buttons, and a line of its own reads better on
        // the wide page too.
        const std::wstring footer =
            std::wstring(L"WindowMark ") + app::kProductVersion + L" · 改动立即生效并保存";
        Add(L"STATIC", footer.c_str(), SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            kPad, footerY, width - kPad * 2, kFooterH, -1);

        Add(L"BUTTON", L"恢复默认值", WS_TABSTOP, kPad, buttonY, kButtonW, kButtonH, kResetId);
        Add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON,
            width - kPad - kButtonW * 2 - kFieldGap, buttonY, kButtonW, kButtonH, kOkId);
        Add(L"BUTTON", L"取消", WS_TABSTOP, width - kPad - kButtonW, buttonY, kButtonW, kButtonH, kCancelId);
    }

    void Load(const Settings& source) {
        for (std::size_t i = 0; i < std::size(kFields) && i < controls_.size(); ++i) {
            const auto& field = kFields[i];
            HWND control = controls_[i];
            if (!control) continue;
            if (field.kind == FieldKind::Text || field.kind == FieldKind::Hotkey) {
                SetWindowTextW(control, field.getText(source).c_str());
                continue;
            }
            const int value = field.getExternal ? field.getExternal() : field.get(source);
            switch (field.kind) {
            case FieldKind::Int:
                SetWindowTextW(control, std::to_wstring(value).c_str());
                break;
            case FieldKind::Palette:
                if (auto* data = SwatchOf(control)) {
                    data->value = static_cast<unsigned>(value);
                    // Park the focus ring on whatever is selected, so arrowing starts from
                    // the current colour rather than from the left edge.
                    const int index = SwatchIndexOf(*data, data->value);
                    data->focus = index < 0 ? data->presetCount : index;
                    InvalidateRect(control, nullptr, TRUE);
                }
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
        // Registry-backed fields are written only once every control has validated, so a
        // typo in some unrelated number cannot leave half the dialog applied.
        std::vector<std::pair<void (*)(int), int>> external;
        for (std::size_t i = 0; i < std::size(kFields) && i < controls_.size(); ++i) {
            const auto& field = kFields[i];
            HWND control = controls_[i];
            if (!control) continue;

            if (field.kind == FieldKind::Text || field.kind == FieldKind::Hotkey) {
                // No length cap on the class list beyond this buffer; a few dozen class
                // names is far more than anyone will ever exclude.
                std::wstring text(1024, L'\0');
                const int copied = GetWindowTextW(control, text.data(),
                                                  static_cast<int>(text.size()));
                text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
                field.setText(draft, text);
                continue;
            }

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
            case FieldKind::Palette: {
                // Nothing to validate: every value the strip can hold came from a preset
                // or from the system colour picker, so there is no typo to catch.
                auto* data = SwatchOf(control);
                value = data ? static_cast<int>(data->value) : 0;
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
            if (field.setExternal) {
                external.emplace_back(field.setExternal, value);
            } else {
                field.set(draft, value);
            }
        }

        for (const auto& [apply, value] : external) apply(value);

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

    // Every width below is derived from the widest thing it has to hold, so the dialog is
    // as narrow as its content allows instead of a round number with air in it.
    static constexpr int kPad = 12;
    static constexpr int kIndent = 8;
    // Four digits, right-aligned.
    static constexpr int kEditW = 54;
    // "#RRGGBBAA" plus the caret.
    static constexpr int kColorEditW = 78;
    // 色卡条：6 个预设 + 1 个自定义 = 7 格。格子 18px，间距 7px，
    // 选中环画在格子外面，所以行高比普通行多 4px。
    static constexpr int kSwatchCell = 18;
    static constexpr int kSwatchGap = 7;
    // 色块到选中环之间留出的空隙。行高要容得下 kSwatchCell + kSwatchRing*2 + 环宽。
    static constexpr int kSwatchRing = 4;
    static constexpr int kSwatchCount = 7;
    static constexpr int kSwatchStripW =
        kSwatchCell * kSwatchCount + kSwatchGap * (kSwatchCount - 1);
    // 「跟随系统 #RRGGBB」是最长的一种说明文字。
    static constexpr int kSwatchTextW = 118;
    static constexpr int kPaletteRowH = 30;
    // Wide enough for the combinations anyone actually binds ("Ctrl+Shift+F12"); the
    // pathological "Ctrl+Alt+Shift+Win+PAGEDOWN" scrolls, which beats widening the whole
    // dialog for a case nobody types.
    static constexpr int kHotkeyEditW = 132;
    static constexpr int kHotkeyHintW = 66;   // 「退格键清除」
    // "#RRGGBB[AA]", the only hint that ever sits next to a colour. SS_LEFTNOWORDWRAP
    // clips rather than wraps, so these have to be measured generously - 78 lost the
    // closing bracket.
    static constexpr int kColorHintW = 86;
    // "跟随系统" plus the drop-down arrow - the longest choice on either page. Sized to
    // its content rather than stretched to the edit+hint span, which left it looking like
    // a text field that had been dragged out.
    static constexpr int kComboW = 100;
    static constexpr int kFieldGap = 6;
    static constexpr int kColumnGap = 14;

    // The label and hint columns are measured per page, not once for both. Sharing them
    // meant every page paid for the other page's longest string: the border page carried
    // a label column sized for 「折叠显示字数」 and the bookmark page a hint column sized
    // for 「px  仅「自定义」时」, which is where the empty channels came from.
    struct PageMetrics {
        int labelW;
        int hintW;
        int columns;
        int height;
        int controlSpan;  // everything right of the label, whichever row needs the most
        int columnW;
        int width;
    };

    // Derived from the field table rather than hand-tuned. The two page heights used to be
    // literals that had to be re-measured by eye every time a field was added, which is
    // exactly the step that gets skipped - and the failure mode is controls drawn on top
    // of the OK button. This walks the same group/row arithmetic BuildControls does.
    [[nodiscard]] static constexpr int PageHeight(SettingsPage page, int columns) {
        int leftY = kPad;
        int rightY = kPad;
        const wchar_t* currentGroup = nullptr;
        for (const auto& field : kFields) {
            if (field.page != page) continue;
            const bool left = columns == 1 || IsLeftColumn(field);
            int& cursor = left ? leftY : rightY;
            if (!currentGroup || wcscmp(currentGroup, field.group) != 0) {
                currentGroup = field.group;
                cursor += kGroupGap + kRowH + 2 + 1 + 6;
            }
            cursor += (field.kind == FieldKind::Palette ? kPaletteRowH : kRowH) + kRowGap;
        }
        const int tallest = leftY > rightY ? leftY : rightY;
        // Room below the last field for the footer line and the button row.
        return tallest + kRowGap + kFooterH + kButtonH + kPad * 2;
    }

    [[nodiscard]] static constexpr PageMetrics MetricsFor(SettingsPage page) {
        PageMetrics m{};
        bool hasColour = false;   // 现在指的是色卡条
        bool hasHotkey = false;
        if (page == SettingsPage::Pinning) {
            hasHotkey = true;
            m.labelW = 66;   // 「高亮颜色」
            m.hintW = 116;   // 「px  比普通边框粗」
            m.columns = 1;
            hasColour = true;
        } else if (page == SettingsPage::Borders) {
            m.labelW = 66;   // 「自定义圆角」「非活动窗口」
            m.hintW = 116;   // 「px  仅「自定义」时」
            m.columns = 1;   // eight fields; two columns left half the window empty
            hasColour = true;
        } else {
            m.labelW = 80;   // 「折叠显示字数」「几何事件节流」
            m.hintW = 86;    // 「px  0=厚度一半」
            m.columns = 2;
        }
        m.height = PageHeight(page, m.columns);
        const int numberSpan = kEditW + kFieldGap + m.hintW;
        const int colourSpan = hasColour ? kSwatchStripW + kFieldGap + kSwatchTextW : 0;
        const int hotkeySpan = hasHotkey ? kHotkeyEditW + kFieldGap + kHotkeyHintW : 0;
        m.controlSpan = numberSpan > colourSpan ? numberSpan : colourSpan;
        if (hotkeySpan > m.controlSpan) m.controlSpan = hotkeySpan;
        m.columnW = kIndent + m.labelW + m.controlSpan;
        const int content = kPad * 2 + m.columnW * m.columns + kColumnGap * (m.columns - 1);
        // The button row has its own minimum. Fields alone made the single-column border
        // page 260 wide, which is less than three buttons need, and they drew on top of
        // each other. Whichever is wider wins.
        m.width = content > kButtonRowW ? content : kButtonRowW;
        return m;
    }
    static constexpr int kRowH = 22;
    static constexpr int kRowGap = 3;
    static constexpr int kGroupGap = 12;
    static constexpr int kButtonW = 76;  // 「恢复默认值」, five glyphs
    static constexpr int kButtonH = 28;
    static constexpr int kButtonGap = 16;  // between 恢复默认值 and the 确定/取消 pair
    static constexpr int kFooterH = 18;
    static constexpr int kButtonRowW =
        kPad * 2 + kButtonW * 3 + kButtonGap + kFieldGap;


    Settings& working_;
    SettingsPage page_{};
    Metrics m_;
    HWND owner_{};
    HWND hwnd_{};
    HFONT font_{};
    std::vector<HWND> controls_;
    bool accepted_{false};
};

} // namespace

bool WinSettingsDialog::ShowModal(HWND owner, Settings& settings, SettingsPage page) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Dialog dialog(settings, page);
    return dialog.Run(owner);
}

} // namespace windowmark::win
