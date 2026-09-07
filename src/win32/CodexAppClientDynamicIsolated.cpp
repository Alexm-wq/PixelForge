#include "CodexAppClient.hpp"
#include "CodexSessionTrace.hpp"
#include "LocalAgentToolSession.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// Automatic PixelForge turns must not inherit arbitrary user MCP servers from
// ~/.codex/config.toml. Dynamic PixelForge tools are supplied directly by the
// App Server client, so external MCP configuration is unnecessary and can make
// thread/start wait on unrelated server startup.
BOOL WINAPI pixelforge_isolated_create_process(
    LPCWSTR application_name,
    LPWSTR command_line,
    LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit_handles,
    DWORD creation_flags,
    LPVOID environment,
    LPCWSTR current_directory,
    LPSTARTUPINFOW startup_info,
    LPPROCESS_INFORMATION process_information) {

    if (!command_line) {
        return ::CreateProcessW(application_name, command_line, process_attributes,
                                thread_attributes, inherit_handles, creation_flags,
                                environment, current_directory, startup_info,
                                process_information);
    }

    std::wstring isolated(command_line);
    constexpr std::wstring_view marker = L" app-server";
    const auto app_server = isolated.find(marker);
    if (app_server != std::wstring::npos &&
        isolated.find(L"mcp_servers={}") == std::wstring::npos) {
        isolated.insert(app_server, L" -c \"mcp_servers={}\"");
    }

    std::vector<wchar_t> mutable_line(isolated.begin(), isolated.end());
    mutable_line.push_back(L'\0');
    return ::CreateProcessW(application_name, mutable_line.data(), process_attributes,
                            thread_attributes, inherit_handles, creation_flags,
                            environment, current_directory, startup_info,
                            process_information);
}

// Codex App Server stdio is JSONL: one complete JSON object per physical line.
// Dynamic-tool schemas are formatted across multiple source lines, so collapse
// literal CR/LF at the final transport boundary. Escaped JSON "\\n" sequences
// are two ordinary characters and are preserved exactly.
BOOL WINAPI pixelforge_jsonl_write_file(HANDLE file,
                                        LPCVOID buffer,
                                        DWORD bytes_to_write,
                                        LPDWORD bytes_written,
                                        LPOVERLAPPED overlapped) {
    if (!buffer || bytes_to_write == 0) {
        return ::WriteFile(file, buffer, bytes_to_write, bytes_written, overlapped);
    }

    const auto* chars = static_cast<const char*>(buffer);
    const bool has_delimiter = chars[bytes_to_write - 1] == '\n';
    const DWORD body_size = has_delimiter ? bytes_to_write - 1 : bytes_to_write;

    bool needs_compaction = false;
    for (DWORD i = 0; i < body_size; ++i) {
        if (chars[i] == '\r' || chars[i] == '\n') {
            needs_compaction = true;
            break;
        }
    }

    if (!needs_compaction) {
        return ::WriteFile(file, buffer, bytes_to_write, bytes_written, overlapped);
    }

    std::string compact;
    compact.reserve(bytes_to_write);
    for (DWORD i = 0; i < body_size; ++i) {
        const char c = chars[i];
        if (c != '\r' && c != '\n') compact.push_back(c);
    }
    if (has_delimiter) compact.push_back('\n');

    DWORD offset = 0;
    while (offset < compact.size()) {
        DWORD actual = 0;
        const BOOL ok = ::WriteFile(file, compact.data() + offset,
                                    static_cast<DWORD>(compact.size() - offset),
                                    &actual, overlapped);
        if (!ok || actual == 0) {
            if (bytes_written) *bytes_written = 0;
            return FALSE;
        }
        offset += actual;
    }

    // The caller advances by the original logical JSONL frame size. Report that
    // size after the compacted transport frame has been fully written.
    if (bytes_written) *bytes_written = bytes_to_write;
    return TRUE;
}

} // namespace

#define CreateProcessW pixelforge_isolated_create_process
#define WriteFile pixelforge_jsonl_write_file
#include "CodexAppClientDynamic.cpp"
#undef WriteFile
#undef CreateProcessW
