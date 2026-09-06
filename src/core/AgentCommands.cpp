#include "AgentCommands.hpp"

#include <cstdlib>
#include <utility>

namespace pixelforge {

namespace {

bool draw_line(PixelDocument::Transaction& tx, int x0, int y0, int x1, int y1, std::uint32_t argb) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if (!tx.set_pixel(x0, y0, argb)) return false;
        if (x0 == x1 && y0 == y1) return true;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

} // namespace

AgentCommandRouter::AgentCommandRouter(PixelDocument& document, AgentTaskController& task)
    : document_(&document), task_(&task) {}

AgentCommandResult AgentCommandRouter::result(bool ok, AgentErrorCode code, std::string message) const {
    AgentCommandResult out;
    out.ok = ok;
    out.error = code;
    out.message = std::move(message);
    out.task = task_->snapshot();
    out.revision = document_->revision();
    return out;
}

AgentCommandResult AgentCommandRouter::validate_task(std::uint64_t task_id) const {
    const auto snap = task_->snapshot();
    if (snap.id == 0 || snap.state == TaskState::Idle) {
        return result(false, AgentErrorCode::NoActiveTask, "No active PixelForge task.");
    }
    if (task_id != snap.id) {
        return result(false, AgentErrorCode::StaleTask,
                      "Command targets a stale task id; refresh task.get before continuing.");
    }
    return result(true);
}

AgentCommandResult AgentCommandRouter::validate_editable(std::uint64_t task_id,
                                                          std::uint64_t expected_revision) const {
    auto task_check = validate_task(task_id);
    if (!task_check.ok) return task_check;
    if (task_->state() != TaskState::Accepted) {
        return result(false, AgentErrorCode::InvalidState,
                      "Pixel edits are only valid while the task is accepted and active.");
    }
    if (expected_revision != document_->revision()) {
        return result(false, AgentErrorCode::StaleRevision,
                      "Document revision changed; inspect the current revision before editing.");
    }
    return result(true);
}

AgentCommandResult AgentCommandRouter::task_get() const {
    const auto snap = task_->snapshot();
    if (snap.id == 0 || snap.state == TaskState::Idle) {
        return result(false, AgentErrorCode::NoActiveTask, "No active PixelForge task.");
    }
    return result(true);
}

AgentCommandResult AgentCommandRouter::task_accept(std::uint64_t task_id, int width, int height) {
    auto task_check = validate_task(task_id);
    if (!task_check.ok) return task_check;

    std::string error;
    if (!task_->accept(width, height, &error)) {
        const bool technical = !document_->can_resize(width, height, nullptr);
        return result(false,
                      technical ? AgentErrorCode::TechnicalLimit : AgentErrorCode::InvalidState,
                      std::move(error));
    }
    return result(true, AgentErrorCode::None, "Task accepted; canvas created.");
}

AgentCommandResult AgentCommandRouter::task_reject(std::uint64_t task_id, std::string reason) {
    auto task_check = validate_task(task_id);
    if (!task_check.ok) return task_check;

    std::string error;
    if (!task_->reject(std::move(reason), &error)) {
        return result(false, AgentErrorCode::InvalidState, std::move(error));
    }
    return result(true, AgentErrorCode::None, task_->snapshot().status_message);
}

AgentCommandResult AgentCommandRouter::task_abort(std::uint64_t task_id, std::string reason) {
    auto task_check = validate_task(task_id);
    if (!task_check.ok) return task_check;

    std::string error;
    if (!task_->abort(std::move(reason), &error)) {
        return result(false, AgentErrorCode::InvalidState, std::move(error));
    }
    return result(true, AgentErrorCode::None, task_->snapshot().status_message);
}

AgentCommandResult AgentCommandRouter::task_finish(std::uint64_t task_id,
                                                    std::uint64_t expected_revision,
                                                    std::string summary) {
    auto edit_check = validate_editable(task_id, expected_revision);
    if (!edit_check.ok) return edit_check;

    std::string error;
    if (!task_->finish(std::move(summary), &error)) {
        return result(false, AgentErrorCode::InvalidState, std::move(error));
    }
    return result(true, AgentErrorCode::None, task_->snapshot().status_message);
}

AgentCommandResult AgentCommandRouter::edit(std::uint64_t task_id,
                                             std::uint64_t expected_revision,
                                             const std::vector<AgentPixelOp>& operations) {
    auto edit_check = validate_editable(task_id, expected_revision);
    if (!edit_check.ok) return edit_check;
    if (operations.empty()) {
        return result(false, AgentErrorCode::InvalidArgument, "edit requires at least one operation.");
    }

    auto tx = document_->begin_transaction();
    for (const auto& op : operations) {
        bool accepted = false;
        switch (op.kind) {
            case AgentPixelOpKind::SetPixel:
                accepted = tx.set_pixel(op.x, op.y, op.argb);
                break;
            case AgentPixelOpKind::FillRect:
                accepted = tx.fill_rect(op.x, op.y, op.width, op.height, op.argb);
                break;
            case AgentPixelOpKind::HorizontalRun:
                if (op.width <= 0) break;
                accepted = true;
                for (int dx = 0; dx < op.width; ++dx) {
                    if (!tx.set_pixel(op.x + dx, op.y, op.argb)) {
                        accepted = false;
                        break;
                    }
                }
                break;
            case AgentPixelOpKind::VerticalRun:
                if (op.height <= 0) break;
                accepted = true;
                for (int dy = 0; dy < op.height; ++dy) {
                    if (!tx.set_pixel(op.x, op.y + dy, op.argb)) {
                        accepted = false;
                        break;
                    }
                }
                break;
            case AgentPixelOpKind::Line:
                accepted = draw_line(tx, op.x, op.y, op.x2, op.y2, op.argb);
                break;
        }
        if (!accepted) {
            tx.cancel();
            return result(false, AgentErrorCode::EditRejected,
                          "One or more pixel operations were outside the valid canvas or malformed; no changes were applied.");
        }
    }

    const std::size_t pending = tx.pending_changes();
    if (pending == 0) {
        tx.cancel();
        auto out = result(true, AgentErrorCode::None, "Edit was a no-op.");
        out.changed_pixels = 0;
        return out;
    }
    if (!tx.commit()) {
        return result(false, AgentErrorCode::EditRejected, "Edit transaction could not be committed.");
    }

    auto out = result(true, AgentErrorCode::None, "Edit transaction committed.");
    out.changed_pixels = pending;
    return out;
}

AgentCommandResult AgentCommandRouter::history_undo(std::uint64_t task_id,
                                                     std::uint64_t expected_revision) {
    auto edit_check = validate_editable(task_id, expected_revision);
    if (!edit_check.ok) return edit_check;
    if (!document_->undo()) {
        return result(false, AgentErrorCode::EditRejected, "Nothing to undo.");
    }
    return result(true, AgentErrorCode::None, "Undo applied.");
}

AgentCommandResult AgentCommandRouter::history_redo(std::uint64_t task_id,
                                                     std::uint64_t expected_revision) {
    auto edit_check = validate_editable(task_id, expected_revision);
    if (!edit_check.ok) return edit_check;
    if (!document_->redo()) {
        return result(false, AgentErrorCode::EditRejected, "Nothing to redo.");
    }
    return result(true, AgentErrorCode::None, "Redo applied.");
}

AgentRegionObservation AgentCommandRouter::inspect_region(std::uint64_t task_id,
                                                           std::uint64_t expected_revision,
                                                           int x,
                                                           int y,
                                                           int width,
                                                           int height) const {
    AgentRegionObservation out;
    out.task_id = task_id;
    out.revision = document_->revision();

    auto edit_check = validate_editable(task_id, expected_revision);
    if (!edit_check.ok) {
        out.error = edit_check.error;
        out.message = edit_check.message;
        return out;
    }
    if (width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x + width > document_->width() || y + height > document_->height()) {
        out.error = AgentErrorCode::InvalidArgument;
        out.message = "Requested inspection region is outside the canvas.";
        return out;
    }

    out.x = x;
    out.y = y;
    out.width = width;
    out.height = height;
    out.pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            out.pixels.push_back(document_->pixel(px, py));
        }
    }
    out.ok = true;
    out.error = AgentErrorCode::None;
    out.message = "Region observation captured.";
    return out;
}

const char* agent_error_code_name(AgentErrorCode code) noexcept {
    switch (code) {
        case AgentErrorCode::None: return "none";
        case AgentErrorCode::NoActiveTask: return "no_active_task";
        case AgentErrorCode::StaleTask: return "stale_task";
        case AgentErrorCode::InvalidState: return "invalid_state";
        case AgentErrorCode::StaleRevision: return "stale_revision";
        case AgentErrorCode::InvalidArgument: return "invalid_argument";
        case AgentErrorCode::TechnicalLimit: return "technical_limit";
        case AgentErrorCode::EditRejected: return "edit_rejected";
    }
    return "unknown";
}

} // namespace pixelforge
