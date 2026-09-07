#pragma once

#include "PixelDocument.hpp"

#include <cstdint>
#include <string>

namespace pixelforge {

enum class TaskState {
    Idle,
    AwaitingAgentDecision,
    Accepted,
    Rejected,
    Aborted,
    Finished
};

struct ReferenceSlot {
    std::string path;
    int width = 0;
    int height = 0;
    bool present = false;
};

struct AgentTaskSnapshot {
    std::uint64_t id = 0;
    TaskState state = TaskState::Idle;
    std::string prompt;
    std::string status_message;
    ReferenceSlot content_reference;
    ReferenceSlot style_reference;
    int canvas_width = 0;
    int canvas_height = 0;
    std::uint64_t document_revision = 0;
    CanvasLimits limits;
};

class AgentTaskController {
public:
    explicit AgentTaskController(PixelDocument& document);

    std::uint64_t begin(std::string prompt);
    bool accept(int width, int height, std::string* error = nullptr);
    bool reject(std::string reason, std::string* error = nullptr);
    bool abort(std::string reason, std::string* error = nullptr);
    bool finish(std::string summary, std::string* error = nullptr);

    void set_content_reference(ReferenceSlot reference);
    void set_style_reference(ReferenceSlot reference);

    [[nodiscard]] AgentTaskSnapshot snapshot() const;
    [[nodiscard]] TaskState state() const noexcept { return state_; }
    [[nodiscard]] bool terminal() const noexcept {
        return state_ == TaskState::Rejected || state_ == TaskState::Aborted || state_ == TaskState::Finished;
    }

private:
    PixelDocument* document_ = nullptr;
    std::uint64_t next_id_ = 1;
    std::uint64_t id_ = 0;
    TaskState state_ = TaskState::Idle;
    bool preserve_canvas_on_accept_ = false;
    std::string prompt_;
    std::string status_message_;
    ReferenceSlot content_reference_;
    ReferenceSlot style_reference_;
};

const char* task_state_name(TaskState state) noexcept;

} // namespace pixelforge
