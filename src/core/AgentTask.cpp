#include "AgentTask.hpp"

#include <utility>

namespace pixelforge {

AgentTaskController::AgentTaskController(PixelDocument& document) : document_(&document) {}

std::uint64_t AgentTaskController::begin(std::string prompt) {
    // If a host-side interruption stopped Codex while the task was still accepted,
    // the authoritative canvas is valuable work-in-progress. Treat the next begin
    // as a continuation and preserve that canvas when the agent accepts again.
    preserve_canvas_on_accept_ = state_ == TaskState::Accepted && document_ &&
        document_->width() > 0 && document_->height() > 0;

    id_ = next_id_++;
    prompt_ = std::move(prompt);
    state_ = TaskState::AwaitingAgentDecision;
    status_message_ = preserve_canvas_on_accept_
        ? "Continuation pending: preserve the existing canvas and accept its current dimensions."
        : "Waiting for agent accept/reject decision.";
    return id_;
}

bool AgentTaskController::accept(int width, int height, std::string* error) {
    if (state_ != TaskState::AwaitingAgentDecision) {
        if (error) *error = "task.accept is only valid while awaiting the agent decision.";
        return false;
    }

    if (preserve_canvas_on_accept_) {
        if (!document_ || width != document_->width() || height != document_->height()) {
            if (error) {
                *error = "This is a continuation of an interrupted drawing. Accept the existing canvas size " +
                    std::to_string(document_ ? document_->width() : 0) + "x" +
                    std::to_string(document_ ? document_->height() : 0) +
                    "; resizing would destroy the current artwork.";
            }
            return false;
        }
        preserve_canvas_on_accept_ = false;
        state_ = TaskState::Accepted;
        status_message_ = "Continuation accepted; existing canvas preserved.";
        return true;
    }

    std::string resize_error;
    if (!document_->can_resize(width, height, &resize_error)) {
        if (error) *error = resize_error;
        return false;
    }
    if (!document_->resize(width, height, &resize_error)) {
        if (error) *error = resize_error;
        return false;
    }
    state_ = TaskState::Accepted;
    status_message_ = "Task accepted by agent.";
    return true;
}

bool AgentTaskController::reject(std::string reason, std::string* error) {
    if (state_ != TaskState::AwaitingAgentDecision) {
        if (error) *error = "task.reject is only valid while awaiting the agent decision.";
        return false;
    }
    if (reason.empty()) {
        if (error) *error = "task.reject requires a user-facing reason.";
        return false;
    }
    preserve_canvas_on_accept_ = false;
    state_ = TaskState::Rejected;
    status_message_ = std::move(reason);
    return true;
}

bool AgentTaskController::abort(std::string reason, std::string* error) {
    if (state_ != TaskState::Accepted) {
        if (error) *error = "task.abort is only valid after the agent accepted the task.";
        return false;
    }
    if (reason.empty()) {
        if (error) *error = "task.abort requires a user-facing reason.";
        return false;
    }
    preserve_canvas_on_accept_ = false;
    state_ = TaskState::Aborted;
    status_message_ = std::move(reason);
    return true;
}

bool AgentTaskController::finish(std::string summary, std::string* error) {
    if (state_ != TaskState::Accepted) {
        if (error) *error = "task.finish requires an accepted task.";
        return false;
    }
    preserve_canvas_on_accept_ = false;
    state_ = TaskState::Finished;
    status_message_ = summary.empty() ? "Task finished." : std::move(summary);
    return true;
}

void AgentTaskController::set_content_reference(ReferenceSlot reference) {
    content_reference_ = std::move(reference);
}

void AgentTaskController::set_style_reference(ReferenceSlot reference) {
    style_reference_ = std::move(reference);
}

AgentTaskSnapshot AgentTaskController::snapshot() const {
    AgentTaskSnapshot out;
    out.id = id_;
    out.state = state_;
    out.prompt = prompt_;
    out.status_message = status_message_;
    out.content_reference = content_reference_;
    out.style_reference = style_reference_;
    out.canvas_width = document_->width();
    out.canvas_height = document_->height();
    out.document_revision = document_->revision();
    out.limits = document_->limits();
    return out;
}

const char* task_state_name(TaskState state) noexcept {
    switch (state) {
        case TaskState::Idle: return "IDLE";
        case TaskState::AwaitingAgentDecision: return "AWAITING AGENT";
        case TaskState::Accepted: return "ACCEPTED";
        case TaskState::Rejected: return "REJECTED";
        case TaskState::Aborted: return "ABORTED";
        case TaskState::Finished: return "FINISHED";
    }
    return "UNKNOWN";
}

} // namespace pixelforge
