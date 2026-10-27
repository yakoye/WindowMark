#include "ImeGuardClient.h"
#include <objbase.h>
namespace ime_guard {
void Client::Release() noexcept {
    if(shared_){UnmapViewOfFile(shared_);shared_=nullptr;}
    if(mapping_){CloseHandle(mapping_);mapping_=nullptr;}
    if(process_){CloseHandle(process_);process_=nullptr;}
}
Client::~Client() {
    Stop();
    if(process_)WaitForSingleObject(process_,6000);
    // 不结束Explorer。控制程序若仍阻塞，目标端期限/进程死亡监测负责恢复。
    Release();
}
bool Client::Start(std::filesystem::path const& directory,std::filesystem::path const& log,DWORD fixturePid) {
    if(status_.running)return false;
    Release();status_={};
    try {
        const auto exe=directory/L"WindowMarkXamlContainerTrial.exe";
        if(!std::filesystem::is_regular_file(exe)||!std::filesystem::is_regular_file(directory/L"WindowMarkXamlContainerTap.dll")) {
            status_.error=L"缺少控制程序或原生组件，请使用完整文件夹。";return false;
        }
        GUID guid{};wchar_t id[40]{};
        if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,id,40)){status_.error=L"无法创建独立会话。";return false;}
        const auto name=L"Local\\WindowMark.ImeGuard.Client."+std::wstring(id);
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(ClientBlock),name.c_str());
        if(!mapping_)throw std::runtime_error("mapping");
        shared_=static_cast<ClientBlock*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(ClientBlock)));
        if(!shared_)throw std::runtime_error("view");
        new(shared_) ClientBlock{};
        shared_->showHints=showHints_?1:0;
        shared_->ownerPid=GetCurrentProcessId();InterlockedExchange64(&shared_->heartbeat,static_cast<LONG64>(GetTickCount64()));
        std::filesystem::create_directories(log.parent_path());
        std::wstring command=L"\""+exe.wstring()+L"\" ";
        command+=fixturePid?L"--fixture-pid "+std::to_wstring(fixturePid)+L" --reserve ":L"--reserve-taskbar --native-research ";
        command+=L"--persistent --client-map \""+name+L"\" --out \""+log.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION info{};
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&info)) {
            status_.error=L"无法启动控制程序，错误码："+std::to_wstring(GetLastError());Release();return false;
        }
        CloseHandle(info.hThread);process_=info.hProcess;status_.running=true;return true;
    } catch(...) {status_.error=L"建立控制会话失败；没有自动重试附加。";Release();return false;}
}
void Client::Stop() noexcept {if(shared_){status_.stopping=true;InterlockedExchange(&shared_->stop,1);}}
void Client::Tick(bool heartbeat) noexcept {
    if(!process_||!shared_)return;
    if(heartbeat&&!shared_->stop)InterlockedExchange64(&shared_->heartbeat,static_cast<LONG64>(GetTickCount64()));
    status_.state=shared_->state;status_.writes=shared_->writes;status_.restores=shared_->restores;
    status_.restoreFailures=shared_->restoreFailures;status_.targetPid=static_cast<DWORD>(shared_->targetPid);
    status_.width=static_cast<double>(InterlockedCompareExchange64(&shared_->widthMilliDip,0,0))/1000;
    status_.targetHeartbeat=static_cast<ULONGLONG>(InterlockedCompareExchange64(&shared_->targetHeartbeat,0,0));
    if(WaitForSingleObject(process_,0)==WAIT_OBJECT_0) {
        GetExitCodeProcess(process_,&status_.exitCode);status_.running=false;
        if(status_.exitCode)status_.error=L"控制程序已退出（"+std::to_wstring(status_.exitCode)+L"），未自动重新附加；请查看诊断日志。";
        Release();
    }
}
}
