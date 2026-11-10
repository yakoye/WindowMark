#pragma once
#include <filesystem>
#include <string>

namespace windowmark::bundle {
// 成功后目录中的每个文件均与当前 EXE 内嵌资源逐字节一致。
bool Prepare(std::filesystem::path& directory, std::wstring& error);
}
