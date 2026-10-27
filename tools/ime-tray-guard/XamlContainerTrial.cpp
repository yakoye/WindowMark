#include <windows.h>
#include <ole2.h>
#include <xamlom.h>
#include "XamlContainerProtocol.h"
#include "ImeGuardClientProtocol.h"
#include "../../include/windowmark/core/ImeGuardPolicy.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>

using namespace container_trial;
namespace {
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE v=nullptr):value(v){}
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
std::atomic_bool stopRequested{};
BOOL WINAPI Control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT) {stopRequested=true;return TRUE;} return FALSE;
}
std::string Quote(const wchar_t* text) {
    std::string out="\"";
    for(;*text;++text) {
        if(*text==L'"'||*text==L'\\') {out+='\\';out+=static_cast<char>(*text);}
        else if(*text>=32 && *text<127) out+=static_cast<char>(*text);
        else out+='?';
    }
    return out+'"';
}
double Finite(double value){return std::isfinite(value)?value:0;}
unsigned Number(const wchar_t* value,unsigned low,unsigned high) {
    std::wstring s=value; if(s.empty()||s.find_first_not_of(L"0123456789")!=s.npos) throw std::runtime_error("Invalid number");
    auto n=std::stoul(s); if(n<low||n>high)throw std::runtime_error("Out of range");return static_cast<unsigned>(n);
}
std::wstring ProcessPath(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if(!process.value)throw std::runtime_error("Target process unavailable");
    wchar_t text[32768]{}; DWORD length=32768;
    if(!QueryFullProcessImageNameW(process.value,0,text,&length))throw std::runtime_error("Cannot identify target process");
    return text;
}
struct Log {
    Handle file;
    explicit Log(const wchar_t* path):file(CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)) {
        if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Evidence exists or cannot be created");
    }
    void Write(std::string const& text) {
        auto line=text+'\n';DWORD written{};
        if(!WriteFile(file.value,line.data(),static_cast<DWORD>(line.size()),&written,nullptr)||written!=line.size()||!FlushFileBuffers(file.value))throw std::runtime_error("Cannot write evidence");
    }
    void Snapshot(Block const& b) {
        SYSTEMTIME t{};GetSystemTime(&t);char utc[40]{};
        sprintf_s(utc,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
        std::ostringstream out;
        out<<"{\"schema\":1,\"utc\":\""<<utc<<"\",\"state\":"<<b.state<<",\"tick\":"<<b.tick
           <<",\"target_pid\":"<<b.targetPid<<",\"ui_thread\":"<<b.uiThread<<",\"fixture\":"<<(b.fixture?"true":"false")
           <<",\"writes\":"<<b.writes<<",\"restores\":"<<b.restores<<",\"restore_failures\":"<<b.restoreFailures
           <<",\"subscribe_hr\":"<<b.subscribeHr<<",\"unsubscribe_hr\":"<<b.unsubscribeHr<<",\"overflow\":"<<b.overflow
           <<",\"errors\":"<<b.errors<<",\"target_handle\":"<<b.targetHandle<<",\"reserved_width\":"<<Finite(b.reservedWidth)
           <<",\"persistent\":"<<(b.persistent?"true":"false")<<",\"subscription_active\":"<<b.subscriptionActive
           <<",\"hint_visible\":"<<b.hintVisible<<",\"hint_left\":"<<Finite(b.hintLeft)<<",\"hint_width\":"<<Finite(b.hintWidth)
           <<",\"protection_validated\":false,\"nodes\":[";
        for(LONG i=0;i<b.count && i<static_cast<LONG>(kCapacity);++i) {
            if(i)out<<',';auto const& n=b.nodes[i];
            out<<"{\"id\":"<<n.handle<<",\"parent\":"<<n.parent<<",\"type\":"<<Quote(n.type)<<",\"name\":"<<Quote(n.name)
               <<",\"width\":"<<Finite(n.width)<<",\"height\":"<<Finite(n.height)<<",\"min_width\":"<<Finite(n.minWidth)
               <<",\"x\":"<<Finite(n.x)<<",\"y\":"<<Finite(n.y)<<",\"visible\":"<<n.visible<<",\"hit_test\":"<<n.hitTest
               <<",\"binding\":"<<n.hasBinding<<",\"local_kind\":"<<n.localKind<<",\"hr\":"<<n.hr<<'}';
        }
        out<<"]}";Write(out.str());
    }
};
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==1 || (argc==2 && std::wstring(argv[1])==L"--help")) {
            puts("XAML local-root controller 0.5 - RESEARCH, not a released feature.\n--fixture-pid PID --reserve --out NEW.jsonl [--seconds 5..90] [--persistent] [--toggle-ms 3000..10000]\nNative attachment DISABLED by default. Explicit --native-research --persistent plus --observe-taskbar/--reserve-taskbar required.\nNo global visual-tree subscription; exact loaded-image identity required. Later leases reuse the local broker. Unknown Windows builds refused.\nCtrl+C restores the current lease. DLL remains until target exits; idle mode does not override layout.");return 0;
        }
        bool native=false,reserve=false,persistent=false,nativeConsent=false,hints=false;DWORD pid{};unsigned seconds=30, firstEndpoint=1,toggleMs=0;const wchar_t* path=nullptr;const wchar_t* clientName=nullptr;
        for(int i=1;i<argc;++i) {
            std::wstring arg=argv[i];
            if(arg==L"--observe-taskbar"&&!native)native=true;
            else if(arg==L"--show-hints")hints=true;
            else if(arg==L"--reserve-taskbar"&&!native&&!reserve){native=true;reserve=true;}
            else if(arg==L"--reserve"&&!reserve)reserve=true;
            else if(arg==L"--persistent"&&!persistent)persistent=true;
            else if(arg==L"--native-research"&&!nativeConsent)nativeConsent=true;
            else if(arg==L"--fixture-pid"&&i+1<argc&&!pid)pid=Number(argv[++i],1,0x7fffffff);
            else if(arg==L"--seconds"&&i+1<argc)seconds=Number(argv[++i],5,90);
            else if(arg==L"--toggle-ms"&&i+1<argc&&!toggleMs)toggleMs=Number(argv[++i],3000,10000);
            else if(arg==L"--endpoint"&&i+1<argc)firstEndpoint=Number(argv[++i],1,16);
            else if(arg==L"--out"&&i+1<argc&&!path)path=argv[++i];
            else if(arg==L"--client-map"&&i+1<argc&&!clientName)clientName=argv[++i];
            else throw std::runtime_error("Unknown/missing argument");
        }
        if(!path || native==static_cast<bool>(pid)) throw std::runtime_error("Unsupported/ambiguous mode");
        if(native&&(!nativeConsent||!persistent)) throw std::runtime_error("Native Explorer attachment disabled without explicit research/persistent flags; no attachment attempted");
        if(nativeConsent&&!native)throw std::runtime_error("Research flag requires explicit taskbar target");
        if(toggleMs&&!reserve)throw std::runtime_error("Toggle requires fixture reservation");
        if(clientName&&(!persistent||!reserve||toggleMs))throw std::runtime_error("Client lease requires persistent reservation without toggle");
        Handle clientMapping,clientOwner;
        struct ClientView {
            ime_guard::ClientBlock* p{};
            ~ClientView(){if(p)UnmapViewOfFile(p);}
        } client;
        if(clientName) {
            const std::wstring name=clientName;
            if(!name.starts_with(L"Local\\WindowMark.ImeGuard.Client.")||name.size()>120)throw std::runtime_error("Invalid client mapping");
            clientMapping.value=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,clientName);
            if(!clientMapping.value)throw std::runtime_error("Client mapping unavailable");
            client.p=static_cast<ime_guard::ClientBlock*>(MapViewOfFile(clientMapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(ime_guard::ClientBlock)));
            if(!client.p||client.p->magic!=ime_guard::kClientMagic||client.p->version!=ime_guard::kClientVersion)throw std::runtime_error("Invalid client protocol");
            DWORD session{},ownSession{};
            clientOwner.value=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,client.p->ownerPid);
            const auto heartbeat=static_cast<ULONGLONG>(InterlockedCompareExchange64(&client.p->heartbeat,0,0));
            if(!clientOwner.value||WaitForSingleObject(clientOwner.value,0)!=WAIT_TIMEOUT||client.p->stop||
               !ProcessIdToSessionId(client.p->ownerPid,&session)||!ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)||session!=ownSession||
               heartbeat>GetTickCount64()||GetTickCount64()-heartbeat>2000)throw std::runtime_error("Client is not alive");
            seconds=30; // 首次附加仍有截止时间，只有健康界面才能续租。
        }
        if(native) {
            wchar_t build[32]{};DWORD bytes=sizeof(build);
            const auto key=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            if(RegGetValueW(HKEY_LOCAL_MACHINE,key,L"CurrentBuildNumber",RRF_RT_REG_SZ,nullptr,build,&bytes)!=ERROR_SUCCESS ||
               !windowmark::ImeGuardPlatformEligible(static_cast<unsigned>(wcstoul(build,nullptr,10))))
                throw std::runtime_error("Windows 11 taskbar required; loaded image identity is checked by native component");
        }
        if(native) {
            HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
            if(!taskbar)throw std::runtime_error("No taskbar in current session");
            GetWindowThreadProcessId(taskbar,&pid);
        }
        const auto executable=std::filesystem::path(ProcessPath(pid)).filename().wstring();
        if(_wcsicmp(executable.c_str(),native?L"explorer.exe":L"WindowMarkXamlContainerFixture.exe")!=0)throw std::runtime_error("Target identity mismatch");
        DWORD ownSession{},targetSession{};
        if(!ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)||!ProcessIdToSessionId(pid,&targetSession)||ownSession!=targetSession)throw std::runtime_error("Session mismatch");
        Handle target(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        USHORT processMachine{},nativeMachine{};
        if(!target.value || !IsWow64Process2(target.value,&processMachine,&nativeMachine) || processMachine!=IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine!=IMAGE_FILE_MACHINE_AMD64)throw std::runtime_error("Only native x64 targets validated");
        Handle singleton(CreateMutexW(nullptr,FALSE,L"Local\\WindowMark.XamlContainerTrial.Controller"));
        if(!singleton.value||GetLastError()==ERROR_ALREADY_EXISTS)return 4;
        GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("GUID failed");
        wchar_t id[40]{};StringFromGUID2(guid,id,40);
        std::wstring mappingName=L"Local\\WindowMark.XamlContainerTrial."+std::wstring(id);
        Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Block),mappingName.c_str()));
        if(!mapping.value)throw std::runtime_error("Mapping failed");
        auto view=static_cast<Block*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Block)));
        if(!view)throw std::runtime_error("Mapping view failed");
        struct ViewGuard {Block* p;~ViewGuard(){if(p){InterlockedExchange(&p->stop,1);UnmapViewOfFile(p);}}} guard{view};
        new(view) Block{};view->ownerPid=GetCurrentProcessId();view->targetPid=pid;view->durationSeconds=seconds;
        view->reserve=reserve;view->fixture=!native;
        view->persistent=persistent;view->nativeConsent=nativeConsent;
        view->showHints=hints;
        wchar_t ownExe[32768]{};GetModuleFileNameW(nullptr,ownExe,32768);
        auto dll=std::filesystem::path(ownExe).parent_path()/L"WindowMarkXamlContainerTap.dll";
        if(!std::filesystem::is_regular_file(dll))throw std::runtime_error("TAP DLL missing");
        Log log(path);
        HWND broker{};
        if(persistent) {
            HWND found{};
            while((found=FindWindowExW(HWND_MESSAGE,found,kDispatchClass,nullptr))!=nullptr) {
                DWORD foundPid{};GetWindowThreadProcessId(found,&foundPid);
                if(foundPid==pid){broker=found;break;}
            }
        }
        Handle nativeMarker;
        if(native&&!broker) {
            auto marker=L"Local\\WindowMark.XamlContainerTrial.NativeOnce07."+std::to_wstring(pid);
            nativeMarker.value=CreateEventW(nullptr,TRUE,FALSE,marker.c_str());
            if(!nativeMarker.value||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Native process already attempted; refusing to replay all roots again");
        }
        HMODULE xaml=LoadLibraryExW(L"Windows.UI.Xaml.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!xaml)throw std::runtime_error("System XAML unavailable");
        auto initialize=reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(GetProcAddress(xaml,"InitializeXamlDiagnosticsEx"));
        if(!initialize)throw std::runtime_error("Diagnostics API unavailable");
        SetConsoleCtrlHandler(Control,TRUE);
        HRESULT attached=E_FAIL;
        // 只在端点不存在时尝试下一个；成功后一次订阅，不轮询式重复附加。
        if(broker) {
            COPYDATASTRUCT data{};data.dwData=kMagic;data.cbData=static_cast<DWORD>((mappingName.size()+1)*sizeof(wchar_t));data.lpData=mappingName.data();
            DWORD_PTR accepted{};
            attached=SendMessageTimeoutW(broker,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&accepted)&&accepted?S_OK:E_FAIL;
            log.Write("{\"event\":\"reuse_local_roots\",\"hr\":"+std::to_string(attached)+"}");
        }
        for(unsigned endpoint=firstEndpoint;!broker&&endpoint<=16;++endpoint) {
            std::wstring connection=L"VisualDiagConnection"+std::to_wstring(endpoint);
            attached=initialize(connection.c_str(),pid,L"",dll.c_str(),kClassId,mappingName.c_str());
            log.Write("{\"event\":\"attach\",\"endpoint\":"+std::to_string(endpoint)+",\"hr\":"+std::to_string(attached)+"}");
            if(attached!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND))break;
        }
        view->attachHr=attached;
        if(FAILED(attached))return 2;
        const auto begin=GetTickCount64();ULONGLONG lastTick{};LONG lastState=-1;
        auto copy=std::make_unique<Block>();ULONGLONG lastLogged{};
        ULONGLONG stopAt{};
        while(client.p || GetTickCount64()-begin<(seconds+15)*1000ULL) {
            if(client.p) {
                InterlockedExchange(&view->showHints,client.p->showHints);
                const auto now=GetTickCount64();
                const auto beat=static_cast<ULONGLONG>(InterlockedCompareExchange64(&client.p->heartbeat,0,0));
                const bool alive=!client.p->stop && WaitForSingleObject(clientOwner.value,0)==WAIT_TIMEOUT && beat<=now && now-beat<=2000;
                // 附加后的首个采样最多等十秒；已观测的 UI 线程失联后不再续租。
                const bool targetStale = view->tick ? (now > view->tick && now-view->tick>3000) : now-begin>10000;
                if(!alive||stopRequested||targetStale) {
                    InterlockedExchange(&view->stop,1);
                    if(!stopAt)stopAt=now;
                    if(now-stopAt>5000){log.Write("{\"event\":\"client_stop_cleanup_timeout\"}");return 3;}
                } else if(!stopAt) {
                    // 原协议的期限是相对附加时间的秒数。每次只续五秒，不去掉目标端看门狗。
                    // 控制程序挂起/死亡时，不会留下无限期宽度覆盖。
                    const auto elapsed=(now-begin)/1000;
                    if(elapsed>86400){InterlockedExchange(&view->stop,1);stopAt=now;}
                    else InterlockedExchange(reinterpret_cast<volatile LONG*>(&view->durationSeconds),static_cast<LONG>(elapsed+5));
                }
                InterlockedExchange(&client.p->state,view->state);
                InterlockedExchange(&client.p->writes,view->writes);
                InterlockedExchange(&client.p->restores,view->restores);
                InterlockedExchange(&client.p->restoreFailures,view->restoreFailures);
                InterlockedExchange(&client.p->targetPid,static_cast<LONG>(pid));
                InterlockedExchange64(&client.p->widthMilliDip,static_cast<LONG64>(Finite(view->reservedWidth)*1000));
                InterlockedExchange64(&client.p->targetHeartbeat,static_cast<LONG64>(view->tick));
            }
            if(toggleMs)InterlockedExchange(&view->reserve,((GetTickCount64()-begin)/toggleMs)%2==0?1:0);
            if(stopRequested)InterlockedExchange(&view->stop,1);
            LONG before=InterlockedCompareExchange(&view->sequence,0,0);
            if(!(before&1)) {
                CopyMemory(copy.get(),view,sizeof(Block));MemoryBarrier();
                LONG after=InterlockedCompareExchange(&view->sequence,0,0);
                if(before==after && (copy->tick!=lastTick||copy->state!=lastState)) {
                    if(!client.p || copy->state!=lastState || GetTickCount64()-lastLogged>=5000) {
                        if(client.p)copy->count=0; // 常驻日志不持续复制视觉树，避免长期运行产生巨量文件。
                        log.Snapshot(*copy);lastLogged=GetTickCount64();
                    }
                    lastTick=copy->tick;lastState=copy->state;
                }
            }
            if(view->state==Closed) {
                if(client.p){InterlockedExchange(&client.p->state,Closed);InterlockedExchange(&client.p->restores,view->restores);InterlockedExchange(&client.p->restoreFailures,view->restoreFailures);}
                return view->restoreFailures?3:0;
            }
            if(view->state==Failed)return 3;
            if(WaitForSingleObject(target.value,100)==WAIT_OBJECT_0) {log.Write("{\"event\":\"target_exited\"}");return 3;}
        }
        InterlockedExchange(&view->stop,1);log.Write("{\"event\":\"timeout_cleanup_not_confirmed\"}");return 3;
    } catch(std::exception const& e) {fprintf(stderr,"%s\n",e.what());return 1;}
}
