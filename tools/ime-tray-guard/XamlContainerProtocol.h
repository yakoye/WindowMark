#pragma once
#include <windows.h>
#include <cstdint>
#include "ImeGuardWidthHint.h"
namespace container_trial {
constexpr GUID kClassId{0x9e8dfe47,0x8d29,0x41ba,{0xa3,0x14,0x89,0xd9,0xf9,0xce,0x11,0x8b}};
constexpr unsigned kMagic=0x574d5843, kVersion=4, kCapacity=256;
constexpr wchar_t kDispatchClass[]=L"WindowMark.XamlContainerTrial.LocalRoots07";
inline UINT ProtocolQuery() { static const UINT value=RegisterWindowMessageW(L"WindowMark.ImeGuard.ProtocolVersion");return value; }
enum State : LONG { Waiting, Attached, Observing, Reserved, Restored, Closed, Failed };
struct Node {
    uint64_t handle{}, parent{};
    wchar_t type[100]{}, name[64]{};
    double width{}, height{}, minWidth{}, x{}, y{};
    int visible{}, hitTest{}, hasBinding{}, localKind{}; // 0 unset, 1 value, 2 binding
    HRESULT hr{};
};
struct Block {
    unsigned magic{kMagic}, version{kVersion};
    DWORD ownerPid{}, targetPid{}, durationSeconds{45};
    LONG reserve{}, fixture{};
    volatile LONG stop{}, sequence{}, state{}, count{}, overflow{}, errors{};
    volatile LONG writes{}, restores{}, restoreFailures{};
    HRESULT attachHr{}, subscribeHr{}, unsubscribeHr{};
    DWORD uiThread{};
    uint64_t tick{}, targetHandle{};
    double reservedWidth{};
    Node nodes[kCapacity]{};
    LONG persistent{}, nativeConsent{};
    volatile LONG subscriptionActive{};
    volatile LONG showHints{}, hintVisible{};
    double hintLeft{}, hintWidth{};
    ime_guard::WidthHint widthHint{}; // v4 尾部扩展，v3 字段布局保持不变。
};
}
