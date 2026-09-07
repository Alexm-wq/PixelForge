#pragma once

#include "AgentTask.hpp"
#include "ImageIO.hpp"
#include "PixelDocument.hpp"

#include <windows.h>

#include <mutex>
#include <string>

namespace pixelforge::win32 {

struct AgentMcpBindings {
    PixelDocument* document = nullptr;
    AgentTaskController* task = nullptr;
    ImageData* content_reference = nullptr;
    ImageData* style_reference = nullptr;
    std::wstring* content_path = nullptr;
    std::wstring* style_path = nullptr;
    std::mutex* state_mutex = nullptr;
    HWND hwnd = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    bool close_window_on_exit = true;
};

// Serves PixelForge as a compact local MCP stdio server while the same process
// keeps the native GUI visible. The host (Codex) owns stdin/stdout; PixelForge
// never prints diagnostics to stdout because that would corrupt MCP framing.
int run_mcp_stdio(AgentMcpBindings bindings);

} // namespace pixelforge::win32
