#pragma once
#include "ImeGuardClientProtocol.h"
#include <filesystem>
#include <string>
namespace ime_guard {
struct Status {
    bool running{}, stopping{};
    LONG state{-1}, writes{}, restores{}, restoreFailures{};
    DWORD targetPid{}, exitCode{};
    double width{};
    ULONGLONG targetHeartbeat{};
    std::wstring error;
};
// 心跳必须由界面线程调用；不要放到一直活着但无法响应用户操作的后台定时线程。
class Client {
    HANDLE mapping_{},process_{};
    ClientBlock* shared_{};
    Status status_;
    bool showHints_{true};
    void Release() noexcept;
public:
    Client()=default;
    Client(Client const&)=delete;
    Client& operator=(Client const&)=delete;
    ~Client();
    bool Start(std::filesystem::path const& directory,std::filesystem::path const& log,DWORD fixturePid=0);
    void Stop() noexcept;
    void SetHints(bool enabled) noexcept { showHints_=enabled; if(shared_)InterlockedExchange(&shared_->showHints,enabled?1:0); }
    void Tick(bool heartbeat=true) noexcept;
    [[nodiscard]] Status const& Current() const noexcept {return status_;}
};
}
