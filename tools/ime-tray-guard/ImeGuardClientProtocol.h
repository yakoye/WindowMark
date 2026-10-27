#pragma once
#include <windows.h>
#include <cstdint>
namespace ime_guard {
constexpr unsigned kClientMagic=0x574d4947, kClientVersion=3;
// 界面只写心跳/停止；控制程序只写运行状态。没有用户输入内容。
struct alignas(8) ClientBlock {
    unsigned magic{kClientMagic}, version{kClientVersion};
    DWORD ownerPid{};
    volatile LONG stop{};
    alignas(8) volatile LONG64 heartbeat{};
    volatile LONG state{-1}, writes{}, restores{}, restoreFailures{}, targetPid{};
    alignas(8) volatile LONG64 widthMilliDip{};
    alignas(8) volatile LONG64 targetHeartbeat{};
    volatile LONG result{STILL_ACTIVE};
    volatile LONG showHints{1};
};
}
