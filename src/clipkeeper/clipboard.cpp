#include "clipboard.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <cwchar>
#include <sstream>

namespace {

constexpr DWORD kClipboardRetryMs = 8;
constexpr int kClipboardRetryCount = 10;

bool OpenClipboardRetry(HWND hwnd) {
    for (int i = 0; i < kClipboardRetryCount; ++i) {
        if (::OpenClipboard(hwnd)) return true;
        ::Sleep(kClipboardRetryMs);
    }
    return false;
}

UINT PngFormat() {
    static UINT fmt = ::RegisterClipboardFormatW(L"PNG");
    return fmt;
}

bool IsTextFormat(UINT f) {
    return f == CF_UNICODETEXT || f == CF_TEXT || f == CF_OEMTEXT;
}

bool IsImageFormat(UINT f) {
    return f == CF_DIB || f == CF_DIBV5 || f == CF_BITMAP || f == PngFormat();
}

bool CopyHGlobalFormat(UINT format, std::vector<std::uint8_t>& out) {
    HANDLE h = ::GetClipboardData(format);
    if (!h) return false;

    SIZE_T size = ::GlobalSize(h);
    if (size == 0) return false;

    void* p = ::GlobalLock(h);
    if (!p) return false;

    out.resize(size);
    std::memcpy(out.data(), p, size);
    ::GlobalUnlock(h);
    return true;
}

bool BitmapToDib(HBITMAP hbmp, std::vector<std::uint8_t>& out) {
    if (!hbmp) return false;

    BITMAP bm{};
    if (::GetObjectW(hbmp, sizeof(bm), &bm) != sizeof(bm)) return false;
    if (bm.bmWidth <= 0 || bm.bmHeight == 0) return false;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = bm.bmWidth;
    bih.biHeight = bm.bmHeight;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    const DWORD stride = ((static_cast<DWORD>(bm.bmWidth) * 32u + 31u) / 32u) * 4u;
    const DWORD imageSize = stride * static_cast<DWORD>(std::abs(bm.bmHeight));
    bih.biSizeImage = imageSize;

    out.resize(sizeof(BITMAPINFOHEADER) + imageSize);
    std::memcpy(out.data(), &bih, sizeof(bih));

    HDC dc = ::GetDC(nullptr);
    if (!dc) return false;

    BITMAPINFO bi{};
    bi.bmiHeader = bih;
    int scanLines = ::GetDIBits(dc, hbmp, 0, static_cast<UINT>(std::abs(bm.bmHeight)),
                                out.data() + sizeof(BITMAPINFOHEADER), &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);

    if (scanLines == 0) {
        out.clear();
        return false;
    }

    std::memcpy(out.data(), &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
    return true;
}

std::wstring TextSummaryFromClipboard() {
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h));
            if (p) {
                std::wstring s(p);
                ::GlobalUnlock(h);
                std::replace(s.begin(), s.end(), L'\r', L' ');
                std::replace(s.begin(), s.end(), L'\n', L' ');
                if (s.size() > 80) s = s.substr(0, 80) + L"…";
                return s;
            }
        }
    }
    return L"文本";
}

bool ReadDibDimensions(UINT format, int& w, int& h) {
    HANDLE handle = ::GetClipboardData(format);
    if (!handle) return false;
    SIZE_T size = ::GlobalSize(handle);
    if (size < sizeof(BITMAPINFOHEADER)) return false;
    auto* p = static_cast<const std::uint8_t*>(::GlobalLock(handle));
    if (!p) return false;

    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(p);
    bool ok = bih->biSize >= sizeof(BITMAPINFOHEADER) && bih->biWidth != 0 && bih->biHeight != 0;
    if (ok) {
        w = std::abs(bih->biWidth);
        h = std::abs(bih->biHeight);
    }
    ::GlobalUnlock(handle);
    return ok;
}

std::wstring ImageSummaryFromClipboard() {
    int w = 0, h = 0;
    if (ReadDibDimensions(CF_DIBV5, w, h) || ReadDibDimensions(CF_DIB, w, h)) {
        return L"图片 " + std::to_wstring(w) + L"×" + std::to_wstring(h);
    }
    if (::IsClipboardFormatAvailable(CF_BITMAP)) {
        auto hbmp = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP));
        BITMAP bm{};
        if (hbmp && ::GetObjectW(hbmp, sizeof(bm), &bm) == sizeof(bm)) {
            return L"图片 " + std::to_wstring(std::abs(bm.bmWidth)) + L"×" + std::to_wstring(std::abs(bm.bmHeight));
        }
    }
    return L"图片";
}

std::wstring BaseName(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

} // namespace

bool CaptureClipboardSnapshot(HWND hwnd, ClipboardSnapshot& out) {
    out = ClipboardSnapshot{};
    if (!OpenClipboardRetry(hwnd)) return false;

    const bool hasText = ::IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                         ::IsClipboardFormatAvailable(CF_TEXT) ||
                         ::IsClipboardFormatAvailable(CF_OEMTEXT);
    const bool hasImage = ::IsClipboardFormatAvailable(CF_DIBV5) ||
                          ::IsClipboardFormatAvailable(CF_DIB) ||
                          ::IsClipboardFormatAvailable(CF_BITMAP) ||
                          ::IsClipboardFormatAvailable(PngFormat());

    if (!hasText && !hasImage) {
        ::CloseClipboard();
        return false;
    }

    auto addHGlobal = [&](UINT fmt) {
        if (!::IsClipboardFormatAvailable(fmt)) return;
        ClipboardBlob blob{};
        blob.format = fmt;
        if (CopyHGlobalFormat(fmt, blob.bytes)) out.blobs.push_back(std::move(blob));
    };

    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) addHGlobal(CF_UNICODETEXT);
    else if (::IsClipboardFormatAvailable(CF_TEXT)) addHGlobal(CF_TEXT);
    else if (::IsClipboardFormatAvailable(CF_OEMTEXT)) addHGlobal(CF_OEMTEXT);

    // Keep one uncompressed DIB representation plus PNG when available.
    // This avoids storing DIB + DIBV5 duplicates for every large screenshot.
    if (::IsClipboardFormatAvailable(CF_DIBV5)) addHGlobal(CF_DIBV5);
    else if (::IsClipboardFormatAvailable(CF_DIB)) addHGlobal(CF_DIB);
    addHGlobal(PngFormat());

    if (!::IsClipboardFormatAvailable(CF_DIB) && !::IsClipboardFormatAvailable(CF_DIBV5) &&
        ::IsClipboardFormatAvailable(CF_BITMAP)) {
        auto hbmp = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP));
        ClipboardBlob dib{};
        dib.format = CF_DIB;
        if (BitmapToDib(hbmp, dib.bytes)) out.blobs.push_back(std::move(dib));
    }

    if (out.blobs.empty()) {
        ::CloseClipboard();
        return false;
    }

    if (hasImage) {
        out.type = L"图片";
        out.summary = ImageSummaryFromClipboard();
    } else {
        out.type = L"文本";
        out.summary = TextSummaryFromClipboard();
    }

    ::GetLocalTime(&out.time);
    out.sequence = ::GetClipboardSequenceNumber();
    out.tick = ::GetTickCount64();

    ::CloseClipboard();
    return true;
}

bool RestoreClipboardSnapshot(HWND hwnd, const ClipboardSnapshot& snap, DWORD* writtenSequence) {
    if (snap.blobs.empty()) return false;
    if (!OpenClipboardRetry(hwnd)) return false;

    if (!::EmptyClipboard()) {
        ::CloseClipboard();
        return false;
    }

    bool wroteAny = false;
    for (const auto& blob : snap.blobs) {
        if (blob.bytes.empty()) continue;
        HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, blob.bytes.size());
        if (!h) continue;
        void* p = ::GlobalLock(h);
        if (!p) {
            ::GlobalFree(h);
            continue;
        }
        std::memcpy(p, blob.bytes.data(), blob.bytes.size());
        ::GlobalUnlock(h);

        if (::SetClipboardData(blob.format, h)) {
            wroteAny = true; // system owns h now
        } else {
            ::GlobalFree(h);
        }
    }

    ::CloseClipboard();
    if (writtenSequence) *writtenSequence = ::GetClipboardSequenceNumber();
    return wroteAny;
}

ClipboardState InspectClipboard(HWND hwnd) {
    ClipboardState s{};
    s.ownerProcess = ClipboardOwnerProcessName();

    if (!OpenClipboardRetry(hwnd)) return s;
    s.opened = true;

    int count = 0;
    bool anyNonPrivate = false;
    UINT fmt = 0;
    while ((fmt = ::EnumClipboardFormats(fmt)) != 0) {
        ++count;
        if (IsTextFormat(fmt)) s.hasText = true;
        if (IsImageFormat(fmt)) s.hasImage = true;
        if (fmt == CF_HDROP) s.hasFiles = true;
        if (fmt < 0xC000) anyNonPrivate = true;
    }
    s.formatCount = count;
    s.empty = (count == 0);
    s.onlyPrivateFormats = count > 0 && !anyNonPrivate && !s.hasText && !s.hasImage && !s.hasFiles;
    ::CloseClipboard();
    return s;
}

std::wstring ClipboardOwnerProcessName() {
    HWND owner = ::GetClipboardOwner();
    if (!owner) return L"";

    DWORD pid = 0;
    ::GetWindowThreadProcessId(owner, &pid);
    if (!pid) return L"";

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"PID " + std::to_wstring(pid);

    wchar_t path[32768]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (::QueryFullProcessImageNameW(process, 0, path, &size)) result = BaseName(path);
    ::CloseHandle(process);
    if (result.empty()) result = L"PID " + std::to_wstring(pid);
    return result;
}

std::wstring FormatSnapshotLabel(const ClipboardSnapshot& snap) {
    wchar_t timeBuf[32]{};
    swprintf_s(timeBuf, L"%02u:%02u:%02u", snap.time.wHour, snap.time.wMinute, snap.time.wSecond);
    return std::wstring(timeBuf) + L"  [" + snap.type + L"]  " + snap.summary;
}
