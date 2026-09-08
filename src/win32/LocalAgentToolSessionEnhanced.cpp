#include "LocalAgentToolSession.hpp"

#define call base_call
#define pixelforge_dynamic_tools_json base_pixelforge_dynamic_tools_json
#include "LocalAgentToolSession.cpp"
#undef pixelforge_dynamic_tools_json
#undef call

#include "AgentInteractionUi.hpp"
#include "ImageIO.hpp"
#include "MiniJson.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace pixelforge::win32 {
namespace {

std::string q(std::string_view text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(static_cast<char>(c));
    }
    out += '"';
    return out;
}

std::uint32_t argb_at(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[3]) << 24) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[0]);
}

std::vector<std::uint32_t> argb_pixels(const ImageData& image) {
    std::vector<std::uint32_t> out;
    if (!image.valid()) return out;
    const auto count = static_cast<std::size_t>(image.width) * image.height;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = argb_at(image.bgra.data() + i * 4u);
    return out;
}

bool seed_document(PixelDocument& document, const ImageData& image, std::string& error) {
    if (!image.valid()) { error = "Reference image is unavailable."; return false; }
    if (document.width() != image.width || document.height() != image.height) {
        error = "Exact reference seeding requires matching canvas dimensions.";
        return false;
    }
    const auto pixels = argb_pixels(image);
    auto tx = document.begin_transaction();
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            if (!tx.set_pixel(x, y, pixels[static_cast<std::size_t>(y) * image.width + x])) {
                error = "Could not seed reference pixels.";
                return false;
            }
    tx.commit();
    return true;
}

std::string palette_for(const ImageData& image, std::size_t max_colors, std::size_t& unique) {
    std::map<std::uint32_t, std::uint64_t> counts;
    const auto count = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t i = 0; i < count; ++i) ++counts[argb_at(image.bgra.data() + i * 4u)];
    unique = counts.size();
    std::vector<std::pair<std::uint32_t, std::uint64_t>> ranked(counts.begin(), counts.end());
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (ranked.size() > max_colors) ranked.resize(max_colors);
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        if (i) out << ',';
        out << std::setw(8) << ranked[i].first;
    }
    return out.str();
}

bool get_reference(const AgentMcpBindings& bindings, std::string reference, ImageData& image) {
    std::transform(reference.begin(), reference.end(), reference.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (reference == "source") { std::wstring path; return agent_interaction_source_snapshot(image, path); }
    if (reference == "content" && bindings.content_reference && bindings.content_reference->valid()) { image = *bindings.content_reference; return true; }
    if (reference == "style" && bindings.style_reference && bindings.style_reference->valid()) { image = *bindings.style_reference; return true; }
    return false;
}

bool add_member(std::string& json, std::string member) {
    const auto end = json.find_last_of('}');
    if (end == std::string::npos) return false;
    json.insert(end, std::move(member));
    return true;
}

bool flag_false(const FlatJsonObject& args, std::string_view key) {
    return args.contains(key) && !args.get_bool(key).value_or(true);
}

} // namespace

LocalToolResult LocalAgentToolSession::call(std::string_view tool, std::string_view arguments_json) {
    agent_interaction_bind_task(bindings_.task, bindings_.state_mutex, bindings_.hwnd);

    FlatJsonObject args;
    std::string parse_error;
    if (!parse_flat_json_object(arguments_json.empty() ? "{}" : arguments_json, args, parse_error))
        return {false, "{\"ok\":false,\"error\":\"invalid_arguments\"}"};

    if (tool == "pixelforge_dialog") {
        const auto task_id = args.get_i64("task_id");
        if (!task_id) return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};
        const std::string reason = args.get("reason"), question = args.get("question"), suggestions = args.get("suggestions");
        std::string state_error;
        {
            std::lock_guard lock(*bindings_.state_mutex);
            if (bindings_.task->snapshot().id != static_cast<std::uint64_t>(*task_id))
                return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};
            if (!bindings_.task->request_user_input(reason, question, suggestions, &state_error))
                return {false, "{\"ok\":false,\"error\":\"dialog_rejected\",\"message\":" + q(state_error) + "}"};
        }
        std::string answer;
        bool cancelled = false;
        std::wstring ui_error;
        if (!agent_interaction_ask_user(reason, question, suggestions, answer, cancelled, ui_error)) {
            std::lock_guard lock(*bindings_.state_mutex);
            bindings_.task->user_answer_input("PixelForge could not open the user dialog.", nullptr);
            return {false, "{\"ok\":false,\"error\":\"dialog_ui_failed\"}"};
        }
        {
            std::lock_guard lock(*bindings_.state_mutex);
            bindings_.task->user_answer_input(cancelled ? "User dismissed the popup without an answer." : answer, nullptr);
        }
        return {true, "{\"ok\":true,\"cancelled\":" + std::string(cancelled ? "true" : "false") +
                      ",\"answer\":" + q(answer) +
                      ",\"message\":\"Continue this same turn using the user's answer.\"}"};
    }

    if (tool == "pixelforge_reference") {
        const auto task_id = args.get_i64("task_id");
        if (!task_id) return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};
        {
            std::lock_guard lock(*bindings_.state_mutex);
            if (bindings_.task->snapshot().id != static_cast<std::uint64_t>(*task_id))
                return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};
        }
        const std::string reference = args.get("reference"), action = args.get("action");
        ImageData image;
        if (!get_reference(bindings_, reference, image))
            return {false, "{\"ok\":false,\"error\":\"reference_unavailable\"}"};
        if (action == "palette") {
            const auto n = static_cast<std::size_t>(std::clamp<std::int64_t>(args.get_i64("max_colors").value_or(20), 1, 256));
            std::size_t unique = 0;
            const auto colors = palette_for(image, n, unique);
            return {true, "{\"ok\":true,\"reference\":" + q(reference) + ",\"width\":" + std::to_string(image.width) +
                          ",\"height\":" + std::to_string(image.height) + ",\"unique_colors\":" + std::to_string(unique) +
                          ",\"colors\":" + q(colors) + "}"};
        }
        if (action == "seed") {
            std::string error;
            {
                std::lock_guard lock(*bindings_.state_mutex);
                if (bindings_.task->snapshot().state != TaskState::Accepted)
                    return {false, "{\"ok\":false,\"error\":\"task_not_accepted\"}"};
                if (!seed_document(*bindings_.document, image, error))
                    return {false, "{\"ok\":false,\"error\":\"seed_failed\",\"message\":" + q(error) + "}"};
                observed_revision_ = bindings_.document->revision();
                has_observed_revision_ = true;
            }
            return {true, "{\"ok\":true,\"seeded_from\":" + q(reference) + ",\"revision\":" + std::to_string(observed_revision_) + "}"};
        }
        if (action == "view") {
            if (reference == "content") return base_call("pixelforge_view", "{\"action\":\"content_reference\",\"task_id\":" + std::to_string(*task_id) + "}");
            if (reference == "style") return base_call("pixelforge_view", "{\"action\":\"style_reference\",\"task_id\":" + std::to_string(*task_id) + "}");
            return {true, "{\"ok\":true,\"reference\":\"source\",\"width\":" + std::to_string(image.width) +
                          ",\"height\":" + std::to_string(image.height) +
                          ",\"message\":\"Source is already available to PixelForge for exact seeding; use palette for colors or seed to copy it mechanically.\"}"};
        }
        return {false, "{\"ok\":false,\"error\":\"unknown_reference_action\"}"};
    }

    if (tool == "pixelforge_task" && args.get("action") == "get") {
        auto result = base_call(tool, arguments_json);
        if (!result.success) return result;
        ImageData source; std::wstring path;
        if (agent_interaction_source_snapshot(source, path))
            add_member(result.text, ",\"source_present\":true,\"source_width\":" + std::to_string(source.width) +
                                    ",\"source_height\":" + std::to_string(source.height) +
                                    ",\"source_semantics\":\"editable image; seed and modify it unless instructed otherwise\"");
        else add_member(result.text, ",\"source_present\":false");
        return result;
    }

    if (tool == "pixelforge_task" && args.get("action") == "accept") {
        AgentTaskSnapshot before;
        { std::lock_guard lock(*bindings_.state_mutex); before = bindings_.task->snapshot(); }
        ImageData source; std::wstring path;
        const bool has_source = agent_interaction_source_snapshot(source, path);
        const bool fresh = before.state == TaskState::AwaitingAgentDecision && !before.continuation_pending;
        const bool seed_source = has_source && fresh && !flag_false(args, "seed_from_source");
        std::string forwarded(arguments_json);
        if (seed_source) {
            const auto requested_w = args.get_i64("width");
            const auto requested_h = args.get_i64("height");
            if ((requested_w && *requested_w != source.width) || (requested_h && *requested_h != source.height))
                return {false, "{\"ok\":false,\"error\":\"source_size_mismatch\",\"message\":\"Exact Source editing starts at Source dimensions. Ask the user with pixelforge_dialog or explicitly set seed_from_source=false if a different size is intentional.\"}"};
            auto end = forwarded.find_last_of('}');
            if (end != std::string::npos && !args.contains("width")) forwarded.insert(end, ",\"width\":" + std::to_string(source.width));
            end = forwarded.find_last_of('}');
            if (end != std::string::npos && !args.contains("height")) forwarded.insert(end, ",\"height\":" + std::to_string(source.height));
        }
        auto result = base_call(tool, forwarded);
        if (!result.success) return result;
        if (seed_source) {
            std::string error;
            std::lock_guard lock(*bindings_.state_mutex);
            if (!seed_document(*bindings_.document, source, error))
                return {false, "{\"ok\":false,\"error\":\"source_seed_failed\",\"message\":" + q(error) + "}"};
            observed_revision_ = bindings_.document->revision();
            has_observed_revision_ = true;
            add_member(result.text, ",\"seeded_from\":\"source\",\"canvas_blank\":false,\"actual_revision\":" + std::to_string(observed_revision_));
        } else if (fresh) add_member(result.text, ",\"canvas_blank\":true");
        return result;
    }

    if (tool == "pixelforge_pack" && args.get("action") == "create") {
        ImageData source; std::wstring path;
        const bool seed_source = agent_interaction_source_snapshot(source, path) && !flag_false(args, "seed_from_source");
        auto result = base_call(tool, arguments_json);
        if (!result.success || !seed_source) return result;
        std::string error;
        {
            std::lock_guard lock(*bindings_.state_mutex);
            if (bindings_.document->width() != source.width || bindings_.document->height() != source.height)
                return {false, "{\"ok\":false,\"error\":\"source_size_mismatch\",\"message\":\"The first pack canvas must match Source dimensions when Source seeding is enabled. Ask the user or set seed_from_source=false if a different layout is intentional.\"}"};
            if (!seed_document(*bindings_.document, source, error))
                return {false, "{\"ok\":false,\"error\":\"source_seed_failed\",\"message\":" + q(error) + "}"};
        }
        add_member(result.text, ",\"primary_seeded_from\":\"source\"");
        return result;
    }

    return base_call(tool, arguments_json);
}

std::string pixelforge_dynamic_tools_json() {
    std::string tools = base_pixelforge_dynamic_tools_json();
    const auto end = tools.find_last_of(']');
    if (end == std::string::npos) return tools;
    tools.insert(end, R"JSON(,
{"type":"function","name":"pixelforge_reference","description":"Inspect or mechanically reuse a supplied reference without shell/Python. source is the editable image; content is a visual target; style is style guidance. palette computes colors locally; seed copies exact pixels into an accepted same-size canvas.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["view","palette","seed"]},"task_id":{"type":"integer"},"reference":{"type":"string","enum":["source","content","style"]},"max_colors":{"type":"integer","minimum":1,"maximum":256}},"required":["action","task_id","reference"]}},
{"type":"function","name":"pixelforge_dialog","description":"Ask the user a concrete question in PixelForge and receive the answer in this same turn. YOU decide whether a pause is warranted. Use it only for a material ambiguity, impossible/unsupported instruction, or consequential choice; explain exactly what is blocked and provide up to three alternatives separated by |. Do not ask about routine artistic choices you can decide yourself.","inputSchema":{"type":"object","properties":{"task_id":{"type":"integer"},"reason":{"type":"string"},"question":{"type":"string"},"suggestions":{"type":"string"}},"required":["task_id","reason","question"]}}
)JSON");
    return tools;
}

} // namespace pixelforge::win32
