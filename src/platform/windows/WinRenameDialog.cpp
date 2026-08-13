#include "WinRenameDialog.h"

#include <commctrl.h>

#include <algorithm>
#include <array>

namespace windowmark::win {
namespace {

constexpr wchar_t kRenameClass[] = L"WindowMark.RenameDialog";
constexpr int kEditId = 4100;
constexpr int kOkId = 4101;
constexpr int kCancelId = 4102;
constexpr int kClearId = 4103;
constexpr std::size_t kMaxNameChars = 64;

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
    Prompt(const std::wstring& title, std::wstring& name) : windowTitle_(title), name_(name) {}

    bool Run(HWND owner) {
        owner_ = owner;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kRenameClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        dpi_ = owner_ ? static_cast<int>(GetDpiForWindow(owner_)) : 96;
        if (dpi_ <= 0) dpi_ = 96;

        RECT bounds{0, 0, S(kWidth), S(kHeight)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
        const int outerW = bounds.right - bounds.left;
        const int outerH = bounds.bottom - bounds.top;

        POINT cursor{};
        GetCursorPos(&cursor);
        int x = cursor.x - outerW / 2;
        int y = cursor.y - outerH - S(12);
        if (y < 0) y = cursor.y + S(12);

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kRenameClass, L"重命名书签",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            x, y, outerW, outerH,
            owner_, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        font_ = CreateUiFont(dpi_);
        Build();

        const bool ownerWasEnabled = owner_ && IsWindowEnabled(owner_);
        if (ownerWasEnabled) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (edit_) {
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
        }

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

    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND control = CreateWindowExW(
            0, cls, text, WS_CHILD | WS_VISIBLE | style,
            S(x), S(y), S(w), S(h),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (control && font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return control;
    }

    void Build() {
        Add(L"STATIC", L"书签显示的名称：", SS_LEFTNOWORDWRAP, 14, 12, 300, 18, -1);
        edit_ = Add(L"EDIT", name_.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 14, 34, 332, 24, kEditId);
        if (edit_) SendMessageW(edit_, EM_SETLIMITTEXT, kMaxNameChars, 0);

        const std::wstring hint = L"留空则恢复为窗口标题：" + Ellipsize(windowTitle_, 28);
        Add(L"STATIC", hint.c_str(), SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 14, 64, 332, 18, -1);
        Add(L"STATIC", L"名称只在本次运行期间有效。", SS_LEFTNOWORDWRAP, 14, 84, 332, 18, -1);

        Add(L"BUTTON", L"清除", WS_TABSTOP, 14, 112, 70, 26, kClearId);
        Add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 178, 112, 80, 26, kOkId);
        Add(L"BUTTON", L"取消", WS_TABSTOP, 266, 112, 80, 26, kCancelId);
    }

    static std::wstring Ellipsize(const std::wstring& text, std::size_t limit) {
        if (text.size() <= limit) return text;
        return text.substr(0, limit) + L"...";
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

    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case kOkId: {
                std::array<wchar_t, kMaxNameChars + 1> buffer{};
                GetWindowTextW(edit_, buffer.data(), static_cast<int>(buffer.size()));
                name_.assign(buffer.data());
                Trim(name_);
                accepted_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            case kClearId:
                SetWindowTextW(edit_, L"");
                SetFocus(edit_);
                return 0;
            case kCancelId:
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
            // Nested inside the app's message loop; clearing hwnd_ ends the modal pump
            // without posting WM_QUIT and taking the tray app down with it.
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    static void Trim(std::wstring& value) {
        const auto notSpace = [](wchar_t c) { return !iswspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    }

    static constexpr int kWidth = 360;
    static constexpr int kHeight = 150;

    std::wstring windowTitle_;
    std::wstring& name_;
    HWND owner_{};
    HWND hwnd_{};
    HWND edit_{};
    HFONT font_{};
    int dpi_{96};
    bool accepted_{false};
};

} // namespace

bool WinRenameDialog::ShowModal(HWND owner, const std::wstring& windowTitle, std::wstring& name) {
    Prompt prompt(windowTitle, name);
    return prompt.Run(owner);
}

} // namespace windowmark::win
