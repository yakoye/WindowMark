# Writes a build stamp header. Run as a pre-build step on every build, not at configure
# time, so the stamp is the moment the binary was actually produced.
#
# Only rewrites the file when the contents change - an unconditional write would make
# everything that includes it recompile on every build even when nothing moved.

string(TIMESTAMP stamp "%Y-%m-%d %H:%M:%S")
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY "${SOURCE}"
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE git_result)
if(NOT git_result EQUAL 0)
    message(FATAL_ERROR "Cannot read Git revision for build identity")
endif()
string(SUBSTRING "${commit}" 0 7 commit)
set(content "#pragma once\n#include <string>\n#include \"AppIdentity.h\"\nnamespace windowmark::app {\ninline constexpr wchar_t kBuildStamp[] = L\"${stamp}\";\ninline constexpr wchar_t kGitCommit[] = L\"${commit}\";\ninline std::wstring BuildIdentity() { return std::wstring(kProductVersion) + L\" · \" + kGitCommit + L\" · \" + kBuildStamp; }\n}\n")

set(existing "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" existing)
endif()
if(NOT existing STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
