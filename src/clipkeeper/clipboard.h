#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>

struct ClipboardBlob {
    UINT format{};
    std::vector<std::uint8_t> bytes;
};

struct ClipboardSnapshot {
    std::vector<ClipboardBlob> blobs;
    std::wstring summary;
    std::wstring type;
    SYSTEMTIME time{};
    DWORD sequence{};
    ULONGLONG tick{};
};

struct ClipboardState {
    bool opened{false};
    bool empty{true};
    bool hasText{false};
    bool hasImage{false};
    bool hasFiles{false};
    bool onlyPrivateFormats{false};
    int formatCount{0};
    std::wstring ownerProcess;
};

bool CaptureClipboardSnapshot(HWND hwnd, ClipboardSnapshot& out);
bool RestoreClipboardSnapshot(HWND hwnd, const ClipboardSnapshot& snap, DWORD* writtenSequence = nullptr);
ClipboardState InspectClipboard(HWND hwnd);
std::wstring ClipboardOwnerProcessName();
std::wstring FormatSnapshotLabel(const ClipboardSnapshot& snap);
