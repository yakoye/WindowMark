#include "EmbeddedTools.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace windowmark::bundle {
namespace {
struct Payload { int id; const wchar_t* name; const void* data{}; DWORD size{}; };
bool Matches(const std::filesystem::path& path, const Payload& payload) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != payload.size || ec) return false;
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes(payload.size);
    return file.read(bytes.data(), payload.size) &&
           std::memcmp(bytes.data(), payload.data, payload.size) == 0;
}
}
bool Prepare(std::filesystem::path& directory, std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    try {
        std::array<Payload, 5> payloads{{
            {301, L"ClipKeeper.exe"}, {302, L"WindowMarkDiag.exe"},
            {303, L"WindowMarkInspect.exe"}, {304, L"WindowMarkXamlContainerTrial.exe"},
            {305, L"WindowMarkXamlContainerTap.dll"}}};
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("hash");
        for (auto& payload : payloads) {
            const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(payload.id), RT_RCDATA);
            payload.size = resource ? SizeofResource(nullptr, resource) : 0;
            payload.data = resource ? LockResource(LoadResource(nullptr, resource)) : nullptr;
            if (!payload.size || !payload.data ||
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(payload.data)),
                               payload.size, 0) < 0) throw std::runtime_error("resource");
        }
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("digest");
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        std::wstring key;
        for (const auto value : digest) {
            key += L"0123456789abcdef"[value >> 4];
            key += L"0123456789abcdef"[value & 15];
        }
        wchar_t exe[32768]{};
        const auto length = GetModuleFileNameW(nullptr, exe, 32768);
        if (!length || length >= 32768) throw std::runtime_error("path");
        const auto root = std::filesystem::path(exe).parent_path() / L".runtime";
        directory = root / key;
        std::filesystem::create_directories(directory);
        SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_HIDDEN);
        for (const auto& payload : payloads) {
            const auto destination = directory / payload.name;
            if (Matches(destination, payload)) continue;
            const auto temporary = destination.wstring() + L"." +
                std::to_wstring(GetCurrentProcessId()) + L".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char*>(payload.data), payload.size);
                out.close();
                if (!out) throw std::runtime_error("write");
            }
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temporary.c_str());
                if (!Matches(destination, payload)) throw std::runtime_error("replace");
            }
            if (!Matches(destination, payload)) throw std::runtime_error("verify");
        }
        return true;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"无法准备内置组件。请将 WindowMark.exe 放在可写目录；如果组件正在使用，请退出后重试。";
        return false;
    }
}
}
