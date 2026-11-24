// 可见但不输入文字的自有本地输入窗口，用于让原生输入法区域首次显现。15 秒自动关闭。
#include <windows.h>
namespace {
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CREATE) {
        const auto edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE,
            12,12,420,80,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetFocus(edit);SetTimer(window,1,15000,nullptr);return 0;
    }
    if(message==WM_SETFOCUS){SetFocus(GetWindow(window,GW_CHILD));return 0;}
    if(message==WM_TIMER || message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=Proc;cls.lpszClassName=L"WindowMark.InputContextFixture";
    cls.hCursor=LoadCursorW(nullptr,IDC_IBEAM);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if(!RegisterClassW(&cls))return 1;
    auto window=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"WindowMark 本地输入测试（15秒自动关闭，不输入文字）",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,80,80,470,145,nullptr,nullptr,instance,nullptr);
    if(!window)return 2;
    ShowWindow(window,SW_SHOW);SetForegroundWindow(window);SetFocus(GetWindow(window,GW_CHILD));
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return 0;
}
