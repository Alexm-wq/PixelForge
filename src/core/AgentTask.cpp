#include "AgentTask.hpp"

#include <string_view>
#include <utility>

namespace pixelforge {
namespace {
bool alpha_nonzero(std::uint32_t argb) { return (argb >> 24) != 0; }
bool blank(std::string_view text) { return text.find_first_not_of(" \t\r\n") == std::string_view::npos; }
}

AgentTaskController::AgentTaskController(PixelDocument& document) : document_(&document) {}
void AgentTaskController::clear_revision_guard() { revision_guard_active_ = false; revision_baseline_.clear(); }
void AgentTaskController::clear_user_input() { awaiting_user_input_ = false; user_input_reason_.clear(); user_input_question_.clear(); user_input_suggestions_.clear(); }

std::uint64_t AgentTaskController::begin(std::string prompt) {
    preserve_canvas_on_accept_ = state_ == TaskState::Accepted && document_ && document_->width() > 0 && document_->height() > 0;
    id_ = next_id_++;
    prompt_ = std::move(prompt);
    state_ = TaskState::AwaitingAgentDecision;
    awaiting_user_review_ = false;
    clear_user_input();
    review_summary_.clear();
    status_message_ = preserve_canvas_on_accept_
        ? "Continuation pending: preserve the existing canvas and accept its current dimensions."
        : "Waiting for agent accept/reject decision.";
    if (!preserve_canvas_on_accept_) clear_revision_guard();
    return id_;
}

bool AgentTaskController::accept(int width, int height, std::string* error) {
    if (state_ != TaskState::AwaitingAgentDecision) { if (error) *error = "task.accept is only valid while awaiting the agent decision."; return false; }
    if (awaiting_user_input_) { if (error) *error = "Answer the pending user question before accepting the task."; return false; }
    if (preserve_canvas_on_accept_) {
        if (!document_ || width != document_->width() || height != document_->height()) {
            if (error) *error = "This is a continuation of an existing drawing. Accept the existing canvas size " +
                std::to_string(document_ ? document_->width() : 0) + "x" +
                std::to_string(document_ ? document_->height() : 0) + "; resizing would destroy the current artwork.";
            return false;
        }
        preserve_canvas_on_accept_ = false;
        state_ = TaskState::Accepted;
        status_message_ = revision_guard_active_
            ? "Revision continuation accepted; existing canvas preserved and protected from bulk replacement."
            : "Continuation accepted; existing canvas preserved.";
        return true;
    }
    std::string resize_error;
    if (!document_->can_resize(width, height, &resize_error) || !document_->resize(width, height, &resize_error)) {
        if (error) *error = resize_error;
        return false;
    }
    clear_revision_guard();
    state_ = TaskState::Accepted;
    status_message_ = "Task accepted by agent.";
    return true;
}

bool AgentTaskController::reject(std::string reason, std::string* error) {
    if (state_ != TaskState::AwaitingAgentDecision) { if (error) *error = "task.reject is only valid while awaiting the agent decision."; return false; }
    if (blank(reason)) { if (error) *error = "task.reject requires a user-facing reason."; return false; }
    preserve_canvas_on_accept_ = false; awaiting_user_review_ = false; clear_user_input(); review_summary_.clear(); clear_revision_guard();
    state_ = TaskState::Rejected; status_message_ = std::move(reason); return true;
}

bool AgentTaskController::abort(std::string reason, std::string* error) {
    if (state_ != TaskState::Accepted) { if (error) *error = "task.abort is only valid after the agent accepted the task."; return false; }
    if (blank(reason)) { if (error) *error = "task.abort requires a user-facing reason."; return false; }
    preserve_canvas_on_accept_ = false; awaiting_user_review_ = false; clear_user_input(); review_summary_.clear(); clear_revision_guard();
    state_ = TaskState::Aborted; status_message_ = std::move(reason); return true;
}

bool AgentTaskController::finish(std::string summary, std::string* error) {
    if (state_ != TaskState::Accepted) { if (error) *error = "task.finish requires an accepted task."; return false; }
    if (awaiting_user_input_) { if (error) *error = "Resolve the pending user question before finishing the task."; return false; }
    preserve_canvas_on_accept_ = false;
    awaiting_user_review_ = true;
    review_summary_ = summary.empty() ? "Artwork submitted for review." : std::move(summary);
    state_ = TaskState::Finished;
    status_message_ = "Awaiting user review.";
    return true;
}

bool AgentTaskController::user_accept_review(std::string* error) {
    if (state_ != TaskState::Finished || !awaiting_user_review_) { if (error) *error = "No agent submission is awaiting user review."; return false; }
    awaiting_user_review_ = false; preserve_canvas_on_accept_ = false; clear_revision_guard();
    status_message_ = review_summary_.empty() ? "Artwork accepted by user." : "Artwork accepted by user. " + review_summary_;
    return true;
}

bool AgentTaskController::user_request_changes(std::string feedback, std::string* error) {
    if (state_ != TaskState::Finished || !awaiting_user_review_) { if (error) *error = "No agent submission is awaiting user review."; return false; }
    if (blank(feedback)) { if (error) *error = "Enter feedback before requesting changes."; return false; }
    awaiting_user_review_ = false;
    review_summary_.clear();
    revision_baseline_ = document_ ? document_->pixels() : std::vector<std::uint32_t>{};
    revision_guard_active_ = document_ && !revision_baseline_.empty();
    state_ = TaskState::Accepted;
    status_message_ = "User requested changes; existing reviewed artwork is protected from bulk replacement: " + feedback;
    return true;
}

bool AgentTaskController::request_user_input(std::string reason, std::string question, std::string suggestions, std::string* error) {
    if (state_ != TaskState::AwaitingAgentDecision && state_ != TaskState::Accepted) { if (error) *error = "The agent may ask the user only while deciding or working on an active task."; return false; }
    if (awaiting_user_review_ || awaiting_user_input_) { if (error) *error = "A user interaction is already pending."; return false; }
    if (blank(reason) || blank(question)) { if (error) *error = "A user question requires both a clear reason and a concrete question."; return false; }
    awaiting_user_input_ = true;
    user_input_reason_ = std::move(reason);
    user_input_question_ = std::move(question);
    user_input_suggestions_ = std::move(suggestions);
    status_message_ = "Agent paused for user input: " + user_input_question_;
    return true;
}

bool AgentTaskController::user_answer_input(std::string answer, std::string* error) {
    if (!awaiting_user_input_) { if (error) *error = "No agent question is awaiting an answer."; return false; }
    if (blank(answer)) { if (error) *error = "Enter an answer or choose one of the agent's suggestions."; return false; }
    clear_user_input();
    status_message_ = "User answered the agent's question; continue the current task.";
    return true;
}

bool AgentTaskController::revision_candidate_allowed(const std::vector<std::uint32_t>& candidate, std::string* error) const {
    if (!revision_guard_active_ || revision_baseline_.empty()) return true;
    if (candidate.size() != revision_baseline_.size()) {
        if (error) *error = "Revision guard rejected a canvas-size change. Continue editing the existing artwork at its current size.";
        return false;
    }
    std::size_t changed = 0, baseline_opaque = 0, erased_opaque = 0;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        const auto before = revision_baseline_[i], after = candidate[i];
        if (before != after) ++changed;
        if (alpha_nonzero(before)) { ++baseline_opaque; if (!alpha_nonzero(after)) ++erased_opaque; }
    }
    const double changed_fraction = candidate.empty() ? 0.0 : static_cast<double>(changed) / static_cast<double>(candidate.size());
    const double erased_fraction = baseline_opaque == 0 ? 0.0 : static_cast<double>(erased_opaque) / static_cast<double>(baseline_opaque);
    if (changed_fraction > 0.72 || erased_fraction > 0.45) {
        if (error) *error = "Revision guard rejected a destructive rewrite of the reviewed artwork (" +
            std::to_string(static_cast<int>(changed_fraction * 100.0)) + "% of canvas changed, " +
            std::to_string(static_cast<int>(erased_fraction * 100.0)) + "% of existing painted pixels erased). Preserve the current image and apply the requested changes incrementally.";
        return false;
    }
    return true;
}

void AgentTaskController::set_next_task_id_for_restore(std::uint64_t task_id) {
    if (task_id > 0) next_id_ = task_id;
}

bool AgentTaskController::resume_existing_project(std::string prompt, std::string* error) {
    if (id_ == 0 || awaiting_user_review_ || awaiting_user_input_) {
        if (error) *error = "The loaded project cannot resume while a user interaction or review is pending.";
        return false;
    }
    if (state_ != TaskState::Accepted && state_ != TaskState::Finished) {
        if (error) *error = "The loaded project can resume only from an active or accepted finished state.";
        return false;
    }
    if (blank(prompt)) {
        if (error) *error = "A project repair turn requires a prompt.";
        return false;
    }
    if (state_ == TaskState::Finished) clear_revision_guard();
    preserve_canvas_on_accept_ = false;
    awaiting_user_review_ = false;
    clear_user_input();
    review_summary_.clear();
    prompt_ = std::move(prompt);
    state_ = TaskState::Accepted;
    status_message_ = revision_guard_active_
        ? "Loaded project repair resumed; reviewed artwork remains protected from destructive replacement."
        : "Loaded project repair resumed on the existing workspace.";
    return true;
}

void AgentTaskController::set_source_reference(ReferenceSlot reference) { source_reference_ = std::move(reference); }
void AgentTaskController::set_content_reference(ReferenceSlot reference) { content_reference_ = std::move(reference); }
void AgentTaskController::set_style_reference(ReferenceSlot reference) { style_reference_ = std::move(reference); }

AgentTaskSnapshot AgentTaskController::snapshot() const {
    AgentTaskSnapshot out;
    out.id = id_; out.state = state_; out.prompt = prompt_; out.status_message = status_message_; out.review_summary = review_summary_;
    out.awaiting_user_review = awaiting_user_review_; out.awaiting_user_input = awaiting_user_input_;
    out.user_input_reason = user_input_reason_; out.user_input_question = user_input_question_; out.user_input_suggestions = user_input_suggestions_;
    out.revision_guard_active = revision_guard_active_; out.continuation_pending = preserve_canvas_on_accept_;
    out.source_reference = source_reference_; out.content_reference = content_reference_; out.style_reference = style_reference_;
    out.canvas_width = document_->width(); out.canvas_height = document_->height(); out.document_revision = document_->revision(); out.limits = document_->limits();
    return out;
}

const char* task_state_name(TaskState state) noexcept {
    switch (state) {
        case TaskState::Idle: return "IDLE"; case TaskState::AwaitingAgentDecision: return "AWAITING AGENT";
        case TaskState::Accepted: return "ACCEPTED"; case TaskState::Rejected: return "REJECTED";
        case TaskState::Aborted: return "ABORTED"; case TaskState::Finished: return "FINISHED";
    }
    return "UNKNOWN";
}

} // namespace pixelforge
