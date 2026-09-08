#include "LocalAgentToolSession.hpp"

#include "MiniJson.hpp"
#include "PackWorkspaceUi.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

std::vector<std::string> split_restore(std::string_view text, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            break;
        }
        out.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::vector<PackUiCanvasInfo> parse_restore_summary(std::string_view summary) {
    std::vector<PackUiCanvasInfo> out;
    for (const auto& row : split_restore(summary, '|')) {
        if (row.empty()) continue;
        const auto fields = split_restore(row, ',');
        if (fields.size() != 5) continue;
        PackUiCanvasInfo canvas;
        canvas.name = fields[0];
        canvas.group = fields[1];
        try {
            canvas.width = std::stoi(fields[2]);
            canvas.height = std::stoi(fields[3]);
            canvas.frame = std::stoi(fields[4]);
        } catch (...) {
            continue;
        }
        out.push_back(std::move(canvas));
    }
    return out;
}

} // namespace

LocalToolResult LocalAgentToolSession::restore_project_pack(
    std::string_view create_arguments_json,
    const std::vector<LocalPackRestoreCanvas>& canvases,
    std::wstring project_directory) {
    FlatJsonObject create_args;
    std::string parse_error;
    if (!parse_flat_json_object(create_arguments_json, create_args, parse_error))
        return {false, "{\"ok\":false,\"error\":\"invalid_restore_create_arguments\"}"};

    const auto task_id_value = create_args.get_i64("task_id");
    if (!task_id_value || *task_id_value <= 0)
        return {false, "{\"ok\":false,\"error\":\"restore_task_id_required\"}"};
    const auto task_id = static_cast<std::uint64_t>(*task_id_value);

    // Deliberately bypass LocalAgentToolSession::call here. The ordinary wrapper
    // persists and republishes after every edit, which is correct for live Astra
    // work but catastrophically expensive when reconstructing dozens of saved
    // canvases. Project restore builds the complete in-memory pack first.
    auto created = base_call("pixelforge_pack", create_arguments_json);
    if (!created.success) return created;

    for (const auto& canvas : canvases) {
        for (const auto& patch : canvas.patches) {
            const std::string request =
                "{\"action\":\"edit\",\"task_id\":" + std::to_string(task_id) +
                ",\"canvas\":" + json_quote(canvas.name) +
                ",\"patch\":" + json_quote(patch) + "}";
            auto edited = base_call("pixelforge_pack", request);
            if (!edited.success) {
                return {false,
                    "{\"ok\":false,\"error\":\"project_restore_canvas_failed\",\"canvas\":" +
                    json_quote(canvas.name) + ",\"cause\":" + json_quote(edited.text) + "}"};
            }
        }
    }

    auto listed = base_call("pixelforge_pack",
        "{\"action\":\"list\",\"task_id\":" + std::to_string(task_id) + "}");
    if (!listed.success) return listed;

    FlatJsonObject fields;
    if (!parse_flat_json_object(listed.text, fields, parse_error))
        return {false, "{\"ok\":false,\"error\":\"restore_list_parse_failed\"}"};

    const auto metadata = parse_restore_summary(fields.get("canvases"));
    const auto revision = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, fields.get_i64("revision").value_or(0)));

    // One publication after the complete project exists. There are no partial
    // frame snapshots for the Canvases/Animation tabs to reveal one by one.
    pack_workspace_ui_publish(bindings_.hwnd, task_id, revision,
                              std::move(project_directory), metadata);

    auto text = listed.text;
    const auto end = text.find_last_of('}');
    if (end != std::string::npos)
        text.insert(end, ",\"restored_project\":true,\"bulk_restore\":true");
    return {true, std::move(text)};
}

} // namespace pixelforge::win32
