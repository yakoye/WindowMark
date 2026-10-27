// 只在自身宿主验证 GetUiLayer 是否能在不订阅全局视觉树的情况下返回局部根。
#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/base.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include "LocalIslandRoots.h"
#include "XamlContainerProtocol.h"
#include <vector>
using namespace container_trial;
using namespace winrt::Windows::UI::Xaml;
namespace {
class Probe : public winrt::implements<Probe,IObjectWithSite> {
    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        if(!site)return S_OK;
        HANDLE mapping{};Block* shared{};
        HRESULT result=E_FAIL;
        try {
            winrt::check_hresult(site->QueryInterface(diagnostics_.put()));
            BSTR init{};winrt::check_hresult(diagnostics_->GetInitializationData(&init));
            std::wstring name=init?init:L"";SysFreeString(init);
            if(!name.starts_with(L"Local\\WindowMark.XamlContainerTrial."))return E_INVALIDARG;
            mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
            if(!mapping)return HRESULT_FROM_WIN32(GetLastError());
            shared=static_cast<Block*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Block)));
            if(!shared){CloseHandle(mapping);return E_FAIL;}
            wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
            const auto leaf=wcsrchr(path,L'\\');
            const bool identity=leaf && (shared->fixture ?
                _wcsicmp(leaf+1,L"WindowMarkXamlContainerFixture.exe")==0 :
                shared->nativeConsent && _wcsicmp(leaf+1,L"explorer.exe")==0);
            if(shared->magic!=kMagic || shared->version!=kVersion ||
               shared->targetPid!=GetCurrentProcessId() || !identity || shared->reserve) {
                result=E_ACCESSDENIED;
            } else {
                shared->uiThread=GetCurrentThreadId();
                local_roots::Roots roots;
                result=local_roots::Read(roots);
                shared->subscribeHr=result; // 本实验记录根查询结果，没有执行 Advise。
                if(SUCCEEDED(result)) {
                    for(auto const& raw:roots) {
                        winrt::Windows::Foundation::IInspectable object{nullptr};
                        winrt::copy_from_abi(object,raw.Get());
                        auto source=object.try_as<Hosting::DesktopWindowXamlSource>();
                        auto content=source?source.Content():nullptr;
                        if(!content)continue; // 当前线程的其它根可能没有应用内容。
                        else {
                            std::vector<DependencyObject> pending{content};
                            while(!pending.empty()&&shared->count<static_cast<LONG>(kCapacity)) {
                                auto current=pending.back();pending.pop_back();
                                auto& n=shared->nodes[shared->count++];
                                n.handle=reinterpret_cast<uintptr_t>(winrt::get_abi(current));
                                const auto type=winrt::get_class_name(current);
                                wcsncpy_s(n.type,type.c_str(),_TRUNCATE);
                                if(auto fe=current.try_as<FrameworkElement>()) {
                                    wcsncpy_s(n.name,fe.Name().c_str(),_TRUNCATE);
                                    n.width=fe.ActualWidth();n.height=fe.ActualHeight();
                                    n.minWidth=fe.MinWidth();n.visible=fe.Visibility()==Visibility::Visible;
                                }
                                const auto count=Media::VisualTreeHelper::GetChildrenCount(current);
                                for(int i=0;i<count;++i)pending.push_back(Media::VisualTreeHelper::GetChild(current,i));
                            }
                        }
                    }
                    if(shared->count==0)result=HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                }
            }
        } catch(winrt::hresult_error const& e){result=e.code();}
          catch(...){result=E_FAIL;}
        if(shared){shared->attachHr=result;shared->tick=GetTickCount64();shared->state=SUCCEEDED(result)?Closed:Failed;UnmapViewOfFile(shared);}
        if(mapping)CloseHandle(mapping);
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** out) noexcept override {
        if(!out)return E_POINTER;*out=nullptr;
        return diagnostics_?diagnostics_->QueryInterface(iid,out):E_FAIL;
    }
};
class Factory : public winrt::implements<Factory,IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID iid,void** out) noexcept override {
        if(outer)return CLASS_E_NOAGGREGATION;
        try{return winrt::make_self<Probe>()->QueryInterface(iid,out);}catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override{return S_OK;}
};
}
HRESULT WINAPI DllGetClassObject(REFCLSID clsid,REFIID iid,void** out) {
    if(clsid!=kClassId)return CLASS_E_CLASSNOTAVAILABLE;
    try{return winrt::make_self<Factory>()->QueryInterface(iid,out);}catch(...){return E_OUTOFMEMORY;}
}
HRESULT WINAPI DllCanUnloadNow(){return S_FALSE;}
