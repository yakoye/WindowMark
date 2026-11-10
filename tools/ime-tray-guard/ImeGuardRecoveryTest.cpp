// 只操作本测试的后台窗口和自建 XAML 宿主；不广播、不改变系统 DPI/会话状态。
#include "WinImeTrayGuard.h"
#include <wtsapi32.h>
#include <cstdio>
#include <functional>
#include <fstream>
#include <sstream>
using windowmark::win::WinImeTrayGuard;
bool Wait(std::function<bool()> const& condition,unsigned ms) {
    const auto deadline=GetTickCount64()+ms;
    do {
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(condition())return true;
        MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT);
    }while(GetTickCount64()<deadline);
    return condition();
}
struct Fixture {
    HANDLE process{}; DWORD pid{};
    bool Start(std::filesystem::path const& directory) {
        auto exe=directory/L"WindowMarkXamlContainerFixture.exe";
        std::wstring command=L"\""+exe.wstring()+L"\" --seconds 90 --always-visible";
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;
        process=pi.hProcess;pid=pi.dwProcessId;CloseHandle(pi.hThread);
        Wait([]{return false;},1000);return true;
    }
    void Stop(){if(process){TerminateProcess(process,0);WaitForSingleObject(process,5000);CloseHandle(process);process=nullptr;}}
    ~Fixture(){Stop();}
};
std::string Read(std::filesystem::path const& path){std::ifstream in(path);std::ostringstream out;out<<in.rdbuf();return out.str();}
void Write(std::filesystem::path const& path,std::string const& text){std::ofstream out(path);out<<text;}
int Lifecycle(std::filesystem::path const& directory,std::filesystem::path const& data) {
    constexpr DWORD missing=0x7ffffffe;
    unsigned disabled{}; bool savedEnabled{};
    auto persist=[&](bool on){savedEnabled=on;if(!on)++disabled;};
    const auto marker=data/L"active-session.lock";
    Fixture first,second;
    WinImeTrayGuard guard;guard.UseFixture(missing);guard.onRequestedChanged=persist;
    if(!guard.Initialize(directory,data))return 20;
    guard.ResumeSavedPreference();
    Wait([]{return false;},2500);
    if(guard.Active()||!guard.Requested()||!savedEnabled)return 21;
    if(!first.Start(directory))return 22;
    guard.UseFixture(first.pid);
    if(!Wait([&]{return guard.Active();},12000))return 23;
    puts("startup_not_ready_preserves_setting=PASS");fflush(stdout);
    SendMessageW(guard.TestWindow(),RegisterWindowMessageW(L"TaskbarCreated"),0,0);
    if(!Wait([&]{return !guard.Active();},1000)||!Wait([&]{return guard.Active();},12000)||disabled)return 24;
    puts("taskbar_created_recovers=PASS");fflush(stdout);
    const auto deadMarker=Read(marker);
    if(deadMarker.find("target_created=")==std::string::npos)return 25;
    first.Stop();guard.UseFixture(missing);
    if(!Wait([&]{return !guard.Active();},5000)||!guard.Requested()||!savedEnabled)return 26;
    if(!second.Start(directory))return 27;
    guard.UseFixture(second.pid);
    if(!Wait([&]{return guard.Active();},15000)||disabled)return 28;
    puts("target_process_restart_recovers=PASS");fflush(stdout);
    guard.Shutdown();if(!savedEnabled||disabled)return 29;
    {
        WinImeTrayGuard restarted;restarted.UseFixture(second.pid);restarted.onRequestedChanged=persist;
        if(!restarted.Initialize(directory,data)||!restarted.Enable(true)||!Wait([&]{return restarted.Active();},8000))return 30;
        restarted.Shutdown();if(!savedEnabled||disabled)return 31;
    }
    puts("application_restart_preserves_setting=PASS");fflush(stdout);
    Write(marker,deadMarker);
    std::string liveMarker;
    {
        WinImeTrayGuard stale;stale.UseFixture(second.pid);stale.onRequestedChanged=persist;
        if(!stale.Initialize(directory,data)||!stale.Enable(true)||!Wait([&]{return stale.Active();},8000))return 32;
        liveMarker=Read(marker);stale.Shutdown();
    }
    puts("dead_target_marker_recovered=PASS");fflush(stdout);
    Write(marker,liveMarker);
    {
        WinImeTrayGuard blocked;blocked.UseFixture(second.pid);blocked.onRequestedChanged=persist;
        if(!blocked.Initialize(directory,data)||blocked.Enable(true)||!blocked.Requested()||!savedEnabled)return 33;
        Wait([]{return false;},2500);
        if(blocked.Active()||disabled)return 34;
        blocked.Restore();if(blocked.Requested()||savedEnabled||disabled!=1)return 35;
    }
    puts("unconfirmed_live_target_keeps_preference_without_retry=PASS");fflush(stdout);
    Write(marker,"pid=1\n");
    std::filesystem::last_write_time(marker,std::filesystem::file_time_type::clock::now()-std::chrono::hours(1));
    {
        WinImeTrayGuard legacy;legacy.UseFixture(second.pid);
        if(!legacy.Initialize(directory,data)||!legacy.Enable(true)||!Wait([&]{return legacy.Active();},8000))return 36;
        SendMessageW(legacy.TestWindow(),RegisterWindowMessageW(L"TaskbarCreated"),0,0);
        legacy.Restore();Wait([]{return false;},5000);
        if(legacy.Requested()||legacy.Active())return 37;
    }
    puts("legacy_marker_and_manual_off=PASS");return 0;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 1;
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    if(std::wstring(argv[1])==L"--lifecycle")return Lifecycle(std::filesystem::path(exe).parent_path(),std::filesystem::absolute(argv[2]));
    WinImeTrayGuard guard;guard.UseFixture(static_cast<DWORD>(_wtoi(argv[1])));
    if(!guard.Initialize(std::filesystem::path(exe).parent_path(),std::filesystem::absolute(argv[2])))return 2;
    unsigned disabled{};guard.onRequestedChanged=[&](bool on){if(!on)++disabled;};
    if(!guard.Enable() || !Wait([&]{return guard.Active();},8000))return 3;
    SendMessageW(guard.TestWindow(),WM_DISPLAYCHANGE,0,0);
    if(!guard.Requested() || !Wait([&]{return !guard.Active();},1000) || !Wait([&]{return guard.Active();},8000) || disabled)return 4;
    puts("layout_auto_recovery=PASS");fflush(stdout);
    SendMessageW(guard.TestWindow(),WM_WTSSESSION_CHANGE,WTS_SESSION_LOCK,0);
    if(!Wait([&]{return !guard.Active();},1500))return 5;
    Wait([]{return false;},2500);
    if(guard.Active() || !guard.Requested())return 6;
    SendMessageW(guard.TestWindow(),WM_WTSSESSION_CHANGE,WTS_SESSION_UNLOCK,0);
    if(!Wait([&]{return guard.Active();},8000))return 7;
    puts("session_resume=PASS");fflush(stdout);
    SendMessageW(guard.TestWindow(),WM_DISPLAYCHANGE,0,0);
    guard.Restore();Wait([]{return false;},4000);
    if(guard.Requested() || guard.Active())return 8;
    puts("manual_off_cancels_recovery=PASS");
    guard.Shutdown();return 0;
}
