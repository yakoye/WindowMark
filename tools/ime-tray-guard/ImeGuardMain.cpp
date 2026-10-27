#include "ImeGuardClient.h"
#include "XamlContainerProtocol.h"
#include <shellapi.h>
#include <shlobj.h>
#include <string>

namespace {
constexpr wchar_t kClass[]=L"WindowMark.ImeTrayGuard.Preview04";
constexpr int kEnable=101,kRestore=102,kClose=103;
ime_guard::Client client;
HWND statusText{},enableButton{},restoreButton{};
HFONT font{};
bool closing{};
ULONGLONG closeTick{};
std::filesystem::path folder,log;
HANDLE owner{};
DWORD fixturePid{};
bool Compatible() {
    if(fixturePid)return true;
    wchar_t build[32]{};DWORD bytes=sizeof(build),ubr{},ubrBytes=sizeof(ubr);
    const auto key=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    return RegGetValueW(HKEY_LOCAL_MACHINE,key,L"CurrentBuildNumber",RRF_RT_REG_SZ,nullptr,build,&bytes)==ERROR_SUCCESS &&
        RegGetValueW(HKEY_LOCAL_MACHINE,key,L"UBR",RRF_RT_REG_DWORD,nullptr,&ubr,&ubrBytes)==ERROR_SUCCESS &&
        std::wstring(build)==L"26300" && ubr==9711;
}
std::wstring StateText() {
    const auto& s=client.Current();
    if(!Compatible())return L"此系统尚未验证，不附加任务栏。Windows 10 支持仍在开发。";
    if(!s.error.empty())return s.error;
    if(!s.running) {
        if(s.writes && s.restores==s.writes && !s.restoreFailures)return L"已关闭 · 本次宽度修改已恢复。原生诊断连接保留至 Explorer 退出。";
        return L"已关闭 · 未修改任务栏。";
    }
    if(closing || s.stopping || s.state==container_trial::Restored)return L"正在恢复系统默认布局……";
    if(s.state==container_trial::Reserved)return L"保护生效 · 原生容器 "+std::to_wstring(static_cast<int>(s.width))+L" DIP；未绘制替代图标。";
    if(s.state==container_trial::Observing)return L"等待原生输入法图标出现并稳定；当前尚未固定宽度。";
    return L"正在检查并连接原生任务栏……";
}
void Refresh() {
    const auto text=StateText();SetWindowTextW(statusText,text.c_str());
    EnableWindow(enableButton,!closing && !client.Current().running && Compatible());
    EnableWindow(restoreButton,!closing && client.Current().running && !client.Current().stopping);
    const auto focused=GetFocus();
    if(!closing && GetForegroundWindow()==GetParent(statusText)) {
        if((focused==enableButton&&!IsWindowEnabled(enableButton))||(!focused&&IsWindowEnabled(restoreButton)))SetFocus(restoreButton);
        else if((focused==restoreButton&&!IsWindowEnabled(restoreButton))||(!focused&&IsWindowEnabled(enableButton)))SetFocus(enableButton);
    }
}
HWND Add(HWND parent,const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
    const auto dpi=GetDpiForWindow(parent);
    auto scale=[dpi](int n){return MulDiv(n,static_cast<int>(dpi),96);};
    HWND control=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(w),scale(h),parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return control;
}
void Start() {
    if(!Compatible())return;
    try {
        wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);folder=std::filesystem::path(path).parent_path();
        PWSTR local{};
        if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))throw 1;
        const auto logs=std::filesystem::path(local)/L"WindowMark"/L"ImeTrayGuardPreview";CoTaskMemFree(local);
        log=logs/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".jsonl");
        client.Start(folder,log,fixturePid);
    } catch(...) {SetWindowTextW(statusText,L"不能创建诊断目录；未启用。");return;}
    Refresh();
}
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE: {
        font=CreateFontW(-MulDiv(10,static_cast<int>(GetDpiForWindow(window)),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        Add(window,L"STATIC",L"任务栏输入法占位保护 · 研究预览",0,20,18,580,28,0);
        Add(window,L"STATIC",L"保留原生「中」「拼」的空间。图标消失时留空，不补画，不添加托盘图标。",0,20,55,580,44,0);
        Add(window,L"STATIC",L"仅实测 Windows 11 26300.9711。首次连接仍可能影响 Explorer；不支持的版本拒绝启用。尚未完成长期、多屏和 Windows 10 验收。",0,20,103,580,55,0);
        statusText=Add(window,L"STATIC",L"",0,20,170,580,52,0);
        enableButton=Add(window,L"BUTTON",L"启用保护（预览）(&E)",BS_PUSHBUTTON|WS_TABSTOP,20,234,185,32,kEnable);
        restoreButton=Add(window,L"BUTTON",L"恢复系统默认(&R)",BS_PUSHBUTTON|WS_TABSTOP,220,234,175,32,kRestore);
        Add(window,L"BUTTON",L"关闭",BS_PUSHBUTTON|WS_TABSTOP,420,234,175,32,kClose);
        if(!SetTimer(window,1,250,nullptr))return -1;
        Refresh();return 0;
    }
    case WM_COMMAND:
        if(LOWORD(w)==kEnable)Start();
        else if(LOWORD(w)==kRestore){client.Stop();SetWindowTextW(statusText,L"已请求恢复，正在等待原值回读确认……");EnableWindow(restoreButton,FALSE);}
        else if(LOWORD(w)==kClose)SendMessageW(window,WM_CLOSE,0,0);
        return 0;
    case WM_TIMER:
        client.Tick();
        if(owner && WaitForSingleObject(owner,0)!=WAIT_TIMEOUT && !closing)SendMessageW(window,WM_CLOSE,0,0);
        Refresh();
        if(closing && (!client.Current().running || GetTickCount64()-closeTick>6500))DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        if(!closing){closing=true;closeTick=GetTickCount64();client.Stop();Refresh();}
        if(!client.Current().running)DestroyWindow(window);
        return 0;
    case WM_QUERYENDSESSION:client.Stop();return TRUE;
    case WM_DESTROY:KillTimer(window,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool valid=argv!=nullptr;
    for(int i=1;valid&&i<argc;++i) {
        const std::wstring arg=argv[i];
        if((arg==L"--owner-pid"||arg==L"--fixture-pid")&&i+1<argc) {
            const auto value=std::wstring(argv[++i]);
            if(value.empty()||value.size()>9||value.find_first_not_of(L"0123456789")!=value.npos){valid=false;break;}
            const auto pid=static_cast<DWORD>(std::stoul(value));
            if(!pid){valid=false;break;}
            if(arg==L"--fixture-pid")fixturePid=pid;
            else {
                DWORD ownSession{},targetSession{};
                owner=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
                valid=owner&&ProcessIdToSessionId(pid,&targetSession)&&ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)&&ownSession==targetSession;
            }
        } else valid=false;
    }
    if(argv)LocalFree(argv);
    if(!valid){if(owner)CloseHandle(owner);return 1;}
    HANDLE singleton=CreateMutexW(nullptr,FALSE,L"Local\\WindowMark.ImeTrayGuard.Preview04");
    if(!singleton||GetLastError()==ERROR_ALREADY_EXISTS){if(auto existing=FindWindowW(kClass,nullptr)){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}if(singleton)CloseHandle(singleton);if(owner)CloseHandle(owner);return 4;}
    WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=instance;wc.lpszClassName=kClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_3DFACE+1);
    if(!RegisterClassW(&wc)){CloseHandle(singleton);return 2;}
    const auto dpi=GetDpiForSystem();RECT size{0,0,MulDiv(620,static_cast<int>(dpi),96),MulDiv(290,static_cast<int>(dpi),96)};
    AdjustWindowRectExForDpi(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
    HWND window=CreateWindowExW(0,kClass,L"WindowMark 输入法占位保护（研究预览）",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,nullptr,nullptr,instance,nullptr);
    if(!window){CloseHandle(singleton);return 3;}
    ShowWindow(window,show);MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {
        if(message.message==WM_SYSKEYDOWN && message.wParam=='E' && IsWindowEnabled(enableButton)) {SendMessageW(window,WM_COMMAND,kEnable,0);continue;}
        if(message.message==WM_SYSKEYDOWN && message.wParam=='R' && IsWindowEnabled(restoreButton)) {SendMessageW(window,WM_COMMAND,kRestore,0);continue;}
        if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(font)DeleteObject(font);if(owner)CloseHandle(owner);CloseHandle(singleton);return 0;
}
