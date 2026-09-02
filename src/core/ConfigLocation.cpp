#include "windowmark/core/ConfigLocation.h"

namespace windowmark {

ConfigLocation ResolveConfigLocation(const ConfigLocationInputs& inputs) {
    if (inputs.portableExists && !inputs.portable.empty()) {
        return ConfigLocation{inputs.portable, ConfigSource::Portable, false};
    }
    if (inputs.configuredUsable && !inputs.configured.empty()) {
        return ConfigLocation{inputs.configured, ConfigSource::Configured, false};
    }
    // 走到这里而注册表里又确实有值，说明那个位置没法用了，得让调用方去提示。
    const bool lost = !inputs.configured.empty();
    return ConfigLocation{inputs.fallback, ConfigSource::Fallback, lost};
}

} // namespace windowmark
