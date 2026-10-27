// 自身隐藏宿主：不操作用户窗口、不抢焦点。用于验证同一诊断DLL的宽度和恢复。
#include <windows.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <chrono>
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
int RunIsland(unsigned index,unsigned seconds,bool churn,bool closeSecondary,bool alwaysVisible,std::atomic_bool& mainReady) {
    const char* stage="initialize";
    try {
        // XAML进程级初始化先由主UI线程完成；并发初始化的E_UNEXPECTED不是目标崩溃。
        while(index && !mainReady.load())std::this_thread::sleep_for(std::chrono::milliseconds(10));
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        auto manager=Hosting::WindowsXamlManager::InitializeForCurrentThread();
        stage="create-window";
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"WindowMark fixture",WS_POPUP,-32000,-32000,400,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window)return 1;
        Hosting::DesktopWindowXamlSource source;
        stage="attach-island";
        winrt::check_hresult(source.as<IDesktopWindowXamlSourceNative>()->AttachToWindow(window));
        HWND child{};winrt::check_hresult(source.as<IDesktopWindowXamlSourceNative>()->get_WindowHandle(&child));
        SetWindowPos(child,nullptr,0,0,400,100,SWP_NOACTIVATE|SWP_NOZORDER|SWP_SHOWWINDOW);
        StackPanel root;root.Orientation(Orientation::Horizontal);
        Grid slot;slot.Name(index==0?L"WindowMarkImeSlots":L"WindowMarkOtherSlots");slot.HorizontalAlignment(HorizontalAlignment::Left);
        StackPanel content;content.Orientation(Orientation::Horizontal);
        Border a,b;a.Width(32);a.Height(48);b.Width(44);b.Height(48);
        a.Name(L"WindowMarkFixtureIcon");b.Name(L"WindowMarkFixtureIcon");
        content.Children().Append(a);content.Children().Append(b);
        slot.Children().Append(content);root.Children().Append(slot);source.Content(root);
        ShowWindow(window,SW_SHOWNOACTIVATE); // 屏幕外、不激活；让XAML内容真正进入Loaded树。
        if(!index)mainReady=true;
        stage="pump-layout";
        const auto start=GetTickCount64();unsigned lastSecond=999;
        bool closed=false;
        while(GetTickCount64()-start<seconds*1000ULL) {
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            auto sec=static_cast<unsigned>((GetTickCount64()-start)/1000);
            if(closeSecondary && index && sec>=7 && !closed) {
                source.Close();closed=true;
                printf("{\"island\":%u,\"event\":\"island_closed_thread_alive\",\"second\":%u}\n",index,sec);fflush(stdout);
            }
            // 5..15秒内容消失，让控制器正常退出/被终止时都能观察恢复。
            content.Visibility(!alwaysVisible&&sec>=5&&sec<15?Visibility::Collapsed:Visibility::Visible);
            if(churn && sec!=lastSecond) {
                // 自身UI线程创建/销毁非目标节点，模拟Explorer中的其它动态XAML树。
                while(root.Children().Size()>1)root.Children().RemoveAtEnd();
                for(unsigned n=0;n<sec%7;++n){Border transient;transient.Width(1);root.Children().Append(transient);}
            }
            root.Measure({400,100});root.Arrange({0,0,400,100});root.UpdateLayout();
            if(sec!=lastSecond) {
                TextBlock hint{nullptr};
                for(auto const& hintChild:slot.Children())if(auto label=hintChild.try_as<TextBlock>();label && label.Name()==L"WindowMarkImeHintLabel")hint=label;
                printf("{\"island\":%u,\"thread\":%lu,\"second\":%u,\"width\":%.3f,\"min_width\":%.3f,\"local_unset\":%s,\"content_visible\":%s,\"hint_present\":%s,\"hint_visible\":%s,\"hint_hit_test\":%s,\"hint_chars\":%u,\"hint_width\":%.3f}\n",index,GetCurrentThreadId(),sec,slot.ActualWidth(),slot.MinWidth(),
                    slot.ReadLocalValue(FrameworkElement::MinWidthProperty())==DependencyProperty::UnsetValue()?"true":"false",content.Visibility()==Visibility::Visible?"true":"false",
                    hint?"true":"false",hint&&hint.Visibility()==Visibility::Visible?"true":"false",hint&&hint.IsHitTestVisible()?"true":"false",hint?hint.Text().size():0,hint?hint.ActualWidth():0);
                fflush(stdout);lastSecond=sec;
            }
            MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT);
        }
        stage="close-island";if(!closed)source.Close();DestroyWindow(window);manager.Close();return 0;
    } catch(winrt::hresult_error const& e) {if(!index)mainReady=true;fprintf(stderr,"Fixture island=%u thread=%lu stage=%s HRESULT %08lx\n",index,GetCurrentThreadId(),stage,static_cast<unsigned long>(e.code().value));return 2;}
}
int wmain(int argc,wchar_t** argv) {
    unsigned threads=1,seconds=20;bool churn=false,retireSecondary=false,closeSecondary=false,alwaysVisible=false;
    try {
        for(int i=1;i<argc;++i){
            std::wstring arg=argv[i];
            if(arg==L"--churn")churn=true;
            else if(arg==L"--retire-secondary")retireSecondary=true;
            else if(arg==L"--close-secondary")closeSecondary=true;
            else if(arg==L"--always-visible")alwaysVisible=true;
            else if((arg==L"--ui-threads"||arg==L"--seconds")&&i+1<argc){
                std::wstring value=argv[++i];if(value.empty()||value.find_first_not_of(L"0123456789")!=value.npos)return 1;
                unsigned n=static_cast<unsigned>(std::stoul(value));
                if(arg==L"--ui-threads"){if(n<1||n>4)return 1;threads=n;}
                else {if(n<5||n>90)return 1;seconds=n;}
            }else return 1;
        }
    }catch(...){return 1;}
    std::atomic_int failed{};std::atomic_bool mainReady{};std::vector<std::thread> workers;
    for(unsigned i=1;i<threads;++i)workers.emplace_back([&,i]{if(RunIsland(i,retireSecondary?3:seconds,churn,closeSecondary,alwaysVisible,mainReady))++failed;});
    if(RunIsland(0,seconds,churn,closeSecondary,alwaysVisible,mainReady))++failed;
    for(auto& thread:workers)thread.join();
    return failed?2:0;
}
