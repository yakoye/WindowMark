#include "WinHomePanel.h"

#include "AppIdentity.h"
#include "Resource.h"

#include <shellscalingapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <string>

namespace windowmark::win {
namespace {

constexpr wchar_t kPanelClass[] = L"WindowMark.HomePanel";

// 所有尺寸都是 96dpi 下的逻辑像素，画之前统一过 Scale。
constexpr int kPad = 16;
constexpr int kWidth = 620;
constexpr int kLineH = 18;        // 一行说明文字
constexpr int kRowH = 24;         // 复选框 / 按钮一行
constexpr int kButtonW = 92;      // 「设置...」
constexpr int kAppsButtonW = 104; // 「排除应用...」
constexpr int kGap = 6;
constexpr int kSectionGap = 12;

enum : int {
    kIdClose = IDCANCEL,
    kIdToggleBase = 2000,   // +feature
    kIdSettingsBase = 2010, // +feature
    kIdAppsBase = 2020,     // +feature
    kIdPinLast = 2040,
    kIdAutoStart = 2041,
    kIdDesktopShortcut = 2042,
    kIdConfigPath = 2043,
    kIdClipKeeper = 2044,
    kIdDiagnose = 2049,
    kIdInstall = 2045,
    kIdAbout = 2046,
    kIdExit = 2047,
    kIdPinGrab = 2048,
};

struct FeatureText {
    HomeFeature feature;
    const wchar_t* name;
    const wchar_t* what;
    const wchar_t* how;
    bool hasApps;   // 有没有「排除应用...」
};

// 说明写成「这东西是什么」+「手上怎么操作」两行。写长了没人看，写成功能清单又等于没说。
constexpr std::array<FeatureText, 4> kFeatures{{
    {HomeFeature::Bookmarks, L"窗口书签",
     L"同一个程序开着的几个窗口，共享一排书签贴在窗口内侧，点一下直接切过去。",
     L"用法：鼠标移到窗口下沿那排彩色小块上，离鼠标越近的越大；左键切换窗口，右键改名。",
     true},
    {HomeFeature::Borders, L"窗口边框",
     L"给每个窗口描一圈边，当前窗口和其他窗口两种颜色，一眼看出焦点在哪。",
     L"用法：开着就有，不用操作。最大化的窗口不画（边框在窗口外 3px，最大化时那 3px 不存在）。",
     true},
    {HomeFeature::Pinning, L"窗口置顶",
     L"把窗口钉在最上层，并给它一条高亮边框表示已经钉住；退出时会恢复原样。",
     L"用法：下面那个「置顶刚才那个窗口」按钮，或在窗口标题栏右键选「置于顶层」；也可以设快捷键。",
     false},
    {HomeFeature::Drag, L"窗口拖动",
     L"按住一个修饰键，在窗口里任意位置拖动就能搬动它，不用够到标题栏。",
     L"用法：默认关着。打开后按住右 Alt 拖动；按一下不拖 = 窗口居中，用来救跑到屏幕外的窗口。",
     true},
}};

[[nodiscard]] bool FeatureEnabled(const Settings& settings, HomeFeature feature) {
    switch (feature) {
    case HomeFeature::Bookmarks: return settings.drawer.enabled;
    case HomeFeature::Borders:   return settings.border.enabled;
    case HomeFeature::Pinning:   return settings.pin.enabled;
    case HomeFeature::Drag:      return settings.drag.enabled;
    }
    return false;
}

HFONT CreateUiFont(int dpi, bool bigger) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(dpi))) {
        return nullptr;
    }
    if (bigger) {
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 13, 10);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    }
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

// 标题太长时截断。按钮上写不下整句，而且这里只是用来认出「是哪个窗口」。
[[nodiscard]] std::wstring Shorten(const std::wstring& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    return text.substr(0, limit) + L"…";
}

class Panel {
public:
    explicit Panel(const HomePanelContext& context) : ctx_(context) {}

    HomeAction Run(HWND owner) {
        owner_ = owner;
        if (!Register()) return HomeAction::None;
        if (!Create()) return HomeAction::None;

        const bool ownerWasEnabled = owner_ && IsWindowEnabled(owner_);
        if (ownerWasEnabled) EnableWindow(owner_, FALSE);

        MSG msg{};
        while (hwnd_) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                // 程序正在退出；把 WM_QUIT 放回去给外面那层循环。
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
        if (titleFont_) DeleteObject(titleFont_);
        return action_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Panel*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            Finish(HomeAction::None);
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    void OnCommand(int id) {
        // 四个开关：当场切、当场刷新，面板不关——它们不开窗口，关掉反而打断。
        for (int i = 0; i < static_cast<int>(kFeatures.size()); ++i) {
            if (id == kIdToggleBase + i) {
                if (ctx_.toggleFeature) ctx_.toggleFeature(kFeatures[static_cast<std::size_t>(i)].feature);
                RefreshStates();
                return;
            }
            if (id == kIdSettingsBase + i) {
                Finish(SettingsActionFor(kFeatures[static_cast<std::size_t>(i)].feature));
                return;
            }
            if (id == kIdAppsBase + i) {
                Finish(AppsActionFor(kFeatures[static_cast<std::size_t>(i)].feature));
                return;
            }
        }

        switch (id) {
        case kIdAutoStart:
            if (ctx_.toggleAutoStart) ctx_.toggleAutoStart();
            RefreshStates();
            return;
        case kIdPinLast:          Finish(HomeAction::PinLastWindow); return;
        case kIdPinGrab:          Finish(HomeAction::PinGrab); return;
        case kIdDesktopShortcut:  Finish(HomeAction::DesktopShortcut); return;
        case kIdConfigPath:       Finish(HomeAction::ConfigPath); return;
        case kIdClipKeeper:       Finish(HomeAction::ClipKeeper); return;
        case kIdDiagnose:         Finish(HomeAction::Diagnose); return;
        case kIdInstall:
            Finish(ctx_.runningFromInstallDir ? HomeAction::Uninstall : HomeAction::Install);
            return;
        case kIdAbout:            Finish(HomeAction::About); return;
        case kIdExit:             Finish(HomeAction::Exit); return;
        case kIdClose:            Finish(HomeAction::None); return;
        default:                  return;
        }
    }

    [[nodiscard]] static HomeAction SettingsActionFor(HomeFeature feature) {
        switch (feature) {
        case HomeFeature::Bookmarks: return HomeAction::BookmarkSettings;
        case HomeFeature::Borders:   return HomeAction::BorderSettings;
        case HomeFeature::Pinning:   return HomeAction::PinSettings;
        case HomeFeature::Drag:      return HomeAction::DragSettings;
        }
        return HomeAction::None;
    }

    [[nodiscard]] static HomeAction AppsActionFor(HomeFeature feature) {
        switch (feature) {
        case HomeFeature::Bookmarks: return HomeAction::BookmarkApps;
        case HomeFeature::Borders:   return HomeAction::BorderApps;
        case HomeFeature::Drag:      return HomeAction::DragApps;
        case HomeFeature::Pinning:   break;
        }
        return HomeAction::None;
    }

    void Finish(HomeAction action) {
        action_ = action;
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool Register() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kPanelClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        wc.hIcon = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    [[nodiscard]] int Scale(int value) const { return MulDiv(value, dpi_, 96); }

    HWND Add(const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w,
             int h, int id, bool titleFont = false) {
        HWND control = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style,
                                      Scale(x), Scale(y), Scale(w), Scale(h), hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      GetModuleHandleW(nullptr), nullptr);
        if (control) {
            HFONT font = titleFont && titleFont_ ? titleFont_ : font_;
            if (font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return control;
    }

    bool Create() {
        // 居中在鼠标所在的那块屏上，理由同设置窗口：owner 是 0x0 的隐藏托盘窗口，按它居中会
        // 把面板摆到屏幕左上角外面去。
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;

        UINT dpiX = 0;
        UINT dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
            dpi_ = static_cast<int>(dpiX);
        }
        font_ = CreateUiFont(dpi_, false);
        titleFont_ = CreateUiFont(dpi_, true);

        const int height = ContentHeight();
        RECT bounds{0, 0, Scale(kWidth), Scale(height)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_DLGMODALFRAME);
        const int outerW = bounds.right - bounds.left;
        const int outerH = bounds.bottom - bounds.top;
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);
        int x = workLeft + ((workRight - workLeft) - outerW) / 2;
        int y = workTop + ((workBottom - workTop) - outerH) / 2;
        x = std::clamp(x, workLeft, std::max(workLeft, workRight - outerW));
        y = std::clamp(y, workTop, std::max(workTop, workBottom - outerH));

        const std::wstring title = std::wstring(L"WindowMark ") + app::kProductVersion;
        if (!CreateWindowExW(WS_EX_DLGMODALFRAME, kPanelClass, title.c_str(),
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, outerW, outerH,
                             nullptr, nullptr, GetModuleHandleW(nullptr), this)) {
            return false;
        }

        BuildControls();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return true;
    }

    // 布局是一列往下排，高度算一次、用两次（定窗口大小、摆控件），所以两处必须同一个公式。
    [[nodiscard]] static int ContentHeight() {
        int y = kPad + kRowH + kLineH + kSectionGap;                 // 标题 + 一句话
        y += static_cast<int>(kFeatures.size()) * (kRowH + kGap + kLineH * 2 + kSectionGap);
        y += kRowH + kGap;                                          // 置顶刚才那个窗口
        y += kRowH + kSectionGap;                                   // 开机启动 + 准星
        y += (kRowH + kGap) * 3;                                    // 程序级按钮三行
        y += kRowH + kPad;                                          // 关闭
        return y;
    }

    void BuildControls() {
        const int full = kWidth - kPad * 2;
        int y = kPad;

        Add(L"STATIC", L"四个窗口小工具，合在一个托盘程序里", SS_LEFT, kPad, y, full, kRowH,
            -1, true);
        y += kRowH;
        Add(L"STATIC", L"下面每一项都可以单独开关；按钮进去是那一项的设置。", SS_LEFT, kPad, y,
            full, kLineH, -1);
        y += kLineH + kSectionGap;

        for (int i = 0; i < static_cast<int>(kFeatures.size()); ++i) {
            const FeatureText& f = kFeatures[static_cast<std::size_t>(i)];
            int buttonX = kWidth - kPad - kButtonW;
            Add(L"BUTTON", L"设置...", BS_PUSHBUTTON | WS_TABSTOP, buttonX, y, kButtonW, kRowH,
                kIdSettingsBase + i);
            if (f.hasApps) {
                buttonX -= kAppsButtonW + kGap;
                Add(L"BUTTON", L"排除应用...", BS_PUSHBUTTON | WS_TABSTOP, buttonX, y,
                    kAppsButtonW, kRowH, kIdAppsBase + i);
            }
            toggles_[static_cast<std::size_t>(i)] =
                Add(L"BUTTON", f.name, BS_AUTOCHECKBOX | WS_TABSTOP, kPad, y,
                    buttonX - kPad - kGap, kRowH, kIdToggleBase + i, true);
            y += kRowH + kGap;
            Add(L"STATIC", f.what, SS_LEFT, kPad, y, full, kLineH, -1);
            y += kLineH;
            Add(L"STATIC", f.how, SS_LEFT, kPad, y, full, kLineH, -1);
            y += kLineH + kSectionGap;
        }

        // 置顶刚才那个窗口：面板是从托盘点出来的，而托盘和面板都不在跟踪列表里，所以程序
        // 记着的「最后一个被跟踪的活动窗口」正是用户打开面板之前在用的那个。
        const std::wstring lastTitle = ctx_.lastWindowTitle ? ctx_.lastWindowTitle() : L"";
        pinLast_ = Add(L"BUTTON", PinButtonText(lastTitle),
                       BS_PUSHBUTTON | WS_TABSTOP, kPad, y, full - kAppsButtonW - kGap, kRowH,
                       kIdPinLast);
        Add(L"BUTTON", L"⊕ 抓取窗口...", BS_PUSHBUTTON | WS_TABSTOP,
            kWidth - kPad - kAppsButtonW, y, kAppsButtonW, kRowH, kIdPinGrab);
        y += kRowH + kGap;

        autoStart_ = Add(L"BUTTON", L"开机时自动启动 WindowMark", BS_AUTOCHECKBOX | WS_TABSTOP,
                         kPad, y, full, kRowH, kIdAutoStart);
        y += kRowH + kSectionGap;

        // 程序级入口。两列，顺序按「多久用一次」排。
        const int halfW = (full - kGap) / 2;
        Add(L"BUTTON", L"配置文件...", BS_PUSHBUTTON | WS_TABSTOP, kPad, y, halfW, kRowH,
            kIdConfigPath);
        Add(L"BUTTON", L"创建桌面快捷方式", BS_PUSHBUTTON | WS_TABSTOP, kPad + halfW + kGap, y,
            halfW, kRowH, kIdDesktopShortcut);
        y += kRowH + kGap;
        Add(L"BUTTON", L"剪贴板守护...", BS_PUSHBUTTON | WS_TABSTOP, kPad, y, halfW, kRowH,
            kIdClipKeeper);
        Add(L"BUTTON", L"诊断报告", BS_PUSHBUTTON | WS_TABSTOP, kPad + halfW + kGap, y, halfW,
            kRowH, kIdDiagnose);
        y += kRowH + kGap;
        Add(L"BUTTON", ctx_.runningFromInstallDir ? L"卸载 WindowMark..." : L"安装到系统...",
            BS_PUSHBUTTON | WS_TABSTOP, kPad, y, halfW, kRowH, kIdInstall);
        Add(L"BUTTON", L"关于", BS_PUSHBUTTON | WS_TABSTOP, kPad + halfW + kGap, y, halfW,
            kRowH, kIdAbout);
        y += kRowH + kGap;

        // 退出摆在最左、关闭在最右：两个都是「结束这次操作」，但后果差一个数量级，
        // 挨着放迟早点错。
        Add(L"BUTTON", L"退出 WindowMark", BS_PUSHBUTTON | WS_TABSTOP, kPad, y, halfW, kRowH,
            kIdExit);
        Add(L"BUTTON", L"关闭", BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - kPad - kButtonW, y,
            kButtonW, kRowH, kIdClose);

        RefreshStates();
    }

    [[nodiscard]] std::wstring PinButtonText(const std::wstring& title) const {
        if (title.empty()) return L"（当前没有可置顶的窗口）";
        const bool pinned = ctx_.lastWindowPinned && ctx_.lastWindowPinned();
        return (pinned ? L"取消置顶「" : L"置顶刚才那个窗口「") + Shorten(title, 28) + L"」";
    }

    void RefreshStates() {
        const Settings settings = ctx_.settings ? ctx_.settings() : Settings{};
        for (std::size_t i = 0; i < kFeatures.size(); ++i) {
            if (!toggles_[i]) continue;
            const bool on = FeatureEnabled(settings, kFeatures[i].feature);
            SendMessageW(toggles_[i], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (autoStart_) {
            const bool on = ctx_.autoStartEnabled && ctx_.autoStartEnabled();
            SendMessageW(autoStart_, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (pinLast_) {
            const std::wstring title = ctx_.lastWindowTitle ? ctx_.lastWindowTitle() : L"";
            SetWindowTextW(pinLast_, PinButtonText(title).c_str());
            EnableWindow(pinLast_, !title.empty() && settings.pin.enabled);
        }
    }

    const HomePanelContext& ctx_;
    HWND owner_{};
    HWND hwnd_{};
    HFONT font_{};
    HFONT titleFont_{};
    int dpi_{96};
    HomeAction action_{HomeAction::None};
    std::array<HWND, 4> toggles_{};
    HWND autoStart_{};
    HWND pinLast_{};
};

} // namespace

HomeAction WinHomePanel::ShowModal(HWND owner, const HomePanelContext& context) {
    Panel panel(context);
    return panel.Run(owner);
}

} // namespace windowmark::win
