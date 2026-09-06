#pragma once

#include "AgentTask.hpp"
#include "SessionRecorder.hpp"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace pixelforge::win32 {

struct RecordMcpBindings {
    SessionRecorder* recorder = nullptr;
    AgentTaskController* task = nullptr;
    std::mutex* state_mutex = nullptr;
    HWND hwnd = nullptr;
    std::wstring recording_directory;
};

std::wstring make_record_pipe_name();

class RecordMcpHost {
public:
    RecordMcpHost() = default;
    RecordMcpHost(const RecordMcpHost&) = delete;
    RecordMcpHost& operator=(const RecordMcpHost&) = delete;
    ~RecordMcpHost();

    bool start(std::wstring pipe_name, RecordMcpBindings bindings, std::wstring& error);
    void stop();

private:
    void serve();

    std::wstring pipe_name_;
    RecordMcpBindings bindings_;
    std::atomic_bool stop_requested_{false};
    std::thread thread_;
};

// Headless MCP stdio bridge launched by Codex. It only forwards bytes to the
// already-running PixelForge GUI's private recording pipe.
int run_record_bridge_stdio(const std::wstring& pipe_name);

} // namespace pixelforge::win32
