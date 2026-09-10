#include "WinDragSettingsDialog.h"

#include "windowmark/core/DragModifiers.h"

#include <algorithm>
#include <vector>

namespace windowmark::win {
namespace {

constexpr wchar_t kDragSettingsClass[] = L"WindowMark.DragSettingsDialog";

constexpr int kNoteId = 4300;
constexpr int kFirstPresetId = 4301;   // 六个复选框占 4301..4306
constexpr int kHintId = 4310;
constexpr int kOkId = 4311;
constexpr int kCancelId = 4312;

constexpr int kWidth = 452;
constexpr int kHeight = 262;

HFONT CreateUiFont(int dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(dpi))) {
        return nullptr;
    }
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

class Prompt {
public:
    explicit Prompt(std::string& modifiers) : modifiers_(modifiers) {
        current_ = ParseDragModifiers(modifiers);
    }

    bool Run(HWND owner) {
        owner_ = owner;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kDragSettingsClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        dpi_ = owner_ ? static_cast<int>(GetDpiForWindow(owner_)) : 96;
        if (dpi_ <= 0) dpi_ = 96;

        RECT bounds{0, 0, S(kWidth), S(kHeight)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_DLGMODALFRAME);
        const int outerW = bounds.right - bounds.left;
        const int outerH = bounds.bottom - bounds.top;

        POINT cursor{};
        GetCursorPos(&cursor);
        int x = cursor.x - outerW / 2;
        int y = cursor.y - outerH - S(12);
        if (y < 0) y = cursor.y + S(12);
        if (x < 0) x = 0;

        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kDragSettingsClass,
                                L"WindowMark - 拖动设置",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, outerW,
                                outerH, owner_, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        font_ = CreateUiFont(dpi_);
        Build();

        const bool ownerWasEnabled = owner_ && IsWindowEnabled(owner_);
        if (ownerWasEnabled) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);

        MSG msg{};
        while (hwnd_) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (hwnd_ && IsDialogMessageW(hwnd_, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (ownerWasEnabled) EnableWindow(owner_, TRUE);
        if (font_) DeleteObject(font_);
        return accepted_;
    }

private:
    int S(int value) const { return MulDiv(value, dpi_, 96); }

    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
             int h, int id) {
        HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x),
                                       S(y), S(w), S(h), hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       GetModuleHandleW(nullptr), nullptr);
        if (control && font_) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        return control;
    }

    void Build() {
        Add(L"STATIC",
            L"按住下面任意一个键，再用鼠标拖动窗口：\n"
            L"左键拖 = 移动，右键拖 = 缩放（按鼠标在窗口内的位置决定拉哪条边）",
            0, 14, 12, 424, 40, kNoteId);

        const auto& presets = PresetModifiers();
        for (std::size_t i = 0; i < presets.size(); ++i) {
            const int col = static_cast<int>(i) % 2;
            const int row = static_cast<int>(i) / 2;
            const int id = kFirstPresetId + static_cast<int>(i);
            Add(L"BUTTON", presets[i].label,
                WS_TABSTOP | BS_AUTOCHECKBOX | (i == 0 ? WS_GROUP : 0), 14 + col * 212,
                60 + row * 30, 202, 26, id);
            CheckDlgButton(hwnd_, id,
                           current_.Contains(presets[i].vk) ? BST_CHECKED : BST_UNCHECKED);
        }

        Add(L"STATIC",
            L"配置文件里还可以写其他按键（如 F13），这里不显示，但确定时不会被抹掉。\n"
            L"勾选 Win 键会额外装一个键盘钩子，用来吞掉抬起时弹出的开始菜单；\n"
            L"只用 Alt / Ctrl 时不会安装。",
            0, 14, 156, 424, 56, kHintId);

        Add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 218, 84, 28, kOkId);
        Add(L"BUTTON", L"取消", WS_TABSTOP, 344, 218, 84, 28, kCancelId);
    }

    [[nodiscard]] std::string Collect() const {
        // 先把配置文件里那些非预设的键原样搬过来。界面不显示它们，更不该悄悄删掉——
        // 用户手写进配置的东西，从设置界面点一次确定就消失，是最让人措手不及的那种
        // 数据丢失。
        DragModifiers next;
        const auto& presets = PresetModifiers();
        for (const unsigned vk : current_.keys) {
            const bool isPreset =
                std::any_of(presets.begin(), presets.end(),
                            [vk](const PresetModifier& p) { return p.vk == vk; });
            if (!isPreset) next.keys.push_back(vk);
        }
        for (std::size_t i = 0; i < presets.size(); ++i) {
            const int id = kFirstPresetId + static_cast<int>(i);
            if (IsDlgButtonChecked(hwnd_, id) == BST_CHECKED &&
                !next.Contains(presets[i].vk)) {
                next.keys.push_back(presets[i].vk);
            }
        }
        // FormatDragModifiers 只认规范顺序，而这里是按「非预设在前」拼起来的。
        // 走一趟解析把它归位——不然写回去的字符串顺序和下次读出来的对不上。
        return FormatDragModifiers(ParseDragModifiers(FormatDragModifiers(next)));
    }

    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case kOkId:
                modifiers_ = Collect();
                accepted_ = true;
                DestroyWindow(hwnd_);
                return 0;
            case kCancelId:
            case IDCANCEL:
                DestroyWindow(hwnd_);
                return 0;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            // 只清句柄，**不发 PostQuitMessage**：这个模态循环嵌在托盘应用自己的消息
            // 循环里，发 WM_QUIT 会把整个应用带走。
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Prompt*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Prompt*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->Handle(msg, wParam, lParam);
    }

    std::string& modifiers_;
    DragModifiers current_;
    HWND owner_{};
    HWND hwnd_{};
    HFONT font_{};
    int dpi_{96};
    bool accepted_{false};
};

} // namespace

bool WinDragSettingsDialog::ShowModal(HWND owner, std::string& modifiers) {
    Prompt prompt(modifiers);
    return prompt.Run(owner);
}

} // namespace windowmark::win
