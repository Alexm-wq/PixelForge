#include "AgentTask.hpp"

#include <utility>

namespace pixelforge {

AgentTaskController::AgentTaskController(PixelDocument& document) : document_(&document) {}

std::uint64_t AgentTaskController::begin(std::string prompt) {
    // If Codex is continuing an interrupted or user-revised drawing, preserve
    // the authoritative canvas when the next task is accepted.
    preserve_canvas_on_accept_ = state_ == TaskState::Accepted && document_ &&
        document_->width() > 0 && document_->height() > 0;

    id_ = next_id_++;
    prompt_ = std::move(prompt);
    state_ = TaskState::AwaitingAgentDecision;
    awaiting_user_review_ = false;
    review_summary_.clear();
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
                *error = "This is a continuation of an existing drawing. Accept the existing canvas size " +
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
    awaiting_user_review_ = false;
    review_summary_.clear();
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
    awaiting_user_review_ = false;
    review_summary_.clear();
    state_ = TaskState::Aborted;
    status_message_ = std::move(reason);
    return true;
}

bool AgentTaskController::finish(std::string summary, std::string* error) {
    if (state_ != TaskState::Accepted) {
        if (error) *error = "task.finish requires an accepted task.";
        return false;
    }

    // The agent cannot finalize artwork on its own. Reuse Finished as the
    // transport-level terminal state so the App Server turn can end cleanly,
    // but keep the submission gated behind awaiting_user_review_.
    preserve_canvas_on_accept_ = false;
    awaiting_user_review_ = true;
    review_summary_ = summary.empty() ? "Artwork submitted for review." : std::move(summary);
    state_ = TaskState::Finished;
    status_message_ = "Awaiting user review.";
    return true;
}

bool AgentTaskController::user_accept_review(std::string* error) {
    if (state_ != TaskState::Finished || !awaiting_user_review_) {
        if (error) *error = "No agent submission is awaiting user review.";
        return false;
    }
    awaiting_user_review_ = false;
    preserve_canvas_on_accept_ = false;
    status_message_ = review_summary_.empty() ? "Artwork accepted by user." : "Artwork accepted by user. " + review_summary_;
    return true;
}

bool AgentTaskController::user_request_changes(std::string feedback, std::string* error) {
    if (state_ != TaskState::Finished || !awaiting_user_review_) {
        if (error) *error = "No agent submission is awaiting user review.";
        return false;
    }
    const auto first_non_ws = feedback.find_first_not_of(" \t\r\n");
    if (first_non_ws == std::string::npos) {
        if (error) *error = "Enter feedback before requesting changes.";
        return false;
    }

    awaiting_user_review_ = false;
    review_summary_.clear();
    // Put the controller back into an active state so begin() recognizes the
    // next turn as a continuation and protects the existing canvas.
    state_ = TaskState::Accepted;
    status_message_ = "User requested changes: " + feedback;
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
    out.review_summary = review_summary_;
    out.awaiting_user_review = awaiting_user_review_;
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
