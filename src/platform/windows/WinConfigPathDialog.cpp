#include "WinConfigPathDialog.h"

#include "WinUtil.h"

#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <string>

namespace windowmark::win {
namespace {

constexpr wchar_t kConfigPathClass[] = L"WindowMark.ConfigPathDialog";

// 三个单选的 id 必须连续，CheckRadioButton 是按区间清其余项的。
constexpr int kDefaultId = 4200;
constexpr int kPortableId = 4201;
constexpr int kCustomId = 4202;
constexpr int kCurrentId = 4203;
constexpr int kDefPathId = 4204;
constexpr int kPortPathId = 4205;
constexpr int kPathEditId = 4206;
constexpr int kBrowseId = 4207;
constexpr int kOkId = 4208;
constexpr int kCancelId = 4209;

HFONT CreateUiFont(int dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(dpi))) {
        return nullptr;
    }
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

[[nodiscard]] const wchar_t* SourceLabel(ConfigSource source) {
    switch (source) {
    case ConfigSource::Portable:
        return L"程序目录";
    case ConfigSource::Configured:
        return L"自定义";
    case ConfigSource::Fallback:
        break;
    }
    return L"默认位置";
}

[[nodiscard]] std::filesystem::path BrowseForFolder(HWND owner) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

    std::filesystem::path chosen;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR text = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) {
                chosen = text;
                CoTaskMemFree(text);
            }
            item->Release();
        }
    }
    dialog->Release();
    return chosen;
}

class Prompt {
public:
    Prompt(const std::filesystem::path& current, ConfigSource currentSource,
           std::filesystem::path& chosen)
        : current_(current), currentSource_(currentSource), chosen_(chosen) {
        portable_ = PortableConfigPath();
        const auto root = LocalDataRoot();
        fallback_ = root.empty() ? std::filesystem::path{} : root / L"settings.conf";
        if (currentSource_ == ConfigSource::Configured) {
            customDir_ = current_.parent_path().wstring();
        }
    }

    bool Run(HWND owner) {
        owner_ = owner;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConfigPathClass;
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

        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kConfigPathClass,
                                L"WindowMark - 配置文件位置",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, outerW, outerH,
                                owner_, nullptr, GetModuleHandleW(nullptr), this);
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

    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
             int id) {
        HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y),
                                       S(w), S(h), hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       GetModuleHandleW(nullptr), nullptr);
        if (control && font_) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        return control;
    }

    void Build() {
        const std::wstring header = std::wstring(L"当前生效：") + current_.wstring() + L"\n（" +
                                    SourceLabel(currentSource_) + L"）";
        Add(L"STATIC", header.c_str(), 0, 14, 12, 492, 36, kCurrentId);

        Add(L"BUTTON", L"默认位置", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 14, 58, 492, 22,
            kDefaultId);
        Add(L"STATIC", fallback_.wstring().c_str(), SS_PATHELLIPSIS, 34, 80, 472, 18,
            kDefPathId);

        Add(L"BUTTON", L"程序目录（便携，跟着程序走）", WS_TABSTOP | BS_AUTORADIOBUTTON, 14,
            104, 492, 22, kPortableId);
        Add(L"STATIC", portable_.wstring().c_str(), SS_PATHELLIPSIS, 34, 126, 472, 18,
            kPortPathId);

        Add(L"BUTTON", L"自定义", WS_TABSTOP | BS_AUTORADIOBUTTON, 14, 150, 492, 22, kCustomId);
        Add(L"EDIT", customDir_.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 34, 174, 366,
            24, kPathEditId);
        Add(L"BUTTON", L"浏览...", WS_TABSTOP, 408, 174, 98, 24, kBrowseId);

        Add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 334, 214, 84, 28, kOkId);
        Add(L"BUTTON", L"取消", WS_TABSTOP, 422, 214, 84, 28, kCancelId);

        CheckRadioButton(hwnd_, kDefaultId, kCustomId, InitialRadioId());
        SyncEnabled();
    }

    [[nodiscard]] int InitialRadioId() const {
        switch (currentSource_) {
        case ConfigSource::Portable:
            return kPortableId;
        case ConfigSource::Configured:
            return kCustomId;
        case ConfigSource::Fallback:
            break;
        }
        return kDefaultId;
    }

    // 用户在「自定义」里选的是目录，文件名由我们补，省得他自己拼。
    [[nodiscard]] std::filesystem::path SelectedPath() const {
        if (IsDlgButtonChecked(hwnd_, kPortableId) == BST_CHECKED) return portable_;
        if (IsDlgButtonChecked(hwnd_, kCustomId) == BST_CHECKED) {
            wchar_t buffer[MAX_PATH * 2]{};
            GetDlgItemTextW(hwnd_, kPathEditId, buffer, static_cast<int>(std::size(buffer)));
            std::wstring text(buffer);
            Trim(text);
            if (text.empty()) return {};
            return std::filesystem::path(text) / L"settings.conf";
        }
        return fallback_;
    }

    void SyncEnabled() {
        const BOOL custom = IsDlgButtonChecked(hwnd_, kCustomId) == BST_CHECKED;
        EnableWindow(GetDlgItem(hwnd_, kPathEditId), custom);
        EnableWindow(GetDlgItem(hwnd_, kBrowseId), custom);
    }

    // 切换单选时只提示不拦，否则用户没法把选择挪走；真正拦住是在确定那一步。
    void WarnIfUnwritable(const std::filesystem::path& candidate) const {
        if (candidate.empty()) return;
        if (IsDirectoryWritable(candidate.parent_path())) return;
        MessageBoxW(hwnd_,
                    L"这个位置写不进去，换一个吧。\n\n"
                    L"装在 C:\\Program Files 这类目录时会遇到这种情况。",
                    L"WindowMark", MB_OK | MB_ICONWARNING);
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
            case kDefaultId:
            case kPortableId:
            case kCustomId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    SyncEnabled();
                    WarnIfUnwritable(SelectedPath());
                }
                return 0;

            case kBrowseId: {
                const auto dir = BrowseForFolder(hwnd_);
                if (!dir.empty()) {
                    SetDlgItemTextW(hwnd_, kPathEditId, dir.c_str());
                    WarnIfUnwritable(dir / L"settings.conf");
                }
                return 0;
            }

            case kOkId: {
                const auto candidate = SelectedPath();
                if (candidate.empty()) {
                    MessageBoxW(hwnd_, L"请先填一个目录。", L"WindowMark",
                                MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                // 这里不能只提示——真让它过去，配置就搬进一个存不住的地方了。
                if (!IsDirectoryWritable(candidate.parent_path())) {
                    WarnIfUnwritable(candidate);
                    return 0;
                }
                chosen_ = candidate;
                accepted_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }

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
            // 这个模态循环嵌在托盘应用自己的消息循环里。清掉 hwnd_ 就能结束它，
            // 而 PostQuitMessage 会把整个托盘应用一起带走。
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

    static constexpr int kWidth = 520;
    static constexpr int kHeight = 256;

    std::filesystem::path current_;
    ConfigSource currentSource_{ConfigSource::Fallback};
    std::filesystem::path& chosen_;
    std::filesystem::path portable_;
    std::filesystem::path fallback_;
    std::wstring customDir_;
    HWND owner_{};
    HWND hwnd_{};
    HFONT font_{};
    int dpi_{96};
    bool accepted_{false};
};

} // namespace

bool WinConfigPathDialog::ShowModal(HWND owner, const std::filesystem::path& current,
                                    ConfigSource currentSource,
                                    std::filesystem::path& chosen) {
    Prompt prompt(current, currentSource, chosen);
    return prompt.Run(owner);
}

} // namespace windowmark::win
