// 只打开现有转储，不支持附加活进程。避免为排错再次触发Explorer崩溃。
#include <windows.h>
#include <dbgeng.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
class DebugOutput final : public IDebugOutputCallbacks {
    LONG refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IDebugOutputCallbacks)){*out=this;AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release() override {auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Output(ULONG,PCSTR text) override {fputs(text,stdout);fflush(stdout);return S_OK;}
};
int wmain(int argc,wchar_t** argv) {
    if(argc<2){puts("Usage: WindowMarkDumpInspect existing.dmp [debugger-command ...]. Offline dumps only.");return 1;}
    ComPtr<IDebugClient4> client;ComPtr<IDebugControl> control;
    HRESULT hr=DebugCreate(__uuidof(IDebugClient4),reinterpret_cast<void**>(client.GetAddressOf()));
    if(FAILED(hr)){printf("DebugCreate: %08lx\n",static_cast<unsigned long>(hr));return 2;}
    ComPtr<IDebugOutputCallbacks> output;output.Attach(new DebugOutput());
    client->SetOutputCallbacks(output.Get());client.As(&control);
    ComPtr<IDebugSymbols> symbols;client.As(&symbols);symbols->SetSymbolPath(".");
    hr=client->OpenDumpFileWide(argv[1],0);
    if(SUCCEEDED(hr))hr=control->WaitForEvent(0,INFINITE);
    if(FAILED(hr)){printf("Open/wait: %08lx\n",static_cast<unsigned long>(hr));return 3;}
    // 调用者控制输出范围，只输出诊断栈，不输出用户文档/剪贴板或完整内存。
    for(int i=2;i<argc;++i){
        int count=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::vector<char> command(count);WideCharToMultiByte(CP_UTF8,0,argv[i],-1,command.data(),count,nullptr,nullptr);
        hr=control->Execute(DEBUG_OUTCTL_THIS_CLIENT,command.data(),DEBUG_EXECUTE_DEFAULT);
        if(FAILED(hr))printf("Command HRESULT: %08lx\n",static_cast<unsigned long>(hr));
    }
    client->EndSession(DEBUG_END_PASSIVE);client->SetOutputCallbacks(nullptr);return 0;
}
