#include "WinMouseControl.h"
#include <commctrl.h>
#include <shellscalingapi.h>

namespace windowmark::win {
namespace {
class MouseDialog {
public:
    MouseSettings value;
    HWND window{}, wheel{}, pointer{}, interval{}, speed{};
    bool accepted{};
    int dpi{96};
    HFONT font{};
    ~MouseDialog() { if (font) DeleteObject(font); }
    int S(int x) const { return MulDiv(x, dpi, 96); }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        const auto control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
            S(x), S(y), S(w), S(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return control;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<MouseDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<MouseDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == WM_COMMAND) {
            if (LOWORD(wp) == IDOK) {
                BOOL intervalOk{}, speedOk{};
                const auto interval = GetDlgItemInt(hwnd, 11, &intervalOk, FALSE);
                const auto speed = GetDlgItemInt(hwnd, 12, &speedOk, FALSE);
                if (!intervalOk || interval < 1 || interval > 1000 || !speedOk || speed < 1 || speed > 20) {
                    MessageBoxW(hwnd, L"滚轮间隔请输入 1–1000 毫秒，指针速度请输入 1–20。", L"鼠标控制", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                self->value.wheelEnabled = SendMessageW(self->wheel, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->value.pointerEnabled = SendMessageW(self->pointer, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->value.wheelIntervalMs = static_cast<int>(interval); self->value.pointerSpeed = static_cast<int>(speed);
                self->accepted = true; DestroyWindow(hwnd); return 0;
            }
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        }
        if (self && msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    bool Run(HWND owner) {
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Proc;
        cls.lpszClassName = L"WindowMark.MouseControl"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        POINT point{}; GetCursorPos(&point); MONITORINFO info{sizeof(info)};
        const auto monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor, &info);
        UINT x{96}, y{96}; GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y);
        dpi = static_cast<int>(x);
        NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, x))
            font = CreateFontIndirectW(&metrics.lfMessageFont);
        RECT rect{0, 0, S(490), S(300)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, static_cast<UINT>(dpi));
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        if (!CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"鼠标控制", style,
            info.rcWork.left + (info.rcWork.right-info.rcWork.left-width)/2,
            info.rcWork.top + (info.rcWork.bottom-info.rcWork.top-height)/2,
            width, height, owner, nullptr, cls.hInstance, this)) return false;
        wheel = Add(L"BUTTON", L"启用滚轮调节", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 16, 220, 26, 10);
        Add(L"STATIC", L"间隔（毫秒，越大越慢）：", 0, 16, 52, 245, 24, -1);
        interval = Add(L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 280, 50, 80, 26, 11);
        SetDlgItemInt(window, 11, static_cast<UINT>(value.wheelIntervalMs), FALSE);
        Add(L"STATIC", L"推荐先用 50；仍快可改为 80 或 100。每次放行一格。", 0, 16, 83, 460, 25, -1);
        pointer = Add(L"BUTTON", L"启用指针调节", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 120, 220, 26, 13);
        Add(L"STATIC", L"指针速度（1 慢 — 20 快）：", 0, 16, 156, 245, 24, -1);
        speed = Add(L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 280, 154, 80, 26, 12);
        SetDlgItemInt(window, 12, static_cast<UINT>(value.pointerSpeed), FALSE);
        Add(L"STATIC", L"关闭或正常退出恢复原速度。远程绝对坐标可能不受影响。", 0, 16, 188, 460, 24, -1);
        Add(L"STATIC", L"两项互相独立，默认都关闭；保存后立即生效。", 0, 16, 220, 460, 22, -1);
        Add(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 292, 258, 80, 28, IDOK);
        Add(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 388, 258, 80, 28, IDCANCEL);
        SendMessageW(wheel, BM_SETCHECK, value.wheelEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(pointer, BM_SETCHECK, value.pointerEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(owner, FALSE); ShowWindow(window, SW_SHOW); SetForegroundWindow(window);
        MSG message{};
        while (IsWindow(window)) {
            const auto result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) { if (!result) PostQuitMessage(static_cast<int>(message.wParam)); break; }
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (IsWindow(window)) DestroyWindow(window);
        EnableWindow(owner, TRUE);
        return accepted;
    }
};
}
bool ShowMouseSettings(HWND owner, MouseSettings& settings) {
    MouseDialog dialog; dialog.value = settings;
    if (!dialog.Run(owner)) return false;
    settings = dialog.value; return true;
}
}
