#pragma once

#include "PixelDocument.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pixelforge {

enum class TaskState { Idle, AwaitingAgentDecision, Accepted, Rejected, Aborted, Finished };

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
    std::string review_summary;
    bool awaiting_user_review = false;
    bool awaiting_user_input = false;
    std::string user_input_reason;
    std::string user_input_question;
    std::string user_input_suggestions;
    bool revision_guard_active = false;
    bool continuation_pending = false;
    ReferenceSlot source_reference;
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
    bool user_accept_review(std::string* error = nullptr);
    bool user_request_changes(std::string feedback, std::string* error = nullptr);
    bool request_user_input(std::string reason, std::string question, std::string suggestions,
                            std::string* error = nullptr);
    bool user_answer_input(std::string answer, std::string* error = nullptr);
    void set_source_reference(ReferenceSlot reference);
    void set_content_reference(ReferenceSlot reference);
    void set_style_reference(ReferenceSlot reference);

    // Project restoration needs to preserve the persisted workspace id so pack
    // autosave continues writing to the same projects/task_N directory.
    void set_next_task_id_for_restore(std::uint64_t task_id);
    // Reuse an already-loaded project's accepted/finished task id for another
    // agent turn instead of silently forking the project into a new task folder.
    bool resume_existing_project(std::string prompt, std::string* error = nullptr);

    [[nodiscard]] AgentTaskSnapshot snapshot() const;
    [[nodiscard]] TaskState state() const noexcept { return state_; }
    [[nodiscard]] bool awaiting_user_review() const noexcept { return awaiting_user_review_; }
    [[nodiscard]] bool awaiting_user_input() const noexcept { return awaiting_user_input_; }
    [[nodiscard]] bool revision_guard_active() const noexcept { return revision_guard_active_; }
    [[nodiscard]] bool revision_candidate_allowed(const std::vector<std::uint32_t>& candidate,
                                                  std::string* error = nullptr) const;
    [[nodiscard]] bool terminal() const noexcept {
        return state_ == TaskState::Rejected || state_ == TaskState::Aborted ||
               (state_ == TaskState::Finished && !awaiting_user_review_);
    }
private:
    void clear_revision_guard();
    void clear_user_input();
    PixelDocument* document_ = nullptr;
    std::uint64_t next_id_ = 1;
    std::uint64_t id_ = 0;
    TaskState state_ = TaskState::Idle;
    bool preserve_canvas_on_accept_ = false;
    bool awaiting_user_review_ = false;
    bool awaiting_user_input_ = false;
    bool revision_guard_active_ = false;
    std::string prompt_;
    std::string status_message_;
    std::string review_summary_;
    std::string user_input_reason_;
    std::string user_input_question_;
    std::string user_input_suggestions_;
    ReferenceSlot source_reference_;
    ReferenceSlot content_reference_;
    ReferenceSlot style_reference_;
    std::vector<std::uint32_t> revision_baseline_;
};

const char* task_state_name(TaskState state) noexcept;

} // namespace pixelforge
