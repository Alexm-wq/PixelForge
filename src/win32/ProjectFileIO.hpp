#pragma once
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace pixelforge::win32 {
// Write beside the target, close it, then replace. A failed save keeps the old
// manifest readable. Never delete a locked destination to force replacement.
inline bool write_project_text_atomic(const std::filesystem::path& path,
                                      std::string_view text, std::wstring& error) {
    static std::atomic_uint64_t sequence{0};
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD code = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (file != INVALID_HANDLE_VALUE) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>((std::min)(text.size() - offset, std::size_t{1u << 20}));
            if (!WriteFile(file, text.data() + offset, chunk, &written, nullptr) || !written) {
                code = GetLastError(); if (!code) code = ERROR_WRITE_FAULT; break;
            }
            offset += written;
        }
        if (!code && !FlushFileBuffers(file)) code = GetLastError();
        if (!CloseHandle(file) && !code) code = GetLastError();
        if (!code && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            code = GetLastError();
        if (code) DeleteFileW(temporary.c_str());
    }
    if (code) {
        error = L"Could not finalize " + path.wstring() + L" (Windows error " + std::to_wstring(code) + L").";
        return false;
    }
    error.clear();
    return true;
}
}
