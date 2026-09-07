#pragma once

#include "AgentMcpServer.hpp"
#include "SessionRecorder.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <unordered_map>

namespace pixelforge::win32 {

struct LocalRecordBindings {
    SessionRecorder* recorder = nullptr;
    AgentTaskController* task = nullptr;
    std::mutex* state_mutex = nullptr;
    HWND hwnd = nullptr;
    std::wstring recording_directory;
};

struct LocalToolResult {
    bool success = false;
    std::string text;
    std::string image_base64;
    std::string image_mime;
};

// In-process adapter used by Codex App Server dynamicTools. Artwork calls are
// routed through the existing PixelForge MCP implementation over anonymous pipes,
// preserving one code path for task/edit/view/palette/history/io semantics while
// eliminating external MCP subprocesses and named-pipe startup from Generate.
class LocalAgentToolSession {
public:
    LocalAgentToolSession() = default;
    LocalAgentToolSession(const LocalAgentToolSession&) = delete;
    LocalAgentToolSession& operator=(const LocalAgentToolSession&) = delete;
    ~LocalAgentToolSession();

    bool start(AgentMcpBindings bindings, LocalRecordBindings record_bindings, std::wstring& error);
    void stop();
    LocalToolResult call(std::string_view tool, std::string_view arguments_json);

private:
    LocalToolResult call_art_tool(std::string_view tool, std::string_view arguments_json);
    LocalToolResult call_record_tool(std::string_view arguments_json);
    bool write_line(std::string_view line);
    bool read_line(std::string& line);

    AgentMcpBindings bindings_{};
    LocalRecordBindings record_bindings_{};
    HANDLE server_input_read_ = nullptr;
    HANDLE client_input_write_ = nullptr;
    HANDLE client_output_read_ = nullptr;
    HANDLE server_output_write_ = nullptr;
    std::thread server_thread_;
    std::mutex call_mutex_;
    std::string receive_buffer_;
    std::uint64_t next_id_ = 1;
    std::atomic_bool running_{false};
    std::unordered_set<std::string> delivered_observations_;
    std::unordered_map<std::string, std::string> reference_observations_;
    std::uint64_t observed_task_ = 0;
    std::uint64_t observed_revision_ = 0;
    bool has_observed_revision_ = false;
};

// JSON array suitable for thread/start.dynamicTools.
std::string pixelforge_dynamic_tools_json();

} // namespace pixelforge::win32
