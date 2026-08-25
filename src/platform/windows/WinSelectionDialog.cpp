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
    DialogState(HWND owner, std::vector<AppSelectionModel>& selection,
                const SelectionDialogOptions& options)
        : owner_(owner), selection_(selection), options_(options) {}

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

        // Same reason as the settings window: a pinned window sits in the topmost band,
        // and a normal window cannot be raised above it.
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            kSelectionClass,
            options_.title,
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
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header && header->idFrom == kTreeId && header->code == TVN_SELCHANGEDW) {
                auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
                Highlight(reinterpret_cast<const NodeRef*>(change->itemNew.lParam));
            }
            break;
        }
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
            // Clear the highlight before the panel goes: the outline lives in another
            // process's z-order and nothing else would take it down.
            if (options_.onHighlight) options_.onHighlight(0);
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
            // TVS_CHECKBOXES is deliberately absent here - see just below.
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 100, 100,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)),
            GetModuleHandleW(nullptr),
            nullptr);

        note_ = CreateWindowExW(
            0, L"STATIC",
            options_.note,
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

        // TVS_CHECKBOXES has to be applied *after* creation and *before* any item is
        // inserted. Documented, and not cosmetic: set at creation time the control can end
        // up with every box reading back unchecked no matter what TVM_SETITEM was told, so
        // pressing 应用 without touching anything wiped the whole list. Caught by a
        // round-trip test - open the panel, change nothing, press 应用, compare the file.
        SetWindowLongPtrW(tree_, GWL_STYLE,
                          GetWindowLongPtrW(tree_, GWL_STYLE) | TVS_CHECKBOXES);

        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND control : {tree_, note_, apply_, cancel_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return true;
    }

    // Height the note needs at this width, so the layout follows the wording instead of
    // the wording having to fit the layout.
    [[nodiscard]] int MeasureNoteHeight(int textWidth) const {
        if (!note_ || !options_.note || textWidth <= 0) return 42;
        HDC dc = GetDC(note_);
        if (!dc) return 42;
        auto font = reinterpret_cast<HFONT>(SendMessageW(note_, WM_GETFONT, 0, 0));
        HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
        RECT box{0, 0, textWidth, 0};
        DrawTextW(dc, options_.note, -1, &box, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        if (previous) SelectObject(dc, previous);
        ReleaseDC(note_, dc);
        const int measured = static_cast<int>(box.bottom - box.top) + 4;
        // Floor keeps a one-line note from crowding the buttons; ceiling keeps a runaway
        // string from squeezing the tree out of existence.
        return std::min(180, std::max(36, measured));
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
        // Measured rather than fixed. It used to be a hardcoded 42, which fits two lines;
        // a three-line note then painted over the button row and 取消 disappeared. Any
        // future wording change would have hit the same wall.
        const int noteH = MeasureNoteHeight(width - margin * 2);

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

    // One place each way round, so the two conversions cannot drift apart.
    [[nodiscard]] bool ShownAsChecked(bool enabled) const {
        return options_.checkedMeansExcluded ? !enabled : enabled;
    }
    [[nodiscard]] bool EnabledFromCheck(bool checked) const {
        return options_.checkedMeansExcluded ? !checked : checked;
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
            SetTreeCheck(tree_, appNode->item, ShownAsChecked(app.enabled));

            for (std::size_t windowIndex = 0; windowIndex < app.windows.size(); ++windowIndex) {
                const auto& window = app.windows[windowIndex];
                nodes_.push_back(NodeRef{NodeRef::Kind::Window, appIndex, windowIndex, nullptr});
                NodeRef* windowNode = &nodes_.back();
                std::wstring title = L"↳ " + Utf8ToWide(window.title);
                windowNode->item = InsertItem(title, appNode->item, windowNode);
                SetTreeCheck(tree_, windowNode->item, ShownAsChecked(window.enabled));
            }
            TreeView_Expand(tree_, appNode->item, TVE_EXPAND);
        }
    }

    // An app row highlights nothing rather than guessing one of its windows - the row
    // stands for all of them, and lighting up an arbitrary member would be a lie.
    void Highlight(const NodeRef* node) {
        if (!options_.onHighlight) return;
        WindowId id = 0;
        if (node && node->kind == NodeRef::Kind::Window &&
            node->appIndex < selection_.size() &&
            node->windowIndex < selection_[node->appIndex].windows.size()) {
            id = selection_[node->appIndex].windows[node->windowIndex].windowId;
        }
        if (id == highlighted_) return;
        highlighted_ = id;
        options_.onHighlight(id);
    }

    void ReadStates() {
        for (const auto& node : nodes_) {
            if (!node.item) continue;
            const bool checked = EnabledFromCheck(GetTreeCheck(tree_, node.item));
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
    SelectionDialogOptions options_;
    std::vector<NodeRef> nodes_;
    WindowId highlighted_{};
    bool applied_{false};
};

} // namespace

bool WinSelectionDialog::ShowModal(HWND owner, std::vector<AppSelectionModel>& selection,
                                   const SelectionDialogOptions& options) {
    DialogState state(owner, selection, options);
    return state.Run();
}

} // namespace windowmark::win
