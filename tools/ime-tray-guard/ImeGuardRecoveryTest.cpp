// 只操作本测试的后台窗口和自建 XAML 宿主；不广播、不改变系统 DPI/会话状态。
#include "WinImeTrayGuard.h"
#include <wtsapi32.h>
#include <cstdio>
#include <functional>
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
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 1;
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    WinImeTrayGuard guard;guard.UseFixture(static_cast<DWORD>(_wtoi(argv[1])));
    if(!guard.Initialize(std::filesystem::path(exe).parent_path(),argv[2]))return 2;
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
