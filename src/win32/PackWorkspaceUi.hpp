#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pixelforge::win32 {

struct PackUiCanvasInfo {
    std::string name;
    std::string group;
    int width = 0;
    int height = 0;
    int frame = -1;
};

// Publishes a read-only snapshot of the agent pack to the user workspace window.
// UI selection, playback, scrolling, and tab state remain entirely local and are
// never routed back through the agent tool session.
void pack_workspace_ui_publish(HWND owner,
                               std::uint64_t task_id,
                               std::uint64_t revision,
                               std::wstring project_directory,
                               std::vector<PackUiCanvasInfo> canvases);

} // namespace pixelforge::win32
