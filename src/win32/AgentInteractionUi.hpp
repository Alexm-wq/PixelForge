#pragma once

#include "AgentTask.hpp"
#include "ImageIO.hpp"

#include <windows.h>

#include <mutex>
#include <string>

namespace pixelforge::win32 {

// Called by the automatic tool session when a generation starts. The editable
// source selector itself is bootstrapped before Generate, so a source chosen by
// the user is immediately attached to the newly active task here.
void agent_interaction_bind_task(AgentTaskController* task, std::mutex* state_mutex, HWND owner);

// Snapshot the user-selected editable source. Returns false when no source is loaded.
bool agent_interaction_source_snapshot(ImageData& image, std::wstring& path);

// Test hook; also useful for non-GUI harnesses. Production users normally set
// the source from the dedicated Source reference tool window.
void agent_interaction_set_source_for_testing(ImageData image, std::wstring path);

// Synchronously asks the user a question on PixelForge's interaction UI thread.
// The calling model tool blocks until the user answers or dismisses the popup.
bool agent_interaction_ask_user(std::string reason,
                                std::string question,
                                std::string suggestions,
                                std::string& answer,
                                bool& cancelled,
                                std::wstring& error);

} // namespace pixelforge::win32
