#pragma once

namespace windowmark {
// 允许尝试的任务栏平台，不代表已验收。实际调用仍须通过加载映像身份校验。
constexpr bool ImeGuardPlatformEligible(unsigned build) noexcept {
    return build >= 22000;
}
constexpr bool ImeGuardActuallyActive(bool running, bool stopping, long state,
                                     long restoreFailures) noexcept {
    return running && !stopping && state == 3 && restoreFailures == 0;
}
constexpr bool ImeGuardRestored(bool running, unsigned exitCode, long state,
                               long writes, long restores, long failures) noexcept {
    return !running && exitCode == 0 && state == 5 && writes == restores && failures == 0;
}
}
