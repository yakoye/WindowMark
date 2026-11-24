#include "WinMouseControl.h"
#include "windowmark/core/WheelThrottle.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <future>
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed line " << __LINE__ << ": " #x << '\n'; std::exit(1); } } while(false)
namespace windowmark::win {
struct MouseControlTestAccess {
    static void BlockedWorker(WinMouseControl& control) {
        auto release = std::make_shared<std::promise<void>>();
        auto resume = release->get_future(); std::promise<void> finished; auto done = finished.get_future();
        control.wheel_ = std::make_shared<MouseWheelState>();
        auto state = control.wheel_; state->active = true;
        control.worker_ = std::thread([state, resume = std::move(resume), finished = std::move(finished)]() mutable {
            resume.wait(); state->active = false; finished.set_value();
        });
        control.threadId_ = GetThreadId(static_cast<HANDLE>(control.worker_.native_handle()));
        const auto start = GetTickCount64(); control.StopWheel();
        CHECK(GetTickCount64() - start < 2200); CHECK(!control.WheelActive());
        release->set_value(); CHECK(done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    }
};
}
int main() {
    using namespace windowmark;
    WheelThrottle gate;
    CHECK(gate.Route(false,120,0,50,false)==WheelAction::Pass);
    CHECK(gate.Route(false,120,1,50,false)==WheelAction::Drop);
    CHECK(gate.Route(false,120,2,50,true)==WheelAction::Pass);
    CHECK(gate.Route(false,-120,2,50,false)==WheelAction::Pass);
    CHECK(gate.Route(true,960,2,50,false)==WheelAction::NormalizeLater);
    CHECK(gate.Route(true,-960,2,50,false)==WheelAction::NormalizeLater);
    CHECK(gate.Route(false,0,3,50,false)==WheelAction::Pass);
    gate.Reset();
    CHECK(gate.Accept(false,120,0,50));
    for (unsigned i=1;i<50;++i) CHECK(!gate.Accept(false,120,i,50));
    CHECK(gate.Accept(false,120,50,50));
    CHECK(gate.Accept(false,-120,50,50)); // 换向立即响应
    CHECK(!gate.Accept(false,-120,99,50));
    CHECK(gate.Accept(false,-120,100,50));
    CHECK(gate.Accept(true,120,50,50)); // 水平独立
    CHECK(gate.Accept(true,-120,50,50));
    CHECK(!gate.Accept(true,0,100,50));
    gate.Reset();
    unsigned accepted=0;
    for (unsigned i=0;i<1000;++i) accepted += gate.Accept(false,120,i,50) ? 1U : 0U;
    CHECK(accepted==20); // 1000 Hz 连续滚动不被完全吞掉
    gate.Reset(); CHECK(gate.Accept(false,960,100,100)); CHECK(!gate.Accept(false,1,199,100));
    CHECK(gate.Accept(false,1,200,100));
    gate.Reset(); CHECK(gate.Accept(false,120,0,80)); CHECK(!gate.Accept(false,120,79,80));
    CHECK(gate.Accept(false,120,80,80));
    const auto dir=std::filesystem::temp_directory_path()/ (L"WindowMark-mouse-test-"+std::to_wstring(GetCurrentProcessId()));
    CHECK(std::filesystem::create_directory(dir));
    const auto config=dir/L"settings.conf";
    Settings fresh;
    CHECK(!fresh.mouse.pointerEnabled && !fresh.mouse.wheelEnabled);
    fresh.mouse.wheelEnabled=true; fresh.mouse.pointerEnabled=false;
    fresh.mouse.wheelIntervalMs=80; fresh.mouse.pointerSpeed=4;
    CHECK(Settings::Save(config,fresh)); auto read=Settings::LoadOrCreate(config);
    CHECK(read.mouse.wheelEnabled && !read.mouse.pointerEnabled && read.mouse.wheelIntervalMs==80 && read.mouse.pointerSpeed==4);
    fresh.mouse.wheelEnabled=false; fresh.mouse.pointerEnabled=true;
    CHECK(Settings::Save(config,fresh)); read=Settings::LoadOrCreate(config);
    CHECK(!read.mouse.wheelEnabled && read.mouse.pointerEnabled);
    { std::ofstream out(config); out<<"mouse.pointer_speed=999\nmouse.wheel_interval_ms=-3\n"; }
    read=Settings::LoadOrCreate(config); CHECK(read.mouse.pointerSpeed==20 && read.mouse.wheelIntervalMs==1);
    { std::ofstream out(config); out<<"mouse.pointer_speed=bad\nmouse.wheel_interval_ms=bad\nmouse.wheel_enabled=bad\n"; }
    read=Settings::LoadOrCreate(config); CHECK(read.mouse.pointerSpeed==10 && read.mouse.wheelIntervalMs==50 && !read.mouse.wheelEnabled);
    // 原生接口验收：仅设置为当前速度，不移动鼠标或发送滚轮输入。
    int current{}; CHECK(SystemParametersInfoW(SPI_GETMOUSESPEED,0,&current,0));
    {
        win::WinMouseControl control(dir); std::wstring error;
        CHECK(control.Recover(error)); MouseSettings value;
        value.pointerEnabled=true; value.pointerSpeed=current;
        CHECK(control.Apply(value,error)); CHECK(std::filesystem::exists(dir/L"mouse-speed.restore"));
        value.pointerEnabled=false; CHECK(control.Apply(value,error));
        CHECK(!std::filesystem::exists(dir/L"mouse-speed.restore"));
        value.wheelEnabled=true; CHECK(control.Apply(value,error));
        CHECK(control.WheelActive()); CHECK(!control.PointerActive());
        value.wheelEnabled=false; CHECK(control.Apply(value,error));
        CHECK(!control.WheelActive());
    }
    int after{}; CHECK(SystemParametersInfoW(SPI_GETMOUSESPEED,0,&after,0)); CHECK(after==current);
    { std::ofstream out(dir/L"mouse-speed.restore"); out<<current<<' '<<current; }
    { win::WinMouseControl control(dir); std::wstring error; CHECK(control.Recover(error)); }
    CHECK(!std::filesystem::exists(dir/L"mouse-speed.restore"));
    { std::ofstream out(dir/L"mouse-speed.restore"); out<<current<<' '<<(current==20 ? 1 : 20); }
    { win::WinMouseControl control(dir); std::wstring error; CHECK(control.Recover(error)); }
    CHECK(SystemParametersInfoW(SPI_GETMOUSESPEED,0,&after,0)); CHECK(after==current);
    { std::ofstream out(dir/L"mouse-speed.restore"); out<<"invalid"; }
    { win::WinMouseControl control(dir); std::wstring error; CHECK(!control.Recover(error)); CHECK(!error.empty()); }
    CHECK(SystemParametersInfoW(SPI_GETMOUSESPEED,0,&after,0)); CHECK(after==current);
    std::filesystem::remove(dir/L"mouse-speed.restore");
    { win::WinMouseControl control(dir); win::MouseControlTestAccess::BlockedWorker(control); }
    std::filesystem::remove(config); std::filesystem::remove(dir);
    std::cout<<"鼠标节流、独立设置、系统接口与恢复检查通过\n";
}
