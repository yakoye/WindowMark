#include "AutoStart.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using windowmark::app::detail::AutoStartLocation;

[[noreturn]] void Fail(const char* message, int line) {
    std::cerr << "FAIL line " << line << ": " << message << '\n';
    std::exit(1);
}

#define CHECK(expr) do { if (!(expr)) Fail(#expr, __LINE__); } while (false)

class TestRegistryTree {
public:
    TestRegistryTree() {
        base_ = L"Software\\WindowMark\\Tests\\AutoStart-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64());
        run_ = base_ + L"\\Run";
        approved_ = base_ + L"\\StartupApproved\\Run";
    }

    ~TestRegistryTree() { RegDeleteTreeW(HKEY_CURRENT_USER, base_.c_str()); }

    [[nodiscard]] AutoStartLocation Location() const {
        return {run_.c_str(), approved_.c_str(), L"WindowMarkAutoStartTest"};
    }

    void DeleteApprovedValue() const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, approved_.c_str(), 0, KEY_SET_VALUE, &key) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(key, L"WindowMarkAutoStartTest");
            RegCloseKey(key);
        }
    }

    void WriteApproval(BYTE state, DWORD type = REG_BINARY) const {
        HKEY key = nullptr;
        CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, approved_.c_str(), 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS);
        BYTE bytes[12]{};
        bytes[0] = state;
        CHECK(RegSetValueExW(key, L"WindowMarkAutoStartTest", 0, type, bytes,
                             sizeof(bytes)) == ERROR_SUCCESS);
        RegCloseKey(key);
    }

    void WriteRunDword() const {
        HKEY key = nullptr;
        CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, run_.c_str(), 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS);
        DWORD value = 1;
        CHECK(RegSetValueExW(key, L"WindowMarkAutoStartTest", 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
              ERROR_SUCCESS);
        RegCloseKey(key);
    }

    [[nodiscard]] std::wstring ReadRunCommand() const {
        HKEY key = nullptr;
        CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, run_.c_str(), 0, KEY_QUERY_VALUE, &key) ==
              ERROR_SUCCESS);
        DWORD type = 0;
        DWORD bytes = 0;
        CHECK(RegQueryValueExW(key, L"WindowMarkAutoStartTest", nullptr, &type, nullptr,
                               &bytes) == ERROR_SUCCESS);
        CHECK(type == REG_SZ);
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        CHECK(RegQueryValueExW(key, L"WindowMarkAutoStartTest", nullptr, &type,
                               reinterpret_cast<BYTE*>(value.data()), &bytes) == ERROR_SUCCESS);
        RegCloseKey(key);
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        return value;
    }

    [[nodiscard]] BYTE ReadApproval() const {
        HKEY key = nullptr;
        CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, approved_.c_str(), 0, KEY_QUERY_VALUE, &key) ==
              ERROR_SUCCESS);
        BYTE bytes[12]{};
        DWORD size = sizeof(bytes);
        DWORD type = 0;
        CHECK(RegQueryValueExW(key, L"WindowMarkAutoStartTest", nullptr, &type, bytes,
                               &size) == ERROR_SUCCESS);
        RegCloseKey(key);
        CHECK(type == REG_BINARY);
        CHECK(size == sizeof(bytes));
        return bytes[0];
    }

private:
    std::wstring base_;
    std::wstring run_;
    std::wstring approved_;
};

} // namespace

int main() {
    TestRegistryTree registry;
    const AutoStartLocation location = registry.Location();
    constexpr wchar_t exe[] = L"C:\\Program Files\\WindowMark\\WindowMark.exe";

    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));
    CHECK(!windowmark::app::detail::SetAutoStart(location, L"", true));

    CHECK(windowmark::app::detail::SetAutoStart(location, exe, true));
    CHECK(registry.ReadRunCommand() ==
          L"\"C:\\Program Files\\WindowMark\\WindowMark.exe\" --autostart");
    CHECK(registry.ReadApproval() == 0x02);
    CHECK(windowmark::app::detail::IsAutoStartEnabled(location));

    registry.WriteApproval(0x03);
    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));

    CHECK(windowmark::app::detail::SetAutoStart(location, exe, true));
    CHECK(registry.ReadApproval() == 0x02);
    CHECK(windowmark::app::detail::IsAutoStartEnabled(location));

    registry.WriteApproval(0x07);
    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));
    registry.WriteApproval(0x06);
    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));
    registry.WriteApproval(0x02, REG_SZ);
    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));

    registry.DeleteApprovedValue();
    CHECK(windowmark::app::detail::IsAutoStartEnabled(location));

    registry.WriteRunDword();
    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));

    CHECK(windowmark::app::detail::SetAutoStart(location, exe, false));
    CHECK(!windowmark::app::detail::IsAutoStartEnabled(location));
    CHECK(windowmark::app::detail::SetAutoStart(location, exe, false));

    std::cout << "autostart registry tests passed\n";
    return 0;
}
