#include "AgentMcpServer.hpp"
#include "ReferenceResponseLimiter.hpp"

#include <windows.h>

#include <string>

namespace {

// AgentMcpServer.cpp intentionally keeps its large tool schema readable. MCP's
// stdio transport is JSONL, however, so embedded formatting newlines must never
// escape onto the wire. Intercept only this translation unit's stdout writes,
// preserve the final message delimiter, and collapse internal CR/LF to spaces.
BOOL WINAPI pixelforge_mcp_write_file(HANDLE file,
                                      LPCVOID buffer,
                                      DWORD bytes_to_write,
                                      LPDWORD bytes_written,
                                      LPOVERLAPPED overlapped) {
    if (!buffer || bytes_to_write == 0) {
        return ::WriteFile(file, buffer, bytes_to_write, bytes_written, overlapped);
    }

    const auto* chars = static_cast<const char*>(buffer);
    std::string compact;
    compact.reserve(bytes_to_write);
    const bool has_delimiter = chars[bytes_to_write - 1] == '\n';
    const DWORD body_size = has_delimiter ? bytes_to_write - 1 : bytes_to_write;
    bool last_was_space = false;
    for (DWORD i = 0; i < body_size; ++i) {
        const char c = chars[i];
        if (c == '\r' || c == '\n') {
            if (!last_was_space) compact.push_back(' ');
            last_was_space = true;
        } else {
            compact.push_back(c);
            last_was_space = c == ' ' || c == '\t';
        }
    }
    if (has_delimiter) compact.push_back('\n');

    // Content/style references are capped by total delivered pixel area at
    // 256x256 (65,536 pixels). The full-resolution reference remains loaded
    // in PixelForge; canvas render observations are never modified here.
    std::string delivered = pixelforge::win32::limit_reference_response(compact);

    DWORD actual = 0;
    const BOOL ok = ::WriteFile(file, delivered.data(), static_cast<DWORD>(delivered.size()), &actual, overlapped);
    if (bytes_written) *bytes_written = ok ? bytes_to_write : 0;
    return ok;
}

} // namespace

#define WriteFile pixelforge_mcp_write_file
#include "AgentMcpServer.cpp"
#undef WriteFile
