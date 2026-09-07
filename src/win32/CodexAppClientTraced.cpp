#include "CodexSessionTrace.hpp"
#include "CodexAppClient.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

BOOL WINAPI pixelforge_codex_read_file(HANDLE file,
                                       LPVOID buffer,
                                       DWORD bytes_to_read,
                                       LPDWORD bytes_read,
                                       LPOVERLAPPED overlapped) {
    const BOOL ok = ::ReadFile(file, buffer, bytes_to_read, bytes_read, overlapped);
    if (ok && bytes_read && *bytes_read > 0) {
        pixelforge::win32::codex_trace_rx_bytes(buffer, *bytes_read);
    }
    return ok;
}

BOOL WINAPI pixelforge_codex_write_file(HANDLE file,
                                        LPCVOID buffer,
                                        DWORD bytes_to_write,
                                        LPDWORD bytes_written,
                                        LPOVERLAPPED overlapped) {
    if (buffer && bytes_to_write > 0) {
        pixelforge::win32::codex_trace_tx_bytes(buffer, bytes_to_write);
    }
    return ::WriteFile(file, buffer, bytes_to_write, bytes_written, overlapped);
}

} // namespace

// All headers used by CodexAppClient.cpp are included above. The macros below
// therefore intercept only its runtime pipe calls rather than leaking into STL
// or Windows header declarations.
#define ReadFile pixelforge_codex_read_file
#define WriteFile pixelforge_codex_write_file
#include "CodexAppClient.cpp"
#undef WriteFile
#undef ReadFile
