#pragma once

#include "AgentTask.hpp"
#include "PixelDocument.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pixelforge {

enum class AgentErrorCode {
    None,
    NoActiveTask,
    StaleTask,
    InvalidState,
    StaleRevision,
    InvalidArgument,
    TechnicalLimit,
    EditRejected
};

enum class AgentPixelOpKind {
    SetPixel,
    FillRect,
    HorizontalRun,
    VerticalRun,
    Line
};

struct AgentPixelOp {
    AgentPixelOpKind kind = AgentPixelOpKind::SetPixel;
    int x = 0;
    int y = 0;
    int x2 = 0;
    int y2 = 0;
    int width = 1;
    int height = 1;
    std::uint32_t argb = 0;
};

struct AgentCommandResult {
    bool ok = false;
    AgentErrorCode error = AgentErrorCode::None;
    std::string message;
    AgentTaskSnapshot task;
    std::uint64_t revision = 0;
    std::size_t changed_pixels = 0;
};

struct AgentRegionObservation {
    bool ok = false;
    AgentErrorCode error = AgentErrorCode::None;
    std::string message;
    std::uint64_t task_id = 0;
    std::uint64_t revision = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
};

// Stable command surface used by the future MCP/stdio adapter. The GUI must not
// semantically interpret the user's prompt; it only presents AgentTaskController state.
class AgentCommandRouter {
public:
    AgentCommandRouter(PixelDocument& document, AgentTaskController& task);

    [[nodiscard]] AgentCommandResult task_get() const;
    AgentCommandResult task_accept(std::uint64_t task_id, int width, int height);
    AgentCommandResult task_reject(std::uint64_t task_id, std::string reason);
    AgentCommandResult task_abort(std::uint64_t task_id, std::string reason);
    AgentCommandResult task_finish(std::uint64_t task_id, std::uint64_t expected_revision, std::string summary);

    AgentCommandResult edit(std::uint64_t task_id,
                            std::uint64_t expected_revision,
                            const std::vector<AgentPixelOp>& operations);
    AgentCommandResult history_undo(std::uint64_t task_id, std::uint64_t expected_revision);
    AgentCommandResult history_redo(std::uint64_t task_id, std::uint64_t expected_revision);

    [[nodiscard]] AgentRegionObservation inspect_region(std::uint64_t task_id,
                                                        std::uint64_t expected_revision,
                                                        int x,
                                                        int y,
                                                        int width,
                                                        int height) const;

private:
    [[nodiscard]] AgentCommandResult result(bool ok,
                                            AgentErrorCode code = AgentErrorCode::None,
                                            std::string message = {}) const;
    [[nodiscard]] AgentCommandResult validate_task(std::uint64_t task_id) const;
    [[nodiscard]] AgentCommandResult validate_editable(std::uint64_t task_id,
                                                        std::uint64_t expected_revision) const;

    PixelDocument* document_ = nullptr;
    AgentTaskController* task_ = nullptr;
};

const char* agent_error_code_name(AgentErrorCode code) noexcept;

} // namespace pixelforge
