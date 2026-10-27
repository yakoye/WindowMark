// 仅供测试启动器看门狗；不进入共享包，不操作任务栏。
#include <windows.h>
#include <string>
int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"fixture-hang") {
            Sleep(60000);
            return 99;
        }
    }
    return 7;
}
