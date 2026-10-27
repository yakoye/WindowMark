// 独立 TSF 实验：不注册/激活输入法，不占用 GUID_LBI_INPUTMODE。
#include <windows.h>
#include <ole2.h>
#include <olectl.h>
#include <msctf.h>
#include <ctfutb.h>
#include <ctffunc.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <new>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID kIds[2] = {
    {0xc67b5d22,0x32e8,0x4e7c,{0x88,0x26,0xa1,0xc2,0x73,0x9d,0x4f,0x19}},
    {0x7f935e10,0x6569,0x43a5,{0xb2,0x38,0x2d,0xe9,0x72,0x5a,0x61,0xe4}}
};
constexpr wchar_t kMutex[] = L"Local\\WindowMark.TsfSlotTrial.v1.Mutex";
constexpr wchar_t kEvent[] = L"Local\\WindowMark.TsfSlotTrial.v1.Stop";
HANDLE gStop = nullptr;
BOOL WINAPI ConsoleStop(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (gStop) SetEvent(gStop);
        return TRUE;
    }
    return FALSE; // 关闭控制台/强制终止不承诺 COM 清理完成。
}
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE v = nullptr) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct ComInit {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct StopHandler {
    explicit StopHandler(HANDLE event) { gStop = event; SetConsoleCtrlHandler(ConsoleStop, TRUE); }
    ~StopHandler() { SetConsoleCtrlHandler(ConsoleStop, FALSE); gStop = nullptr; }
};
struct Calls { unsigned info{}, status{}, icon{}, text{}, click{}, advise{}; };

HICON TransparentIcon() {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header); header.bV5Width = 32; header.bV5Height = -32;
    header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&header),
                                    DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    ZeroMemory(bits, 32 * 32 * 4);
    std::array<BYTE, 128> maskBits{}; maskBits.fill(0xff);
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, maskBits.data());
    ICONINFO info{}; info.fIcon = TRUE; info.hbmColor = color; info.hbmMask = mask;
    HICON result = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return result;
}

class EmptyItem final : public ITfLangBarItemButton, public ITfSource {
    LONG refs_{1};
    GUID id_;
    HICON icon_{};
    ComPtr<ITfLangBarItemSink> sink_;
    ~EmptyItem() { if (icon_) DestroyIcon(icon_); }
public:
    Calls calls;
    explicit EmptyItem(GUID id) : id_(id), icon_(TransparentIcon()) {}
    bool Ready() const { return icon_ != nullptr; }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfLangBarItem || iid == IID_ITfLangBarItemButton)
            *out = static_cast<ITfLangBarItemButton*>(this);
        else if (iid == IID_ITfSource) *out = static_cast<ITfSource*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override {
        if (!info) return E_POINTER;
        ++calls.info; *info = {};
        // 不冒用已安装输入法 CLSID；未绑定活动服务可能被系统忽略，正是实验边界。
        info->clsidService = GUID_NULL; info->guidItem = id_;
        info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
        info->ulSort = id_ == kIds[0] ? 0 : 1;
        return S_OK;
    }
    STDMETHODIMP GetStatus(DWORD* status) override {
        if (!status) return E_POINTER;
        ++calls.status; *status = TF_LBI_STATUS_DISABLED; return S_OK;
    }
    STDMETHODIMP Show(BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP GetTooltipString(BSTR* text) override { return EmptyString(text); }
    STDMETHODIMP OnClick(TfLBIClick, POINT, const RECT*) override { ++calls.click; return S_OK; }
    STDMETHODIMP InitMenu(ITfMenu*) override { return S_OK; }
    STDMETHODIMP OnMenuSelect(UINT) override { return S_OK; }
    STDMETHODIMP GetIcon(HICON* icon) override {
        if (!icon) return E_POINTER;
        ++calls.icon; *icon = CopyIcon(icon_);
        return *icon ? S_OK : E_FAIL;
    }
    STDMETHODIMP GetText(BSTR* text) override { ++calls.text; return EmptyString(text); }
    STDMETHODIMP AdviseSink(REFIID iid, IUnknown* sink, DWORD* cookie) override {
        if (!sink || !cookie) return E_POINTER;
        *cookie = TF_INVALID_COOKIE;
        if (iid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
        if (sink_) return CONNECT_E_ADVISELIMIT;
        const HRESULT hr = sink->QueryInterface(IID_PPV_ARGS(&sink_));
        if (FAILED(hr)) return hr;
        ++calls.advise; *cookie = 1; return S_OK;
    }
    STDMETHODIMP UnadviseSink(DWORD cookie) override {
        if (cookie != 1 || !sink_) return CONNECT_E_NOCONNECTION;
        sink_.Reset(); return S_OK;
    }
    static HRESULT EmptyString(BSTR* text) {
        if (!text) return E_POINTER;
        *text = SysAllocString(L""); return *text ? S_OK : E_OUTOFMEMORY;
    }
};

struct Log {
    Handle file;
    ULONGLONG start{GetTickCount64()};
    explicit Log(const wchar_t* path) : file(CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create new evidence file");
    }
    void Row(const char* phase, int item = -1, HRESULT hr = S_OK,
             const std::string& fields = "") {
        SYSTEMTIME t{}; GetSystemTime(&t);
        char stamp[40]{};
        sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
        std::string row = "{\"schema\":1,\"trial_version\":\"0.1\",\"utc\":\"" + std::string(stamp) +
            "\",\"elapsed_ms\":" + std::to_string(GetTickCount64()-start) +
            ",\"phase\":\"" + phase + "\",\"item\":" + std::to_string(item) +
            ",\"hr\":" + std::to_string(hr) +
            ",\"protection_active\":false,\"visibility_verified\":false" + fields + "}\n";
        DWORD wrote{};
        if (!WriteFile(file.value,row.data(),static_cast<DWORD>(row.size()),&wrote,nullptr) ||
            wrote != row.size() || !FlushFileBuffers(file.value)) throw std::runtime_error("Evidence write failed");
    }
};

// 对所有已尝试注册的项目逆序移除。AddItem 的 S_OK 不能作为“显示成功”。
template<class Add, class Remove> bool AddPair(Add add, Remove remove) {
    for (int i = 0; i < 2; ++i) {
        if (FAILED(add(i))) {
            for (int j = i; j >= 0; --j) remove(j);
            return false;
        }
    }
    return true;
}
class Trial {
    ITfLangBarItemMgr* manager_;
    Log& log_;
    std::array<ComPtr<EmptyItem>,2> items_;
    std::array<bool,2> attempted_{};
public:
    bool cleanupOk{true};
    Trial(ITfLangBarItemMgr* manager, Log& log) : manager_(manager), log_(log) {
        for (int i=0;i<2;++i) {
            items_[i].Attach(new EmptyItem(kIds[i]));
            if (!items_[i]->Ready()) throw std::runtime_error("Transparent icon creation failed");
        }
    }
    ~Trial() { for (int i=1;i>=0;--i) if (attempted_[i]) manager_->RemoveItem(items_[i].Get()); }
    bool Add() {
        // 已有相同私有标识则拒绝；不移除不属于本次实验的项目。
        for (int i=0;i<2;++i) {
            ComPtr<ITfLangBarItem> found;
            HRESULT hr = manager_->GetItem(kIds[i], &found);
            log_.Row("preflight_get",i,hr, found ? ",\"found\":true" : ",\"found\":false");
            if (found || hr != S_FALSE) return false;
        }
        return AddPair([this](int i) {
            attempted_[i] = true;
            HRESULT hr = manager_->AddItem(items_[i].Get());
            log_.Row("add",i,hr); return hr;
        }, [this](int i) { Remove(i); });
    }
    void Snapshot(const char* phase) {
        ULONG count{}; HRESULT hr = manager_->GetItemNum(&count);
        log_.Row(phase,-1,hr,",\"manager_item_count\":"+std::to_string(count));
        for (int i=0;i<2;++i) {
            ComPtr<ITfLangBarItem> found;
            hr = manager_->GetItem(kIds[i],&found);
            const Calls c = items_[i]->calls;
            log_.Row("item_snapshot",i,hr, std::string(",\"found\":")+(found?"true":"false")+
                ",\"info_calls\":"+std::to_string(c.info)+",\"status_calls\":"+std::to_string(c.status)+
                ",\"icon_calls\":"+std::to_string(c.icon)+",\"text_calls\":"+std::to_string(c.text)+
                ",\"click_calls\":"+std::to_string(c.click)+",\"advise_calls\":"+std::to_string(c.advise));
        }
    }
    void Remove(int i) {
        if (!attempted_[i]) return;
        HRESULT hr = manager_->RemoveItem(items_[i].Get());
        // 日志失败时析构仍会尝试移除。
        log_.Row("remove",i,hr);
        ComPtr<ITfLangBarItem> found;
        HRESULT getHr = manager_->GetItem(kIds[i],&found);
        const bool absent = SUCCEEDED(hr) && !found && getHr == S_FALSE;
        log_.Row("after_remove_get",i,getHr, std::string(",\"found\":")+(found?"true":"false")+
                  ",\"absence_confirmed\":"+(absent?"true":"false"));
        // 只有 Remove 成功且 GetItem 明确返回“不存在”才确认清理。
        if (absent) attempted_[i] = false;
        else cleanupOk = false;
    }
    void RemoveAll() { for (int i=1;i>=0;--i) Remove(i); }
};

bool Pump(unsigned seconds, HANDLE stop) {
    const ULONGLONG end = GetTickCount64() + seconds * 1000ULL;
    while (GetTickCount64() < end) {
        if (WaitForSingleObject(stop,0) == WAIT_OBJECT_0) return false;
        MsgWaitForMultipleObjects(1,&stop,FALSE,50,QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
    return true;
}
void Check(bool condition) { if (!condition) throw std::runtime_error("Self-test assertion failed"); }
int SelfTest() {
    ComPtr<EmptyItem> item; item.Attach(new EmptyItem(kIds[0])); Check(item->Ready());
    Check(kIds[0] != kIds[1] && kIds[0] != GUID_LBI_INPUTMODE && kIds[1] != GUID_LBI_INPUTMODE);
    TF_LANGBARITEMINFO info{}; Check(item->GetInfo(&info)==S_OK && info.clsidService==GUID_NULL && info.guidItem==kIds[0]);
    DWORD status{}; Check(item->GetStatus(&status)==S_OK && status==TF_LBI_STATUS_DISABLED);
    BSTR text{}; Check(item->GetText(&text)==S_OK && SysStringLen(text)==0); SysFreeString(text);
    Check(item->GetInfo(nullptr)==E_POINTER && item->GetIcon(nullptr)==E_POINTER);
    ComPtr<ITfSource> source; Check(item.As(&source)==S_OK);
    Check(source->UnadviseSink(1)==CONNECT_E_NOCONNECTION);
    HICON icon{}; Check(item->GetIcon(&icon)==S_OK);
    ICONINFO ii{}; Check(GetIconInfo(icon,&ii)!=0);
    std::array<DWORD,1024> pixels{}; pixels.fill(0xffffffff);
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=32;
    bi.bmiHeader.biHeight=-32; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    HDC dc=GetDC(nullptr);
    const int lines=GetDIBits(dc,ii.hbmColor,0,32,pixels.data(),&bi,DIB_RGB_COLORS);
    ReleaseDC(nullptr,dc); DeleteObject(ii.hbmColor); DeleteObject(ii.hbmMask); DestroyIcon(icon);
    Check(lines==32); for (DWORD pixel:pixels) Check(pixel==0);
    for (int fail=-1;fail<2;++fail) {
        std::string calls;
        bool success=AddPair([&](int i){calls+='0'+static_cast<char>(i);return i==fail?E_FAIL:S_OK;},
                            [&](int i){calls+='a'+static_cast<char>(i);});
        Check(success==(fail==-1));
        Check(calls==(fail==-1?"01":fail==0?"0a":"01ba"));
    }
    puts("Self-test passed: identity, disabled/empty contract, COM interfaces, transparent pixels, rollback ordering.");
    return 0;
}
unsigned Number(const wchar_t* text, unsigned lo, unsigned hi) {
    if (!text || !*text) throw std::runtime_error("Invalid number");
    for (const wchar_t* p=text;*p;++p) if (*p<L'0'||*p>L'9') throw std::runtime_error("Invalid number");
    wchar_t* end{}; unsigned long n=wcstoul(text,&end,10);
    if (*end || n<lo || n>hi) throw std::runtime_error("Number out of range");
    return static_cast<unsigned>(n);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc==1 || (argc==2 && wcscmp(argv[1],L"--help")==0)) {
            puts("WindowMark TSF slot trial 0.1 (NOT a guard).\n--run-trial --out NEW.jsonl [--seconds 5..90] [--baseline-seconds 2..15]\n--stop | --self-test\nPrivate IDs; no IME activation; auto-remove + 5 seconds recovery; normal cleanup logged."); return 0;
        }
        if (argc==2 && wcscmp(argv[1],L"--self-test")==0) return SelfTest();
        if (argc==2 && wcscmp(argv[1],L"--stop")==0) {
            Handle event(OpenEventW(EVENT_MODIFY_STATE,FALSE,kEvent));
            if (!event.value) return 4;
            return SetEvent(event.value)?0:4;
        }
        bool consent=false; const wchar_t* path=nullptr; unsigned seconds=45, baseline=8;
        for (int i=1;i<argc;++i) {
            std::wstring arg=argv[i];
            if (arg==L"--run-trial" && !consent) consent=true;
            else if (arg==L"--out" && i+1<argc && !path) path=argv[++i];
            else if (arg==L"--seconds" && i+1<argc) seconds=Number(argv[++i],5,90);
            else if (arg==L"--baseline-seconds" && i+1<argc) baseline=Number(argv[++i],2,15);
            else throw std::runtime_error("Unknown or missing argument");
        }
        if (!consent || !path) throw std::runtime_error("Explicit --run-trial and --out required");
        Handle mutex(CreateMutexW(nullptr,FALSE,kMutex));
        const DWORD mutexError=GetLastError();
        if (!mutex.value || mutexError==ERROR_ALREADY_EXISTS) return 4;
        Handle stop(CreateEventW(nullptr,TRUE,FALSE,kEvent));
        const DWORD eventError=GetLastError();
        if (!stop.value || eventError==ERROR_ALREADY_EXISTS) return 4;
        StopHandler handler(stop.value);
        Log log(path);
        log.Row("start",-1,S_OK,",\"duration_seconds\":"+std::to_string(seconds)+
            ",\"baseline_seconds\":"+std::to_string(baseline)+",\"pid\":"+std::to_string(GetCurrentProcessId()));
        ComInit com;
        log.Row("com_init",-1,com.hr); if (FAILED(com.hr)) return 2;
        ComPtr<ITfLangBarItemMgr> manager;
        HRESULT hr=CoCreateInstance(CLSID_TF_LangBarItemMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager));
        log.Row("manager_create",-1,hr); if (FAILED(hr)) return 2;
        Trial trial(manager.Get(),log);
        trial.Snapshot("baseline");
        if (!Pump(baseline,stop.value)) { log.Row("stopped_before_add"); return 0; }
        if (!trial.Add()) { trial.RemoveAll(); log.Row("registration_failed"); return 2; }
        trial.Snapshot("registered");
        bool completed=true;
        for (unsigned i=0;i<seconds;++i) {
            if (!Pump(1,stop.value)) { completed=false; break; }
            trial.Snapshot("holding");
        }
        log.Row(completed?"hold_complete":"stop_requested");
        trial.RemoveAll(); trial.Snapshot("removed");
        // 清理后停留5秒，给并行只读采样器观察布局恢复。停止请求不影响移除。
        ResetEvent(stop.value); Pump(5,stop.value);
        log.Row("end",-1,S_OK,trial.cleanupOk?",\"cleanup_confirmed\":true":",\"cleanup_confirmed\":false");
        return trial.cleanupOk?0:3;
    } catch (const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
