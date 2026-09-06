#pragma once

#include "AgentMcpServer.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace pixelforge::win32 {

std::wstring make_agent_pipe_name();

class PipeMcpHost {
public:
    PipeMcpHost() = default;
    PipeMcpHost(const PipeMcpHost&) = delete;
    PipeMcpHost& operator=(const PipeMcpHost&) = delete;
    ~PipeMcpHost();

    bool start(std::wstring pipe_name, AgentMcpBindings bindings, std::wstring& error);
    void stop();

    [[nodiscard]] const std::wstring& pipe_name() const noexcept { return pipe_name_; }

private:
    void serve();

    std::wstring pipe_name_;
    AgentMcpBindings bindings_{};
    std::atomic_bool stop_requested_{false};
    std::thread thread_;
};

// Headless mode used by Codex. It forwards MCP stdio bytes to the named-pipe
// server owned by the already-running PixelForge GUI process.
int run_mcp_bridge_stdio(const std::wstring& pipe_name);

} // namespace pixelforge::win32
