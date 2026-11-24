#pragma once
// 限定映像的局部根访问：精确匹配当前加载映像后，只查询当前 XAML 线程的 Island。
// 不订阅视觉树、不调用 GetRuntimeElement、不修改系统指令。未知映像直接拒绝。
#include <windows.h>
#include <wrl/client.h>
#include <inspectable.h>
#include <vector>
#include <cstring>
namespace local_roots {
using Roots = std::vector<Microsoft::WRL::ComPtr<IInspectable>>;
inline HRESULT Read(Roots& roots) {
    auto module=GetModuleHandleW(L"Windows.UI.Xaml.dll");
    if(!module)return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    auto base=reinterpret_cast<const unsigned char*>(module);
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return E_UNEXPECTED;
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)
        return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if(directory.VirtualAddress>=nt->OptionalHeader.SizeOfImage ||
       directory.Size>nt->OptionalHeader.SizeOfImage-directory.VirtualAddress)return E_UNEXPECTED;
    auto entries=reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base+directory.VirtualAddress);
    struct Adapter { DWORD imageSize,rva;unsigned char signature[24]; };
    // 两个入口均由对应 GUID 的微软 PDB 确认，不能仅凭文件版本或系统补丁号选择。
    constexpr Adapter adapters[]={
        {0x1121000,0x86c06c,{0x52,0x53,0x44,0x53,0xed,0x88,0x5a,0xe1,0x99,0x95,0x47,0xb3,0xa0,0xd2,0x57,0x6c,0x13,0x28,0x7f,0xc4,0x01,0,0,0}},
        {0x111f000,0x86bc0c,{0x52,0x53,0x44,0x53,0xca,0x88,0x7e,0xac,0x92,0x1f,0xf3,0x2e,0x59,0x84,0x79,0x8a,0x5c,0x8c,0xa4,0x6c,0x01,0,0,0}}
    };
    const Adapter* matched=nullptr;
    for(unsigned i=0;i<directory.Size/sizeof(IMAGE_DEBUG_DIRECTORY);++i) {
        const auto& e=entries[i];
        for(const auto& adapter:adapters) {
            if(nt->OptionalHeader.SizeOfImage==adapter.imageSize && e.Type==IMAGE_DEBUG_TYPE_CODEVIEW &&
               e.SizeOfData>=sizeof(adapter.signature) && e.AddressOfRawData<adapter.imageSize-sizeof(adapter.signature) &&
               !memcmp(base+e.AddressOfRawData,adapter.signature,sizeof(adapter.signature)))matched=&adapter;
        }
    }
    // RVA 来自上述 GUID 对应的微软 PDB；同时核验函数入口，拒绝不一致的加载映像。
    constexpr unsigned char prefix[]={0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x56,0x41,0x57,
        0x48,0x8b,0xec,0x48,0x83,0xec,0x30,0x4c,0x8b,0xf1};
    if(!matched || memcmp(base+matched->rva,prefix,sizeof(prefix)))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    // 预分配避免常见岛数量下跨模块重新分配；私有 ABI 只对上方精确匹配的映像开放。
    roots.reserve(1024);
    using Function=HRESULT(__fastcall*)(Roots*);
    return reinterpret_cast<Function>(const_cast<unsigned char*>(base+matched->rva))(&roots);
}
}
