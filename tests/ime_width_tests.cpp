#include "ImeGuardWidthHint.h"
#include "ImeGuardClientProtocol.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"Failed: %d\n",__LINE__);std::exit(1);}}while(false)
int main() {
    static_assert(offsetof(ime_guard::ClientBlock,widthHint)==72);
    ime_guard::WidthHint value;
    CHECK(!ime_guard::Valid(value));
    const auto window=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,-32000,-32000,400,40,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);value.milliDip=76000;value.dpi=GetDpiForWindow(window);CHECK(GetWindowRect(window,&value.taskbar));
    CHECK(ime_guard::Valid(value));CHECK(ime_guard::Matches(value,window));
    auto bad=value;bad.dpi++;CHECK(!ime_guard::Matches(bad,window));
    bad=value;bad.taskbar.left++;CHECK(!ime_guard::Matches(bad,window));
    bad=value;bad.milliDip=-1;CHECK(!ime_guard::Valid(bad));
    bad=value;bad.milliDip=513000;CHECK(!ime_guard::Valid(bad));
    bad=value;bad.taskbar.right=bad.taskbar.left;CHECK(!ime_guard::Valid(bad));
    DestroyWindow(window);CHECK(!ime_guard::Matches(value,window));
    std::puts("Width cache context checks passed");
}
