#include "ImeGuardClient.h"
#include <cstdio>
#include <string>
// 仅能对名称经过控制程序核验的自身夹具运行。无原生任务栏入口。
int wmain(int argc,wchar_t** argv) {
    if(argc!=5&&argc!=6){puts("fixture PID, fresh log path, seconds 10..120, heartbeat-stop second (0=normal), optional --toggle-hints");return 1;}
    const bool toggleHints=argc==6 && std::wstring(argv[5])==L"--toggle-hints";
    if(argc==6&&!toggleHints)return 1;
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
    const auto pid=static_cast<DWORD>(_wtoi(argv[1]));const int seconds=_wtoi(argv[3]),stopBeat=_wtoi(argv[4]);
    if(!pid||seconds<10||seconds>120||stopBeat<0||stopBeat>=seconds)return 1;
    ime_guard::Client client;
    if(!client.Start(std::filesystem::path(module).parent_path(),std::filesystem::absolute(argv[2]),pid))return 2;
    const auto begin=GetTickCount64();
    LONG previous=-2;
    while(client.Current().running) {
        const auto elapsed=GetTickCount64()-begin;
        if(toggleHints)client.SetHints(elapsed<7000 || elapsed>=10000);
        if(elapsed>=static_cast<ULONGLONG>(seconds)*1000)client.Stop();
        client.Tick(!stopBeat||elapsed<static_cast<ULONGLONG>(stopBeat)*1000);
        const auto& state=client.Current();
        if(previous!=state.state||!state.running) {
            printf("{\"elapsed_ms\":%llu,\"state\":%ld,\"running\":%s,\"writes\":%ld,\"restores\":%ld,\"restore_failures\":%ld,\"exit\":%lu}\n",elapsed,state.state,state.running?"true":"false",state.writes,state.restores,state.restoreFailures,state.exitCode);
            fflush(stdout);previous=state.state;
        }
        if(elapsed>static_cast<ULONGLONG>(seconds+12)*1000)return 3;
        Sleep(100);
    }
    const auto& final=client.Current();
    return final.exitCode==0&&final.writes>0&&final.writes==final.restores&&!final.restoreFailures?0:4;
}
