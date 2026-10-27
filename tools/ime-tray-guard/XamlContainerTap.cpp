// XAML 局部布局组件：通过诊断 API 进入所属线程，但不订阅全局视觉树。
#include <windows.h>
#include <ole2.h>
#include <ocidl.h>
#include <xamlom.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/base.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include "LocalIslandRoots.h"
#include "XamlContainerProtocol.h"
#include "XamlWidthOverride.h"
#include "XamlHintPresenter.h"
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <atomic>

using namespace container_trial;
using namespace winrt::Windows::UI::Xaml;
namespace {
HMODULE gModule{};
HANDLE gNativeMarker{}; // 跟随目标进程寿命，不因控制程序退出丢失“已附加”记录。
constexpr UINT kReady=WM_APP+31, kFinish=WM_APP+32;
constexpr auto kWindowClass=kDispatchClass;
struct Entry { InstanceHandle id{}, parent{}; std::wstring type; };
class Tap : public winrt::implements<Tap,IObjectWithSite> {
    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
    std::map<InstanceHandle,Entry> entries_;
    std::map<InstanceHandle,FrameworkElement> elements_;
    std::mutex mutex_;
    HWND window_{};
    HANDLE mapping_{}, owner_{};
    Block* shared_{};
    ULONGLONG begin_{};
    WidthOverride width_;
    HintPresenter hint_;
    bool stopping_{};
    bool persistent_{};
    std::atomic<HRESULT> subscribeResult_{E_PENDING},unsubscribeResult_{E_PENDING};
    unsigned stableCount_{};
    double lastWidth_{};
    double learnedWidth_{};
    InstanceHandle learnedCandidate_{};
    FrameworkElement learnedElement_{nullptr}; // 保持对象身份，防止空闲期间地址复用命中旧宽度。
    InstanceHandle lastCandidate_{};

    static LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) noexcept {
        auto self=reinterpret_cast<Tap*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            self=static_cast<Tap*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd,message,w,l);
        try {
            if(message==WM_COPYDATA) {
                const auto data=reinterpret_cast<COPYDATASTRUCT*>(l);
                if(!data || data->dwData!=kMagic || data->cbData<sizeof(wchar_t) || data->cbData>240 || data->cbData%sizeof(wchar_t))return FALSE;
                const auto name=static_cast<const wchar_t*>(data->lpData);
                if(!name || name[data->cbData/sizeof(wchar_t)-1]!=0 || !self->persistent_ || self->shared_ || self->subscribeResult_.load()!=S_OK)return FALSE;
                if(FAILED(self->OpenLease(name)))return FALSE;
                self->stopping_=false;self->begin_=GetTickCount64();self->stableCount_=0;
                // 重连发生于布局/DPI变化或重新启用，不能复用上次环境的宽度。
                self->learnedWidth_=0;self->learnedCandidate_=0;self->learnedElement_=nullptr;
                self->lastCandidate_=0;self->lastWidth_=0;
                self->shared_->uiThread=GetCurrentThreadId();self->shared_->subscribeHr=S_OK;self->shared_->subscriptionActive=0;
                self->shared_->state=Observing;
                if(!SetTimer(hwnd,1,250,nullptr)){self->Stop();return FALSE;}
                return TRUE;
            }
            if (message==kReady) {
                if(self->stopping_)return 0;
                self->shared_->subscribeHr=self->subscribeResult_.load();
                if (FAILED(self->shared_->subscribeHr)) self->Stop();
                else {
                    self->shared_->subscriptionActive=0;
                    self->shared_->state=Observing;
                }
                return 0;
            }
            if (message==WM_TIMER) {
                if(!self->shared_)return 0;
                if(self->shared_->state==Attached) {
                    if(self->shared_->stop || WaitForSingleObject(self->owner_,0)!=WAIT_TIMEOUT ||
                       GetTickCount64()-self->begin_>self->shared_->durationSeconds*1000ULL) self->Stop();
                } else self->Tick();
                return 0;
            }
            if (message==kFinish) {
                self->Finish(); DestroyWindow(hwnd); self->window_=nullptr; self->Release(); return 0;
            }
        } catch (...) { if(self->shared_) InterlockedIncrement(&self->shared_->errors); self->Stop(); }
        return DefWindowProcW(hwnd,message,w,l);
    }
    void Stop() noexcept {
        if (stopping_) return;
        stopping_=true; KillTimer(window_,1);
        hint_.Clear();
        if(shared_)shared_->hintVisible=0;
        if (width_.Active()) {
            if (width_.Restore()) { InterlockedIncrement(&shared_->restores); shared_->state=Restored; }
            else { InterlockedIncrement(&shared_->restoreFailures); shared_->state=Failed; }
        }
        if(persistent_ && subscribeResult_.load()==S_OK && shared_ && !shared_->restoreFailures) {
            shared_->state=Closed; // 只结束本次控制租约，连接仍保留；日志明确标记subscriptionActive。
            DropLease();return;
        }
        unsubscribeResult_=S_OK;
        PostMessageW(window_,kFinish,0,0);
    }
    void DropLease() noexcept {
        std::lock_guard lock(mutex_);
        entries_.clear();elements_.clear();
        if(shared_){UnmapViewOfFile(shared_);shared_=nullptr;}
        if(mapping_){CloseHandle(mapping_);mapping_=nullptr;}
        if(owner_){CloseHandle(owner_);owner_=nullptr;}
    }
    HRESULT OpenLease(const std::wstring& name) noexcept {
        if(shared_ || !name.starts_with(L"Local\\WindowMark.XamlContainerTrial.") || name.size()>120)return E_INVALIDARG;
        HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
        if(!mapping)return HRESULT_FROM_WIN32(GetLastError());
        auto block=static_cast<Block*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Block)));
        if(!block){CloseHandle(mapping);return E_FAIL;}
        wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
        const wchar_t* file=wcsrchr(path,L'\\');file=file?file+1:path;
        bool identity=block->fixture?_wcsicmp(file,L"WindowMarkXamlContainerFixture.exe")==0:
            (block->nativeConsent && block->persistent && _wcsicmp(file,L"explorer.exe")==0);
        if(block->magic!=kMagic || block->version!=kVersion || block->targetPid!=GetCurrentProcessId() ||
            block->durationSeconds<5 || block->durationSeconds>90 || !identity || !block->persistent) {
            UnmapViewOfFile(block);CloseHandle(mapping);return E_ACCESSDENIED;
        }
        HANDLE owner=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,block->ownerPid);
        if(!owner){UnmapViewOfFile(block);CloseHandle(mapping);return E_ACCESSDENIED;}
        std::lock_guard lock(mutex_);mapping_=mapping;shared_=block;owner_=owner;return S_OK;
    }
    void Finish() noexcept {
        if (shared_) {
            shared_->unsubscribeHr=unsubscribeResult_.load();shared_->subscriptionActive=0;
            if (FAILED(shared_->unsubscribeHr) || shared_->restoreFailures) shared_->state=Failed;
            else shared_->state=Closed;
        }
        diagnostics_=nullptr;
        { std::lock_guard lock(mutex_); entries_.clear();elements_.clear(); }
        DropLease();
    }
    FrameworkElement Element(InstanceHandle handle) {
        auto it=elements_.find(handle);
        return it==elements_.end()?nullptr:it->second;
    }
    void RefreshLocalTree() {
        local_roots::Roots roots;
        winrt::check_hresult(local_roots::Read(roots));
        entries_.clear();elements_.clear();
        struct Pending { DependencyObject object;InstanceHandle parent; };
        std::vector<Pending> pending;
        for(auto const& raw:roots) {
            winrt::Windows::Foundation::IInspectable object{nullptr};
            winrt::copy_from_abi(object,raw.Get());
            if(auto source=object.try_as<Hosting::DesktopWindowXamlSource>()) {
                if(!shared_->fixture) {
                    HWND child{};
                    auto native=source.try_as<IDesktopWindowXamlSourceNative>();
                    if(!native || FAILED(native->get_WindowHandle(&child)) ||
                       GetAncestor(child,GA_ROOT)!=FindWindowW(L"Shell_TrayWnd",nullptr))continue;
                }
                if(auto content=source.Content())pending.push_back({content,0});
            }
        }
        size_t visited{};
        while(!pending.empty()) {
            if(++visited>6000){shared_->overflow=1;break;}
            auto current=std::move(pending.back());pending.pop_back();
            const auto id=reinterpret_cast<InstanceHandle>(winrt::get_abi(current.object));
            const std::wstring type=winrt::get_class_name(current.object).c_str();
            // 应用按钮内容与 IME 布局无关，不进入其子树。
            if(!shared_->fixture && type==L"Taskbar.TaskListButton")continue;
            entries_[id]={id,current.parent,type};
            if(auto fe=current.object.try_as<FrameworkElement>())elements_.emplace(id,fe);
            const int count=Media::VisualTreeHelper::GetChildrenCount(current.object);
            for(int i=0;i<count;++i)pending.push_back({Media::VisualTreeHelper::GetChild(current.object,i),id});
        }
    }
    void Tick() {
        if (!shared_ || stopping_) return;
        if (shared_->stop || WaitForSingleObject(owner_,0)!=WAIT_TIMEOUT ||
            GetTickCount64()-begin_>shared_->durationSeconds*1000ULL) { Stop(); return; }
        RefreshLocalTree();
        std::vector<Entry> nodes;
        std::set<InstanceHandle> languageAncestors;
        {
            std::lock_guard lock(mutex_);
            std::set<InstanceHandle> wanted;
            for (auto const& [id,e]:entries_) {
                if(e.type==L"SystemTray.LanguageImageIconContent" || e.type==L"SystemTray.LanguageTextIconContent") {
                    bool visible=true;
                    for(auto p=id;p;) {
                        auto fe=Element(p);
                        if(fe && (fe.Visibility()!=Visibility::Visible || fe.ActualWidth()<=0))visible=false;
                        auto it=entries_.find(p);if(it==entries_.end())break;p=it->second.parent;
                    }
                    if(!visible)continue; // 首次隐藏时不能把收缩后的宽度学成正常宽度。
                    auto p=id;
                    for(int depth=0;p&&depth<16;++depth) {
                        auto it=entries_.find(p);if(it==entries_.end())break;
                        languageAncestors.insert(p);p=it->second.parent;
                    }
                }
                if (e.type.starts_with(L"SystemTray.") || shared_->fixture) {
                    auto parent=id;
                    for (int depth=0;parent && depth<8;++depth) {
                        auto it=entries_.find(parent); if(it==entries_.end()) break;
                        wanted.insert(parent); parent=it->second.parent;
                    }
                }
            }
            for (auto id:wanted) {
                if (nodes.size()>=kCapacity) { shared_->overflow=1; break; }
                nodes.push_back(entries_.at(id));
            }
        }
        std::vector<Node> snapshot;
        FrameworkElement candidate{nullptr}; InstanceHandle candidateId{};
        unsigned candidateCount{};
        for (auto const& node:nodes) {
            Node out{}; out.handle=node.id; out.parent=node.parent;
            wcsncpy_s(out.type,node.type.c_str(),_TRUNCATE);
            try {
                auto element=Element(node.id);
                if (!element) { out.hr=E_NOINTERFACE; snapshot.push_back(out); continue; }
                if (!element.Dispatcher().HasThreadAccess()) { out.hr=RPC_E_WRONG_THREAD; snapshot.push_back(out); continue; }
                // x:Name是结构标识；只记录ASCII标识符，拒绝路径/任意Unicode内容。
                auto name=element.Name(); bool safe=name.size()<64;
                for (auto c:name) if (!(c==L'_' || (c>=L'A'&&c<=L'Z') || (c>=L'a'&&c<=L'z') || (c>=L'0'&&c<=L'9'))) safe=false;
                if (safe) wcsncpy_s(out.name,name.c_str(),_TRUNCATE);
                out.width=element.ActualWidth(); out.height=element.ActualHeight();
                out.minWidth=element.MinWidth(); out.visible=element.Visibility()==Visibility::Visible;
                out.hitTest=element.IsHitTestVisible();
                out.hasBinding=element.GetBindingExpression(FrameworkElement::MinWidthProperty())?1:0;
                out.localKind=out.hasBinding?2:(element.ReadLocalValue(FrameworkElement::MinWidthProperty())==DependencyProperty::UnsetValue()?0:1);
                auto position=element.TransformToVisual(nullptr).TransformPoint({0,0}); out.x=position.X; out.y=position.Y;
                if (shared_->fixture && name==L"WindowMarkImeSlots") { candidate=element; candidateId=node.id; ++candidateCount; }
                // 仅针对本轮实测容器：原生语言内容的稳定祖先，不使用序号或图标字形。
                if (!shared_->fixture && node.type==L"SystemTray.Stack" && name==L"NonActivatableStack") {
                    candidate=element;candidateId=node.id;++candidateCount;
                }
            } catch (winrt::hresult_error const& e) { out.hr=e.code(); }
              catch (...) { out.hr=E_FAIL; }
            snapshot.push_back(out);
        }
        if (width_.Active() && (candidateCount!=1 || candidateId!=shared_->targetHandle ||
            candidate.Visibility()!=Visibility::Visible || shared_->overflow)) { Stop();return; }
        if(!shared_->reserve && width_.Active()) {
            hint_.Clear();
            if(!width_.Restore()){InterlockedIncrement(&shared_->restoreFailures);shared_->state=Failed;Stop();return;}
            InterlockedIncrement(&shared_->restores);shared_->state=Observing;stableCount_=0;
        }
        if (shared_->reserve && !width_.Active() && candidateCount==1 && !shared_->overflow &&
            (shared_->fixture || learnedCandidate_==candidateId || languageAncestors.contains(candidateId))) {
            const double size=learnedCandidate_==candidateId?learnedWidth_:candidate.ActualWidth();
            if (candidateId==lastCandidate_ && std::abs(size-lastWidth_)<0.01 && size>0) ++stableCount_;
            else stableCount_=0;
            lastCandidate_=candidateId; lastWidth_=size;
            if (stableCount_>=8) {
                width_.Apply(candidate,size);
                learnedWidth_=size;learnedCandidate_=candidateId;learnedElement_=candidate;
                shared_->targetHandle=candidateId; shared_->reservedWidth=size;
                InterlockedIncrement(&shared_->writes); shared_->state=Reserved;
            }
        }
        // 文字是装饰层；失败时仅移除文字，不能中断原生宽度保护。
        try {
            if(width_.Active())hint_.Update(candidate,shared_->showHints!=0,shared_->fixture!=0);
            else hint_.Clear();
        } catch(...) {hint_.Clear();}
        InterlockedIncrement(&shared_->sequence);
        shared_->hintVisible=hint_.Visible();shared_->hintLeft=hint_.Left();shared_->hintWidth=hint_.Width();
        shared_->count=static_cast<LONG>(snapshot.size()); shared_->tick=GetTickCount64();
        for (size_t i=0;i<snapshot.size();++i) shared_->nodes[i]=snapshot[i];
        InterlockedIncrement(&shared_->sequence);
    }
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        if (!site) { if(window_) PostMessageW(window_,WM_TIMER,0,0); return S_OK; }
        if (window_ || shared_) return HRESULT_FROM_WIN32(ERROR_BUSY);
        try {
            diagnostics_=nullptr;
            winrt::check_hresult(site->QueryInterface(diagnostics_.put()));
            BSTR init{}; winrt::check_hresult(diagnostics_->GetInitializationData(&init));
            std::wstring name=init?init:L""; SysFreeString(init);
            if (!name.starts_with(L"Local\\WindowMark.XamlContainerTrial.") || name.size()>120) return E_INVALIDARG;
            mapping_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
            if (!mapping_) return HRESULT_FROM_WIN32(GetLastError());
            shared_=static_cast<Block*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Block)));
            if (!shared_) { Finish(); return E_FAIL; }
            if (shared_->magic!=kMagic || shared_->version!=kVersion || shared_->targetPid!=GetCurrentProcessId() ||
                shared_->durationSeconds<5 || shared_->durationSeconds>90) { Finish(); return E_INVALIDARG; }
            // 原生入口须显式研究授权；一次订阅、后续租约复用，不能靠重复Advise绕过生命周期风险。
            wchar_t processPath[32768]{};GetModuleFileNameW(nullptr,processPath,32768);
            const wchar_t* processName=wcsrchr(processPath,L'\\');processName=processName?processName+1:processPath;
            const bool identity=shared_->fixture?_wcsicmp(processName,L"WindowMarkXamlContainerFixture.exe")==0:
                (shared_->nativeConsent && shared_->persistent && _wcsicmp(processName,L"explorer.exe")==0);
            if(!identity) { Finish();return E_ACCESSDENIED; }
            persistent_=shared_->persistent!=0;
            if(!shared_->fixture) {
                auto marker=L"Local\\WindowMark.XamlContainerTrial.NativeOnce07."+std::to_wstring(GetCurrentProcessId());
                gNativeMarker=OpenEventW(SYNCHRONIZE,FALSE,marker.c_str());
                if(!gNativeMarker){Finish();return E_ACCESSDENIED;}
            }
            owner_=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,shared_->ownerPid);
            if (!owner_) { Finish(); return E_ACCESSDENIED; }
            // 在建立驻留窗口前核验加载映像与局部根查询；不回退到全局订阅。
            RefreshLocalTree();
            begin_=GetTickCount64(); stopping_=false; stableCount_=0; lastCandidate_=0;
            WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=gModule; wc.lpszClassName=kWindowClass;
            if (!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { Finish(); return E_FAIL; }
            window_=CreateWindowExW(0,kWindowClass,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,gModule,this);
            if (!window_) { Finish(); return E_FAIL; }
            AddRef(); // 消息窗口的生命期。
            shared_->uiThread=GetCurrentThreadId(); shared_->state=Attached;
            if(!SetTimer(window_,1,250,nullptr)) {
                DestroyWindow(window_);window_=nullptr;Finish();Release();return E_FAIL;
            }
            subscribeResult_=S_OK;unsubscribeResult_=S_OK;
            PostMessageW(window_,kReady,0,0);return S_OK;
        } catch (winrt::hresult_error const& e) { Finish(); return e.code(); }
          catch (...) { Finish(); return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** out) noexcept override {
        if(!out) return E_POINTER; *out=nullptr;
        return diagnostics_?diagnostics_->QueryInterface(iid,out):E_FAIL;
    }
};
struct Factory : winrt::implements<Factory,IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID iid,void** object) noexcept override {
        if(outer) return CLASS_E_NOAGGREGATION;
        try { return winrt::make_self<Tap>()->QueryInterface(iid,object); } catch(...) {return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};
}
HRESULT WINAPI DllGetClassObject(REFCLSID clsid,REFIID iid,void** object) {
    if(clsid!=kClassId) return CLASS_E_CLASSNOTAVAILABLE;
    try { return winrt::make_self<Factory>()->QueryInterface(iid,object); } catch(...) {return E_OUTOFMEMORY;}
}
// 诊断运行时可能保留IObjectWithSite；不强行卸载导致悬空虚表。
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { gModule=module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
