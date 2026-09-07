#include "CodexAppClient.hpp"
#include "CodexSessionTrace.hpp"
#include "LocalAgentToolSession.hpp"

#include <windows.h>

#include <string>
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

} // namespace

#define CreateProcessW pixelforge_isolated_create_process
#include "CodexAppClientDynamic.cpp"
#undef CreateProcessW
