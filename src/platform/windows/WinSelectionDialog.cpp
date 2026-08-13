#include "WinSelectionDialog.h"

#include "WinUtil.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace windowmark::win {
namespace {

constexpr wchar_t kSelectionClass[] = L"WindowMark.SelectionDialog";
constexpr int kTreeId = 2001;
constexpr int kApplyId = 2002;
constexpr int kCancelId = 2003;
constexpr int kNoteId = 2004;

struct NodeRef {
    enum class Kind { App, Window };
    Kind kind{Kind::App};
    std::size_t appIndex{};
    std::size_t windowIndex{};
    HTREEITEM item{};
};

void SetTreeCheck(HWND tree, HTREEITEM item, bool checked) {
    TVITEMW tvi{};
    tvi.mask = TVIF_HANDLE | TVIF_STATE;
    tvi.hItem = item;
    tvi.stateMask = TVIS_STATEIMAGEMASK;
    tvi.state = INDEXTOSTATEIMAGEMASK(checked ? 2 : 1);
    TreeView_SetItem(tree, &tvi);
}

bool GetTreeCheck(HWND tree, HTREEITEM item) {
    TVITEMW tvi{};
    tvi.mask = TVIF_HANDLE | TVIF_STATE;
    tvi.hItem = item;
    tvi.stateMask = TVIS_STATEIMAGEMASK;
    if (!TreeView_GetItem(tree, &tvi)) return false;
    return ((tvi.state & TVIS_STATEIMAGEMASK) >> 12U) == 2U;
}

class DialogState {
public:
    DialogState(HWND owner, std::vector<AppSelectionModel>& selection)
        : owner_(owner), selection_(selection) {}

    bool Run() {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kSelectionClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        RECT bounds{0, 0, 720, 540};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, WS_EX_DLGMODALFRAME);
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        if (owner_ && IsWindow(owner_) && IsWindowVisible(owner_)) {
            RECT ownerRect{};
            GetWindowRect(owner_, &ownerRect);
            x = ownerRect.left + ((ownerRect.right - ownerRect.left) - (bounds.right - bounds.left)) / 2;
            y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - (bounds.bottom - bounds.top)) / 2;
        } else {
            const int sw = GetSystemMetrics(SM_CXSCREEN);
            const int sh = GetSystemMetrics(SM_CYSCREEN);
            x = std::max(0, (sw - static_cast<int>(bounds.right - bounds.left)) / 2);
            y = std::max(0, (sh - static_cast<int>(bounds.bottom - bounds.top)) / 2);
        }

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kSelectionClass,
            L"WindowMark - 选择需要书签的应用/窗口",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
            x,
            y,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            owner_,
            nullptr,
            wc.hInstance,
            this);
        if (!hwnd_) return false;

        if (owner_ && IsWindow(owner_)) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        SetForegroundWindow(hwnd_);

        MSG msg{};
        int getMessageResult = 1;
        while (hwnd_ && (getMessageResult = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0))) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (getMessageResult == 0) {
            // Do not swallow an application-wide quit requested while the modal
            // selector was open. Re-post it for the outer message loop.
            PostQuitMessage(static_cast<int>(msg.wParam));
        }

        if (owner_ && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        return applied_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<DialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->HandleMessage(msg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE:
            if (!CreateChildren()) return -1;
            Populate();
            LayoutChildren();
            return 0;
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kApplyId) {
                ReadStates();
                applied_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == kCancelId) {
                DestroyWindow(hwnd_);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    bool CreateChildren() {
        tree_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_TREEVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_CHECKBOXES,
            0, 0, 100, 100,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)),
            GetModuleHandleW(nullptr),
            nullptr);

        note_ = CreateWindowExW(
            0, L"STATIC",
            L"说明：应用勾选状态会保存；单个窗口勾选状态只对本次运行有效。应用未勾选时，其下面窗口即使勾选也不会显示书签。",
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 40,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNoteId)),
            GetModuleHandleW(nullptr),
            nullptr);

        apply_ = CreateWindowExW(
            0, L"BUTTON", L"应用",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, 0, 90, 30,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyId)),
            GetModuleHandleW(nullptr),
            nullptr);

        cancel_ = CreateWindowExW(
            0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 90, 30,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)),
            GetModuleHandleW(nullptr),
            nullptr);

        if (!tree_ || !note_ || !apply_ || !cancel_) return false;

        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND control : {tree_, note_, apply_, cancel_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return true;
    }

    void LayoutChildren() {
        if (!hwnd_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int width = std::max(200, static_cast<int>(rc.right - rc.left));
        const int height = std::max(200, static_cast<int>(rc.bottom - rc.top));
        constexpr int margin = 14;
        constexpr int buttonW = 92;
        constexpr int buttonH = 32;
        constexpr int gap = 10;
        constexpr int noteH = 42;

        const int bottomY = height - margin - buttonH;
        MoveWindow(tree_, margin, margin, width - margin * 2,
                   std::max(80, bottomY - margin - gap - noteH - margin), TRUE);
        MoveWindow(note_, margin, bottomY - gap - noteH,
                   width - margin * 2, noteH, TRUE);
        MoveWindow(cancel_, width - margin - buttonW, bottomY, buttonW, buttonH, TRUE);
        MoveWindow(apply_, width - margin - buttonW * 2 - gap, bottomY, buttonW, buttonH, TRUE);
    }

    HTREEITEM InsertItem(const std::wstring& text, HTREEITEM parent, NodeRef* node) {
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(text.c_str());
        insert.item.lParam = reinterpret_cast<LPARAM>(node);
        return TreeView_InsertItem(tree_, &insert);
    }

    void Populate() {
        std::size_t totalNodes = selection_.size();
        for (const auto& app : selection_) totalNodes += app.windows.size();
        nodes_.reserve(totalNodes);

        if (selection_.empty()) {
            NodeRef dummy{};
            nodes_.push_back(dummy);
            const auto item = InsertItem(L"当前没有可配置的普通顶层窗口。", TVI_ROOT, &nodes_.back());
            TreeView_SelectItem(tree_, item);
            EnableWindow(apply_, FALSE);
            return;
        }

        for (std::size_t appIndex = 0; appIndex < selection_.size(); ++appIndex) {
            const auto& app = selection_[appIndex];
            nodes_.push_back(NodeRef{NodeRef::Kind::App, appIndex, 0, nullptr});
            NodeRef* appNode = &nodes_.back();

            std::wstring appText = Utf8ToWide(app.appName.empty() ? app.groupKey : app.appName);
            appText += L"  (" + std::to_wstring(app.windows.size()) + L" 个窗口)";
            appNode->item = InsertItem(appText, TVI_ROOT, appNode);
            SetTreeCheck(tree_, appNode->item, app.enabled);

            for (std::size_t windowIndex = 0; windowIndex < app.windows.size(); ++windowIndex) {
                const auto& window = app.windows[windowIndex];
                nodes_.push_back(NodeRef{NodeRef::Kind::Window, appIndex, windowIndex, nullptr});
                NodeRef* windowNode = &nodes_.back();
                std::wstring title = L"↳ " + Utf8ToWide(window.title);
                windowNode->item = InsertItem(title, appNode->item, windowNode);
                SetTreeCheck(tree_, windowNode->item, window.enabled);
            }
            TreeView_Expand(tree_, appNode->item, TVE_EXPAND);
        }
    }

    void ReadStates() {
        for (const auto& node : nodes_) {
            if (!node.item) continue;
            const bool checked = GetTreeCheck(tree_, node.item);
            if (node.kind == NodeRef::Kind::App) {
                if (node.appIndex < selection_.size()) selection_[node.appIndex].enabled = checked;
            } else if (node.appIndex < selection_.size() &&
                       node.windowIndex < selection_[node.appIndex].windows.size()) {
                selection_[node.appIndex].windows[node.windowIndex].enabled = checked;
            }
        }
    }

    HWND owner_{};
    HWND hwnd_{};
    HWND tree_{};
    HWND note_{};
    HWND apply_{};
    HWND cancel_{};
    std::vector<AppSelectionModel>& selection_;
    std::vector<NodeRef> nodes_;
    bool applied_{false};
};

} // namespace

bool WinSelectionDialog::ShowModal(HWND owner, std::vector<AppSelectionModel>& selection) {
    DialogState state(owner, selection);
    return state.Run();
}

} // namespace windowmark::win
