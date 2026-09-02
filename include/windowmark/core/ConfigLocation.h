#pragma once

#include <filesystem>

namespace windowmark {

// settings.conf 最终用的是哪一份。
enum class ConfigSource {
    Portable,    // exe 同目录，跟着程序走
    Configured,  // 注册表 ConfigPath 指定，跟着机器走
    Fallback,    // %LOCALAPPDATA%\WindowMark
};

// 三个候选位置，外加各自是否可用。存在性和可写性由平台层判断后填进来，这里只做选择，
// 所以这段优先级逻辑可以脱离文件系统被测试。
struct ConfigLocationInputs {
    std::filesystem::path portable;
    bool portableExists{false};
    std::filesystem::path configured;
    bool configuredUsable{false};
    std::filesystem::path fallback;
};

struct ConfigLocation {
    std::filesystem::path path;
    ConfigSource source{ConfigSource::Fallback};
    // 注册表里明明指定了位置，但那个位置已经用不了（U 盘拔了、目录被删、变成只读），
    // 于是回落到了默认位置。调用方必须把这件事告诉用户：静默回落会让人以为自己的
    // 配置还在原处，直到发现改动全丢了才反应过来。
    bool configuredUnavailable{false};
};

// 便携排在显式指定之前，是刻意的：注册表跟着机器走，exe 旁边的 conf 跟着程序走。
// 代价是可能反直觉——用户指定了路径却因为 exe 旁边有个 conf 而没生效，所以设置界面
// 必须显示当前实际生效的那一个。
[[nodiscard]] ConfigLocation ResolveConfigLocation(const ConfigLocationInputs& inputs);

} // namespace windowmark
