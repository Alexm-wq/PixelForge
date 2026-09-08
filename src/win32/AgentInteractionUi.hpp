#pragma once

#include "AgentTask.hpp"
#include "ImageIO.hpp"

#include <windows.h>

#include <mutex>
#include <string>
#include <utility>

namespace pixelforge::win32 {

// Called by the automatic tool session when a generation starts. A Source
// selected before Generate is immediately attached to the newly active task.
void agent_interaction_bind_task(AgentTaskController* task, std::mutex* state_mutex, HWND owner);

// Snapshot the user-selected editable Source. Returns false when no Source is loaded.
bool agent_interaction_source_snapshot(ImageData& image, std::wstring& path);

// Stores the editable Source used by Astra's source seeding/palette workflow.
// The main PixelForge reference button calls this directly.
void agent_interaction_set_source_for_testing(ImageData image, std::wstring path);
inline void agent_interaction_set_source(ImageData image, std::wstring path) {
    agent_interaction_set_source_for_testing(std::move(image), std::move(path));
}

// Synchronously asks the user a question on PixelForge's interaction UI thread.
// The calling model tool blocks until the user answers or dismisses the popup.
bool agent_interaction_ask_user(std::string reason,
                                std::string question,
                                std::string suggestions,
                                std::string& answer,
                                bool& cancelled,
                                std::wstring& error);

} // namespace pixelforge::win32
