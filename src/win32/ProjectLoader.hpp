#pragma once

#include "AgentTask.hpp"
#include "LocalAgentToolSession.hpp"
#include "PixelDocument.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace pixelforge::win32 {

struct ProjectLoadSummary {
    std::uint64_t task_id = 0;
    std::size_t canvas_count = 0;
    std::wstring manifest_path;
    std::wstring source_directory;
    std::wstring active_directory;
};

// Opens the native Windows file picker for a PixelForge project manifest.
// Returns false with an empty error when the user cancels.
bool choose_project_manifest(HWND owner, std::wstring& manifest_path, std::wstring& error);

// Reconstructs the complete saved pack through PixelForge's normal pack tool
// path, so the restored documents are exactly the documents Astra will see.
// The caller must start LocalAgentToolSession before calling this function.
bool load_project_into_workspace(const std::wstring& manifest_path,
                                 const std::wstring& repo_root,
                                 LocalAgentToolSession& tools,
                                 AgentTaskController& task,
                                 PixelDocument& primary,
                                 std::mutex& state_mutex,
                                 ProjectLoadSummary& summary,
                                 std::wstring& error);

} // namespace pixelforge::win32
