// 只读预研工具：只遍历当前会话任务栏，不注入、不改变布局、不读取输入文字。
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wtsapi32.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
struct EvidenceFile {
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit EvidenceFile(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~EvidenceFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    EvidenceFile(const EvidenceFile&) = delete;
    EvidenceFile& operator=(const EvidenceFile&) = delete;
    bool Write(const std::string& value) {
        DWORD written{};
        return WriteFile(handle, value.data(), static_cast<DWORD>(value.size()), &written, nullptr)
            && written == value.size() && FlushFileBuffers(handle);
    }
};
std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}
std::string Json(const std::wstring& s) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : Utf8(s)) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) { const char* hex = "0123456789abcdef"; out << "\\u00" << hex[c >> 4] << hex[c & 15]; }
        else out << c;
    }
    out << '"';
    return out.str();
}
std::string RectJson(const RECT& r) {
    return "[" + std::to_string(r.left) + "," + std::to_string(r.top) + "," +
           std::to_string(r.right) + "," + std::to_string(r.bottom) + "]";
}
std::wstring Class(HWND hwnd) {
    wchar_t buf[256]{};
    GetClassNameW(hwnd, buf, 256);
    return buf;
}
std::uint64_t Handle(HWND hwnd) { return reinterpret_cast<std::uintptr_t>(hwnd); }
std::wstring ProcessName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buf[32768]{};
    DWORD len = 32768;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buf, &len);
    CloseHandle(process);
    return ok ? std::filesystem::path(buf).filename().wstring() : std::wstring{};
}
std::wstring RegText(const wchar_t* name) {
    wchar_t buf[256]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                    RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}
std::wstring FileVersion(const std::wstring& path) {
    DWORD unused{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0) return {};
    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length{};
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionMS)) +
           L"." + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionLS));
}
bool Contains(const std::wstring& s, const wchar_t* needle) { return s.find(needle) != std::wstring::npos; }
bool ImeCandidate(const std::wstring& s) {
    return Contains(s, L"Language") || Contains(s, L"InputIndicator") || Contains(s, L"IMEMode") ||
           Contains(s, L"ImeMode") || Contains(s, L"InputMethod");
}
// 仅输出固定的分类值，绝不保存其它托盘按钮名称（可能含 SSID、文件名等）。
std::wstring ImeHint(const std::wstring& name) {
    for (const wchar_t* token : {L"微软拼音", L"Microsoft Pinyin", L"中文模式", L"英文模式",
                               L"Chinese mode", L"English mode", L"输入指示器", L"Input Indicator"}) {
        if (Contains(name, token)) return token;
    }
    return {};
}
std::string Stamp() {
    SYSTEMTIME t{}; GetSystemTime(&t);
    char b[40]{};
    sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay,
              t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}
BOOL CALLBACK Monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto& out = *reinterpret_cast<std::vector<std::string>*>(data);
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    UINT x{}, y{};
    const HRESULT hr = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y);
    out.push_back("{\"device\":" + Json(info.szDevice) + ",\"bounds\":" + RectJson(info.rcMonitor) +
                  ",\"work_area\":" + RectJson(info.rcWork) + ",\"primary\":" +
                  std::to_string((info.dwFlags & MONITORINFOF_PRIMARY) != 0) + ",\"dpi\":[" +
                  std::to_string(x) + "," + std::to_string(y) + "],\"dpi_hr\":" + std::to_string(hr) + "}");
    return TRUE;
}
std::string Metadata() {
    DWORD session{}; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    LPWSTR protocol = nullptr; DWORD size{}; int proto = -1;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSClientProtocolType, &protocol, &size)) {
        if (size >= sizeof(USHORT)) proto = *reinterpret_cast<USHORT*>(protocol);
        WTSFreeMemory(protocol);
    }
    DWORD ubr{}, bytes = sizeof(ubr);
    const LSTATUS ubrStatus = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &bytes);
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    DWORD foregroundPid{}; GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    std::vector<std::string> monitors;
    EnumDisplayMonitors(nullptr, nullptr, Monitor, reinterpret_cast<LPARAM>(&monitors));
    std::ostringstream out;
    out << "{\"session_id\":" << session << ",\"console_session_id\":" << WTSGetActiveConsoleSessionId()
        << ",\"wts_protocol\":" << proto << ",\"sm_remote_session\":" << GetSystemMetrics(SM_REMOTESESSION)
        << ",\"product_name\":" << Json(RegText(L"ProductName")) << ",\"display_version\":" << Json(RegText(L"DisplayVersion"))
        << ",\"build\":" << Json(RegText(L"CurrentBuildNumber")) << ",\"ubr\":" << ubr
        << ",\"ubr_status\":" << ubrStatus << ",\"explorer_file_version\":"
        << Json(FileVersion(std::wstring(windows) + L"\\explorer.exe"))
        << ",\"foreground_exe\":" << Json(ProcessName(foregroundPid)) << ",\"monitors\":[";
    for (std::size_t i = 0; i < monitors.size(); ++i) { if (i) out << ','; out << monitors[i]; }
    out << "]}";
    return out.str();
}
BOOL CALLBACK FindTaskbar(HWND hwnd, LPARAM data) {
    const std::wstring cls = Class(hwnd);
    if (cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd")
        reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
    return TRUE;
}
std::string Native(HWND hwnd) {
    RECT rect{}; const BOOL rectOk = GetWindowRect(hwnd, &rect);
    DWORD pid{}; const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    std::ostringstream out;
    out << "{\"hwnd\":" << Handle(hwnd) << ",\"parent\":" << Handle(GetParent(hwnd))
        << ",\"class\":" << Json(Class(hwnd)) << ",\"pid\":" << pid << ",\"tid\":" << tid
        << ",\"visible\":" << IsWindowVisible(hwnd) << ",\"rect\":" << RectJson(rect)
        << ",\"rect_ok\":" << rectOk << ",\"dpi\":" << GetDpiForWindow(hwnd) << '}';
    return out.str();
}
BOOL CALLBACK NativeChild(HWND hwnd, LPARAM data) {
    auto& list = *reinterpret_cast<std::vector<std::string>*>(data);
    if (list.size() >= 512) return FALSE;
    list.push_back(Native(hwnd));
    return TRUE;
}
std::wstring PropertyText(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v; VariantInit(&v);
    std::wstring value;
    if (SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_BSTR && v.bstrVal) value = v.bstrVal;
    VariantClear(&v);
    return value;
}
int PropertyBool(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v; VariantInit(&v);
    int value = -1;
    if (SUCCEEDED(e->GetCachedPropertyValue(id, &v)) && v.vt == VT_BOOL) value = v.boolVal != VARIANT_FALSE;
    VariantClear(&v);
    return value;
}
struct Scan {
    bool includeAppGeometry{};
    ComPtr<IUIAutomationTreeWalker> walker;
    ComPtr<IUIAutomationCacheRequest> cache;
    std::ostringstream data;
    int count{};
    int errors{};
    bool truncated{};
    ULONGLONG start{GetTickCount64()};
    void Walk(IUIAutomationElement* e, int parent, int depth, bool imeParent, bool appParent = false) {
        if (depth > 30 || count >= 600 || GetTickCount64() - start > 8000) { truncated = true; return; }
        const auto cls = PropertyText(e, UIA_ClassNamePropertyId);
        // 应用区只采几何和临时 RuntimeId，绝不保存窗口标题或 AutomationId 中的路径。
        bool appGeometry = appParent || Contains(cls, L"TaskList");
        if (appGeometry && !includeAppGeometry) return;
        auto id = appGeometry ? std::wstring{} : PropertyText(e, UIA_AutomationIdPropertyId);
        if (Contains(id, L"TaskList")) {
            if (!includeAppGeometry) return;
            appGeometry = true; id.clear();
        }
        const int index = count++;
        RECT rect{}; const HRESULT rectHr = e->get_CachedBoundingRectangle(&rect);
        CONTROLTYPEID type{}; e->get_CachedControlType(&type);
        std::wstring hint;
        std::wstring role;
        HRESULT nameHr = S_FALSE;
        if (!appGeometry && ((Contains(cls, L"SystemTray.") && type == UIA_ButtonControlTypeId) || ImeCandidate(cls) || ImeCandidate(id))) {
            BSTR name = nullptr;
            nameHr = e->get_CurrentName(&name);
            if (SUCCEEDED(nameHr) && name) {
                hint = ImeHint(name);
                const std::wstring value(name);
                if (Contains(value, L"Everything")) role = L"Everything";
                else if (Contains(value, L"隐藏") || Contains(value, L"hidden icons")) role = L"overflow";
                else if (Contains(value, L"音量") || Contains(value, L"volume") || Contains(value, L"Volume")) role = L"volume";
                else if (Contains(value, L"网络") || Contains(value, L"Network")) role = L"network";
            }
            SysFreeString(name);
        }
        const bool ime = imeParent || ImeCandidate(cls) || ImeCandidate(id) || !hint.empty();
        const int transform = PropertyBool(e, UIA_IsTransformPatternAvailablePropertyId);
        int canResize = -1;
        if (transform == 1) {
            ComPtr<IUIAutomationTransformPattern> p;
            if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TransformPatternId, IID_PPV_ARGS(&p)))) {
                BOOL value{}; if (SUCCEEDED(p->get_CurrentCanResize(&value))) canResize = value;
            }
        }
        if (index) data << ',';
        data << "{\"index\":" << index << ",\"parent\":" << parent << ",\"class\":" << Json(cls)
             << ",\"automation_id\":" << Json(id) << ",\"control_type\":" << type << ",\"rect\":" << RectJson(rect)
             << ",\"rect_hr\":" << rectHr << ",\"offscreen\":" << PropertyBool(e, UIA_IsOffscreenPropertyId)
             << ",\"enabled\":" << PropertyBool(e, UIA_IsEnabledPropertyId)
             << ",\"invoke_available\":" << PropertyBool(e, UIA_IsInvokePatternAvailablePropertyId)
             << ",\"transform_available\":" << transform << ",\"can_resize\":" << canResize
             << ",\"ime_candidate\":" << (ime ? "true" : "false")
             << ",\"ime_hint\":" << Json(hint) << ",\"name_probe_hr\":" << nameHr
             << ",\"anchor_role\":" << Json(role) << ",\"app_geometry\":" << (appGeometry ? "true" : "false");
        SAFEARRAY* rid = nullptr;
        data << ",\"runtime_id\":[";
        if (SUCCEEDED(e->GetRuntimeId(&rid)) && rid) {
            LONG lo{}, hi{}; SafeArrayGetLBound(rid, 1, &lo); SafeArrayGetUBound(rid, 1, &hi);
            for (LONG j = lo; j <= hi; ++j) { int v{}; SafeArrayGetElement(rid, &j, &v); if (j != lo) data << ','; data << v; }
            SafeArrayDestroy(rid);
        }
        data << "]}";
        // 应用按钮不是研究对象，跳过其子树。
        ComPtr<IUIAutomationElement> child;
        HRESULT hr = walker->GetFirstChildElementBuildCache(e, cache.Get(), &child);
        if (FAILED(hr)) { ++errors; return; }
        while (child && !truncated) {
            Walk(child.Get(), index, depth + 1, ime, appGeometry);
            ComPtr<IUIAutomationElement> next;
            hr = walker->GetNextSiblingElementBuildCache(child.Get(), cache.Get(), &next);
            if (FAILED(hr)) { ++errors; break; }
            child = std::move(next);
        }
    }
};
std::string Capture(IUIAutomation* uia, HWND hwnd, bool includeAppGeometry) {
    std::vector<std::string> native{Native(hwnd)};
    EnumChildWindows(hwnd, NativeChild, reinterpret_cast<LPARAM>(&native));
    Scan scan;
    scan.includeAppGeometry = includeAppGeometry;
    HRESULT hr = E_NOINTERFACE;
    if (uia) {
        hr = uia->get_RawViewWalker(&scan.walker);
        if (SUCCEEDED(hr)) hr = uia->CreateCacheRequest(&scan.cache);
        if (SUCCEEDED(hr)) {
            for (const PROPERTYID prop : {UIA_ClassNamePropertyId, UIA_AutomationIdPropertyId,
                UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId,
                UIA_IsEnabledPropertyId, UIA_IsInvokePatternAvailablePropertyId, UIA_IsTransformPatternAvailablePropertyId}) {
                hr = scan.cache->AddProperty(prop); if (FAILED(hr)) break;
            }
            if (SUCCEEDED(hr)) {
                ComPtr<IUIAutomationElement> root;
                hr = uia->ElementFromHandleBuildCache(hwnd, scan.cache.Get(), &root);
                if (SUCCEEDED(hr) && root) scan.Walk(root.Get(), -1, 0, false);
            }
        }
    }
    std::ostringstream out;
    out << "{\"hwnd\":" << Handle(hwnd) << ",\"native\":[";
    for (std::size_t i = 0; i < native.size(); ++i) { if (i) out << ','; out << native[i]; }
    out << "],\"uia_hr\":" << hr << ",\"uia_errors\":" << scan.errors
        << ",\"uia_truncated\":" << (scan.truncated ? "true" : "false")
        << ",\"native_truncated\":" << (native.size() >= 512 ? "true" : "false")
        << ",\"uia\":[" << scan.data.str() << "]}";
    return out.str();
}
int Number(const wchar_t* s) {
    std::size_t consumed{}; const int n = std::stoi(s, &consumed);
    if (consumed != std::wstring(s).size()) throw std::runtime_error("数字参数无效");
    return n;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path output;
    std::wstring label = L"baseline";
    int samples = 1, interval = 1000, delay = 0;
    bool nativeOnly = false, includeAppGeometry = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--native-only") nativeOnly = true;
            else if (arg == L"--include-app-geometry") includeAppGeometry = true;
            else if (arg == L"--help") {
                std::cout << "只读任务栏采集：--out 文件.jsonl [--label 标记] [--samples 1..600] [--interval-ms 250..10000] [--delay-ms 0..30000] [--native-only] [--include-app-geometry]\n";
                return 0;
            } else if (i + 1 < argc) {
                if (arg == L"--out") output = argv[++i];
                else if (arg == L"--label") label = argv[++i];
                else if (arg == L"--samples") samples = Number(argv[++i]);
                else if (arg == L"--interval-ms") interval = Number(argv[++i]);
                else if (arg == L"--delay-ms") delay = Number(argv[++i]);
                else throw std::runtime_error("未知参数");
            } else throw std::runtime_error("缺少参数值");
        }
        if (output.empty() || samples < 1 || samples > 600 || interval < 250 || interval > 10000 || delay < 0 || delay > 30000)
            throw std::runtime_error("参数范围无效或缺少 --out");
        if (std::filesystem::exists(output)) throw std::runtime_error("输出文件已存在，拒绝覆盖证据");
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        EvidenceFile file(output);
        if (file.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("不能独占创建新输出文件");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(co)) throw std::runtime_error("COM 初始化失败");
        int result = 0;
        {
            ComPtr<IUIAutomation2> uia;
            HRESULT hr = E_NOINTERFACE;
            if (!nativeOnly) {
                hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
                if (SUCCEEDED(hr)) {
                    hr = uia->put_ConnectionTimeout(1000);
                    if (SUCCEEDED(hr)) hr = uia->put_TransactionTimeout(1500);
                    if (SUCCEEDED(hr)) hr = uia->put_AutoSetFocus(FALSE);
                    if (FAILED(hr)) uia.Reset();
                }
            }
            Sleep(static_cast<DWORD>(delay));
            for (int i = 0; i < samples; ++i) {
                const ULONGLONG start = GetTickCount64();
                std::vector<HWND> taskbars;
                EnumWindows(FindTaskbar, reinterpret_cast<LPARAM>(&taskbars));
                std::ostringstream row;
                row << "{\"schema\":1,\"probe_version\":\"0.2\",\"readonly\":true,\"protection_active\":false,\"native_only\":" << (nativeOnly ? "true" : "false") << ",\"utc\":\"" << Stamp()
                    << "\",\"protection_state_scope\":\"collector_only\",\"sample\":" << i << ",\"label\":" << Json(label) << ",\"uia_init_hr\":" << hr
                    << ",\"metadata\":" << Metadata() << ",\"taskbars\":[";
                for (std::size_t j = 0; j < taskbars.size(); ++j) { if (j) row << ','; row << Capture(uia.Get(), taskbars[j], includeAppGeometry); }
                row << "],\"elapsed_ms\":" << (GetTickCount64() - start) << "}\n";
                if (!file.Write(row.str())) { result = 3; break; }
                if (taskbars.empty() || (!nativeOnly && !uia)) result = 2;
                if (i + 1 < samples) Sleep(static_cast<DWORD>(interval));
            }
        }
        CoUninitialize();
        std::cout << "采集结束（本采集器只读，不判断其他进程是否保护）：" << Utf8(output.wstring()) << '\n';
        return result;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
