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
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// Host-only project reconstruction payload. This is deliberately not part of
// the agent tool schema: opening a project is a user/UI operation, not something
// Astra should be able to invoke. Patches are the already-decoded native canvas
// pixels encoded in PixelForge's compact exact-pixel grammar.
struct LocalPackRestoreCanvas {
    std::string name;
    std::vector<std::string> patches;
};

class LocalAgentToolSession {
public:
    LocalAgentToolSession() = default;
    LocalAgentToolSession(const LocalAgentToolSession&) = delete;
    LocalAgentToolSession& operator=(const LocalAgentToolSession&) = delete;
    ~LocalAgentToolSession();

    bool start(AgentMcpBindings bindings, LocalRecordBindings record_bindings, std::wstring& error);
    void stop();
    LocalToolResult call(std::string_view tool, std::string_view arguments_json);

    // Trusted UI-only restore path. It bypasses the agent-facing workspace
    // wrapper so loading N saved canvases does not autosave/re-export/re-publish
    // the complete pack after every restored patch. The finished pack is returned
    // as one list snapshot; ProjectLoader publishes it once after reconstruction.
    LocalToolResult restore_project_pack(std::string_view create_arguments_json,
                                         const std::vector<LocalPackRestoreCanvas>& canvases);

private:
    LocalToolResult base_call(std::string_view tool, std::string_view arguments_json);
    LocalToolResult call_art_tool(std::string_view tool, std::string_view arguments_json);
    LocalToolResult call_record_tool(std::string_view arguments_json);
    LocalToolResult call_pack_tool(std::string_view arguments_json);
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
    std::unordered_map<std::string, std::string> render_observations_;
    std::uint64_t observed_task_ = 0;
    std::uint64_t observed_revision_ = 0;
    bool has_observed_revision_ = false;
};

std::string pixelforge_dynamic_tools_json();

} // namespace pixelforge::win32
