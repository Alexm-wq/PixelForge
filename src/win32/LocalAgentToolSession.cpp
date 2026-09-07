#include "LocalAgentToolSession.hpp"

#include "MiniJson.hpp"
#include "PixelProgram.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace pixelforge::win32 {
namespace {

std::string json_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xf]);
                    out.push_back(hex[c & 0xf]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

bool parse_json_string(std::string_view text, std::size_t& p, std::string& out) {
    if (p >= text.size() || text[p] != '"') return false;
    ++p;
    out.clear();
    while (p < text.size()) {
        const char c = text[p++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (p >= text.size()) return false;
        const char e = text[p++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default:
                // PixelForge-generated JSON does not emit \u escapes in tool
                // text; keep the parser deliberately small and deterministic.
                return false;
        }
    }
    return false;
}

std::string find_string_value(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    std::size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    std::string out;
    return parse_json_string(json, p, out) ? out : std::string{};
}

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t seed = 1469598103934665603ull) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << value;
    return ss.str();
}

std::string observation_for_reference(const ImageData& image, std::string_view kind) {
    if (!image.valid()) return {};
    auto hash = fnv1a(image.bgra.data(), image.bgra.size());
    hash = fnv1a(&image.width, sizeof(image.width), hash);
    hash = fnv1a(&image.height, sizeof(image.height), hash);
    return std::string(kind) + ":" + hex64(hash);
}

std::string render_key(const FlatJsonObject& args) {
    return std::to_string(args.get_i64("task_id").value_or(-1)) + ":" +
        std::to_string(args.get_i64("expected_revision").value_or(-1)) + ":" +
        std::to_string(args.get_i64("x").value_or(0)) + ":" +
        std::to_string(args.get_i64("y").value_or(0)) + ":" +
        std::to_string(args.get_i64("width").value_or(0)) + ":" +
        std::to_string(args.get_i64("height").value_or(0)) + ":" +
        std::to_string(args.get_i64("scale").value_or(8));
}

std::wstring default_recording_path(const std::wstring& directory, std::uint64_t task_id) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wostringstream name;
    name << L"pixelforge-task-" << task_id << L"-"
         << std::setfill(L'0') << std::setw(4) << st.wYear
         << std::setw(2) << st.wMonth << std::setw(2) << st.wDay << L"-"
         << std::setw(2) << st.wHour << std::setw(2) << st.wMinute << std::setw(2) << st.wSecond
         << L".mp4";
    return (std::filesystem::path(directory) / name.str()).wstring();
}

} // namespace

LocalAgentToolSession::~LocalAgentToolSession() {
    stop();
}

bool LocalAgentToolSession::start(AgentMcpBindings bindings,
                                  LocalRecordBindings record_bindings,
                                  std::wstring& error) {
    if (running_.load(std::memory_order_relaxed) || server_thread_.joinable()) {
        error = L"Local PixelForge tool session is already running.";
        return false;
    }
    if (!bindings.document || !bindings.task || !bindings.content_reference || !bindings.style_reference ||
        !bindings.content_path || !bindings.style_path || !bindings.state_mutex) {
        error = L"Local PixelForge artwork bindings are incomplete.";
        return false;
    }
    if (!record_bindings.recorder || !record_bindings.task || !record_bindings.state_mutex || !record_bindings.hwnd) {
        error = L"Local PixelForge recording bindings are incomplete.";
        return false;
    }

    HANDLE server_in = nullptr;
    HANDLE client_in = nullptr;
    HANDLE client_out = nullptr;
    HANDLE server_out = nullptr;
    if (!CreatePipe(&server_in, &client_in, nullptr, 1024u * 1024u) ||
        !CreatePipe(&client_out, &server_out, nullptr, 1024u * 1024u)) {
        if (server_in) CloseHandle(server_in);
        if (client_in) CloseHandle(client_in);
        if (client_out) CloseHandle(client_out);
        if (server_out) CloseHandle(server_out);
        error = L"Could not create the in-process PixelForge tool pipes.";
        return false;
    }

    bindings_ = bindings;
    // Ending the local tool session must never close the real GUI.
    bindings_.close_window_on_exit = false;
    bindings_.input = server_in;
    bindings_.output = server_out;
    record_bindings_ = std::move(record_bindings);
    server_input_read_ = server_in;
    client_input_write_ = client_in;
    client_output_read_ = client_out;
    server_output_write_ = server_out;
    receive_buffer_.clear();
    next_id_ = 1;
    delivered_observations_.clear();
    reference_observations_.clear();
    render_observations_.clear();
    observed_task_ = 0;
    has_observed_revision_ = false;
    running_.store(true, std::memory_order_relaxed);

    server_thread_ = std::thread([this] {
        run_mcp_stdio(bindings_);
        if (server_input_read_) {
            CloseHandle(server_input_read_);
            server_input_read_ = nullptr;
        }
        if (server_output_write_) {
            CloseHandle(server_output_write_);
            server_output_write_ = nullptr;
        }
        running_.store(false, std::memory_order_relaxed);
    });
    return true;
}

void LocalAgentToolSession::stop() {
    std::lock_guard lock(call_mutex_);
    if (client_input_write_) {
        CloseHandle(client_input_write_);
        client_input_write_ = nullptr;
    }
    if (server_thread_.joinable()) server_thread_.join();
    if (client_output_read_) {
        CloseHandle(client_output_read_);
        client_output_read_ = nullptr;
    }
    if (server_input_read_) {
        CloseHandle(server_input_read_);
        server_input_read_ = nullptr;
    }
    if (server_output_write_) {
        CloseHandle(server_output_write_);
        server_output_write_ = nullptr;
    }
    receive_buffer_.clear();
    running_.store(false, std::memory_order_relaxed);
}

LocalToolResult LocalAgentToolSession::call(std::string_view tool, std::string_view arguments_json) {
    std::lock_guard lock(call_mutex_);
    if (tool == "pixelforge_record") return call_record_tool(arguments_json);

    FlatJsonObject args;
    std::string error;
    if (!parse_flat_json_object(arguments_json, args, error))
        return {false, "{\"ok\":false,\"error\":\"invalid_arguments\"}"};

    const std::string requested_tool(tool);
    std::string effective_tool(tool);
    std::string effective(arguments_json);
    PixelProgramResult program_result;

    if (requested_tool == "pixelforge_program") {
        const auto task_id = args.get_i64("task_id");
        const std::string program = args.get("program");
        if (!task_id || *task_id < 0 || program.empty())
            return {false, "{\"ok\":false,\"error\":\"task_id_and_program_required\"}"};

        int width = 0, height = 0;
        std::vector<std::uint32_t> pixels;
        {
            std::lock_guard state_lock(*bindings_.state_mutex);
            const auto snap = bindings_.task->snapshot();
            if (snap.id != static_cast<std::uint64_t>(*task_id))
                return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};
            if (snap.state != TaskState::Accepted)
                return {false, "{\"ok\":false,\"error\":\"invalid_state\"}"};
            width = bindings_.document->width();
            height = bindings_.document->height();
            pixels = bindings_.document->pixels();
        }
        program_result = compile_pixel_program(program, width, height, pixels);
        if (!program_result.ok) {
            return {false, "{\"ok\":false,\"error\":\"invalid_program\",\"message\":" + json_quote(program_result.error) + "}"};
        }

        effective = "{\"task_id\":" + std::to_string(*task_id) + ",\"patch\":" + json_quote(program_result.patch);
        if (args.contains("expected_revision"))
            effective += ",\"expected_revision\":" + std::to_string(args.get_i64("expected_revision").value_or(-1));
        if (args.contains("render_scale"))
            effective += ",\"render_scale\":" + std::to_string(args.get_i64("render_scale").value_or(-1));
        effective += "}";
        effective_tool = "pixelforge_edit";
        if (!parse_flat_json_object(effective, args, error))
            return {false, "{\"ok\":false,\"error\":\"program_compile_internal\"}"};
    }

    const auto task_id = args.get_i64("task_id");
    const bool needs_revision = effective_tool == "pixelforge_edit" || effective_tool == "pixelforge_history" || effective_tool == "pixelforge_io" ||
        (effective_tool == "pixelforge_task" && args.get("action") == "finish") ||
        (effective_tool == "pixelforge_view" && (args.get("action") == "render" || args.get("action") == "inspect"));
    // Use only a revision already returned to this agent, never an unobserved
    // live revision. Concurrent mouse edits still cause the mutation to reject.
    if (needs_revision && !args.contains("expected_revision") && has_observed_revision_ &&
        task_id && *task_id >= 0 && static_cast<std::uint64_t>(*task_id) == observed_task_) {
        const auto end = effective.find_last_of('}');
        effective.insert(end, ",\"expected_revision\":" + std::to_string(observed_revision_));
        parse_flat_json_object(effective, args, error);
    }

    const auto scale = args.get_i64("render_scale");
    if (effective_tool == "pixelforge_edit" && args.contains("render_scale") && (!scale || *scale < 1 || *scale > 32))
        return {false, "{\"ok\":false,\"error\":\"invalid_render_scale\",\"message\":\"Use an integer from 1 to 32; edit not applied.\"}"};

    auto result = call_art_tool(effective_tool, effective);
    FlatJsonObject fields;
    if (parse_flat_json_object(result.text, fields, error)) {
        const auto returned_task = fields.get_i64("task_id");
        const auto revision = fields.get_i64("revision");
        // A stale-revision response contains the authoritative revision. Remember
        // it so the next omitted-revision render/inspect does not fail again.
        if (revision && *revision >= 0 && (returned_task || task_id)) {
            observed_task_ = static_cast<std::uint64_t>(returned_task.value_or(task_id.value_or(0)));
            observed_revision_ = static_cast<std::uint64_t>(*revision);
            has_observed_revision_ = true;
        }
        if (result.success && effective_tool == "pixelforge_task" && args.get("action") == "begin")
            has_observed_revision_ = false;

        if (result.success && effective_tool == "pixelforge_edit" && scale && task_id && revision) {
            const auto view = call_art_tool("pixelforge_view", "{\"action\":\"render\",\"task_id\":" + std::to_string(*task_id) +
                ",\"expected_revision\":" + std::to_string(*revision) + ",\"scale\":" + std::to_string(*scale) + "}");
            // Rendering may fail independently. The edit is already committed;
            // never invite a replay because observation failed.
            FlatJsonObject view_fields;
            std::string ignored;
            parse_flat_json_object(view.text, view_fields, ignored);
            result.text = "{\"ok\":true,\"revision\":" + std::to_string(*revision) +
                ",\"changed_pixels\":" + fields.get("changed_pixels", "0");
            if (requested_tool == "pixelforge_program") {
                result.text += ",\"commands\":" + std::to_string(program_result.commands) +
                    ",\"patch_operations\":" + std::to_string(program_result.patch_operations) +
                    ",\"clipped_writes\":" + std::to_string(program_result.clipped_writes);
            }
            result.text += ",\"render_ok\":" + std::string(view.success ? "true" : "false");
            const auto observation = view_fields.get("observation");
            if (!observation.empty()) result.text += ",\"observation\":" + json_quote(observation);
            result.text += "}";
            result.image_base64 = view.image_base64;
            result.image_mime = view.image_mime;
        } else if (result.success && effective_tool == "pixelforge_edit" && revision) {
            result.text = "{\"ok\":true,\"revision\":" + std::to_string(*revision) +
                ",\"changed_pixels\":" + fields.get("changed_pixels", "0");
            if (requested_tool == "pixelforge_program") {
                result.text += ",\"commands\":" + std::to_string(program_result.commands) +
                    ",\"patch_operations\":" + std::to_string(program_result.patch_operations) +
                    ",\"clipped_writes\":" + std::to_string(program_result.clipped_writes);
            }
            result.text += "}";
        } else if (result.success && effective_tool == "pixelforge_view" && args.get("action") != "inspect") {
            // Paths and crop bookkeeping are useful to the host but add little to
            // model context. Keep only the state needed for subsequent decisions.
            const auto observation = fields.get("observation");
            if (!observation.empty()) {
                result.text = "{\"ok\":true,\"observation\":" + json_quote(observation);
                if (fields.get_bool("unchanged").value_or(false)) result.text += ",\"unchanged\":true";
                if (const auto rev = fields.get_i64("revision")) result.text += ",\"revision\":" + std::to_string(*rev);
                if (const auto w = fields.get_i64("width")) result.text += ",\"width\":" + std::to_string(*w);
                if (const auto h = fields.get_i64("height")) result.text += ",\"height\":" + std::to_string(*h);
                if (const auto s = fields.get_i64("scale")) result.text += ",\"scale\":" + std::to_string(*s);
                result.text += "}";
            }
        } else if (result.success && effective_tool == "pixelforge_task" && args.get("action") != "get") {
            // Task.get carries the user prompt and limits. Subsequent lifecycle
            // calls only need compact state/revision metadata.
            result.text = "{\"ok\":true";
            if (returned_task) result.text += ",\"task_id\":" + std::to_string(*returned_task);
            if (revision) result.text += ",\"revision\":" + std::to_string(*revision);
            const auto state = fields.get("state");
            if (!state.empty()) result.text += ",\"state\":" + json_quote(state);
            if (const auto w = fields.get_i64("canvas_width")) result.text += ",\"width\":" + std::to_string(*w);
            if (const auto h = fields.get_i64("canvas_height")) result.text += ",\"height\":" + std::to_string(*h);
            result.text += "}";
        }
    }
    return result;
}

LocalToolResult LocalAgentToolSession::call_art_tool(std::string_view tool, std::string_view arguments_json) {
    if (!running_.load(std::memory_order_relaxed) || !client_input_write_ || !client_output_read_)
        return {false, "{\"ok\":false,\"error\":\"local_tool_session_unavailable\"}"};

    const auto id = next_id_++;
    std::string args = arguments_json.empty() ? "{}" : std::string(arguments_json);
    FlatJsonObject request_fields;
    std::string parse_error;
    parse_flat_json_object(args, request_fields, parse_error);
    const auto action = request_fields.get("action");
    const bool reference = tool == "pixelforge_view" && (action == "content_reference" || action == "style_reference");

    // Strong reference dedupe: compare the currently loaded in-memory snapshot
    // before asking the MCP server to encode it again. A changed GUI reference
    // gets a new hash and is delivered normally.
    if (reference) {
        const auto known = reference_observations_.find(action);
        if (known != reference_observations_.end()) {
            std::string current_observation;
            {
                std::lock_guard state_lock(*bindings_.state_mutex);
                const auto& image = action == "style_reference" ? *bindings_.style_reference : *bindings_.content_reference;
                current_observation = observation_for_reference(image, action == "style_reference" ? "style" : "content");
            }
            if (!current_observation.empty() && current_observation == known->second) {
                return {true, "{\"ok\":true,\"unchanged\":true,\"observation\":" + json_quote(current_observation) + "}"};
            }
        }
    }

    const bool render = tool == "pixelforge_view" && action == "render";
    const bool resend = render && request_fields.get_bool("resend_image").value_or(false);
    std::string current_render_key;
    if (render) {
        current_render_key = render_key(request_fields);
        const auto known = render_observations_.find(current_render_key);
        if (!resend && known != render_observations_.end()) {
            return {true, "{\"ok\":true,\"unchanged\":true,\"observation\":" + json_quote(known->second) +
                ",\"revision\":" + std::to_string(request_fields.get_i64("expected_revision").value_or(-1)) + "}"};
        }
    }

    const auto known = reference_observations_.find(action);
    if (reference && known != reference_observations_.end() && !request_fields.contains("known_observation"))
        args.insert(args.find_last_of('}'), ",\"known_observation\":" + json_quote(known->second));

    const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":\"tools/call\",\"params\":{\"name\":" + json_quote(tool) +
        ",\"arguments\":" + args + "}}";
    if (!write_line(request))
        return {false, "{\"ok\":false,\"error\":\"local_tool_write_failed\"}"};

    std::string response;
    if (!read_line(response))
        return {false, "{\"ok\":false,\"error\":\"local_tool_read_failed\"}"};

    LocalToolResult result;
    result.success = response.find("\"isError\":true") == std::string::npos &&
                     response.find("\"error\":{") == std::string::npos;
    result.text = find_string_value(response, "text");
    result.image_base64 = find_string_value(response, "data");
    result.image_mime = find_string_value(response, "mimeType");
    if (result.text.empty()) {
        result.success = false;
        result.text = "{\"ok\":false,\"error\":\"malformed_local_tool_response\"}";
    }
    if (result.success && !result.image_base64.empty()) {
        const auto observation = find_string_value(result.text, "observation");
        if (reference && !observation.empty()) reference_observations_[action] = observation;
        if (render && !observation.empty()) render_observations_[current_render_key] = observation;
        if (!observation.empty() && !delivered_observations_.insert(observation).second && !resend) {
            result.image_base64.clear();
            result.image_mime.clear();
            result.text = "{\"ok\":true,\"unchanged\":true,\"observation\":" + json_quote(observation) + "}";
        }
    }
    return result;
}

LocalToolResult LocalAgentToolSession::call_record_tool(std::string_view arguments_json) {
    FlatJsonObject args;
    std::string parse_error;
    if (!parse_flat_json_object(arguments_json.empty() ? "{}" : arguments_json, args, parse_error))
        return {false, "{\"ok\":false,\"error\":\"invalid_arguments\"}"};

    const auto task_id = args.get_i64("task_id");
    if (!task_id || *task_id < 0)
        return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};

    AgentTaskSnapshot snap;
    {
        std::lock_guard lock(*record_bindings_.state_mutex);
        snap = record_bindings_.task->snapshot();
    }
    if (snap.id != static_cast<std::uint64_t>(*task_id))
        return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};

    const std::string action = args.get("action");
    if (action == "start") {
        if (snap.state != TaskState::AwaitingAgentDecision && snap.state != TaskState::Accepted)
            return {false, "{\"ok\":false,\"error\":\"invalid_state\"}"};
        const auto fps_value = args.get_i64("fps").value_or(30);
        if (fps_value < 1 || fps_value > 60)
            return {false, "{\"ok\":false,\"error\":\"invalid_fps\"}"};
        std::error_code ec;
        std::filesystem::create_directories(record_bindings_.recording_directory, ec);
        std::wstring record_error;
        const auto path = default_recording_path(record_bindings_.recording_directory, snap.id);
        if (!record_bindings_.recorder->start(record_bindings_.hwnd, path, static_cast<int>(fps_value), record_error))
            return {false, "{\"ok\":false,\"error\":\"record_start_failed\"}"};
        return {true, "{\"ok\":true,\"recording\":true,\"fps\":" + std::to_string(fps_value) + "}"};
    }
    if (action == "status") {
        return {true, "{\"ok\":true,\"recording\":" +
            std::string(record_bindings_.recorder->active() ? "true" : "false") +
            ",\"frames\":" + std::to_string(record_bindings_.recorder->frames_written()) + "}"};
    }
    if (action == "stop") {
        std::wstring record_error;
        if (!record_bindings_.recorder->stop(record_error))
            return {false, "{\"ok\":false,\"error\":\"record_finalize_failed\"}"};
        return {true, "{\"ok\":true,\"recording\":false,\"saved\":true,\"frames\":" +
            std::to_string(record_bindings_.recorder->frames_written()) + "}"};
    }
    return {false, "{\"ok\":false,\"error\":\"unknown_record_action\"}"};
}

bool LocalAgentToolSession::write_line(std::string_view line) {
    if (!client_input_write_) return false;
    std::string framed(line);
    framed.push_back('\n');
    DWORD offset = 0;
    while (offset < framed.size()) {
        DWORD written = 0;
        if (!WriteFile(client_input_write_, framed.data() + offset,
                       static_cast<DWORD>(framed.size() - offset), &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

bool LocalAgentToolSession::read_line(std::string& line) {
    line.clear();
    for (;;) {
        const auto newline = receive_buffer_.find('\n');
        if (newline != std::string::npos) {
            line.assign(receive_buffer_.data(), newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            receive_buffer_.erase(0, newline + 1);
            return true;
        }
        if (!client_output_read_) return false;
        char buffer[16384];
        DWORD read = 0;
        if (!ReadFile(client_output_read_, buffer, sizeof(buffer), &read, nullptr) || read == 0) return false;
        receive_buffer_.append(buffer, buffer + read);
        if (receive_buffer_.size() > 32u * 1024u * 1024u) return false;
    }
}

std::string pixelforge_dynamic_tools_json() {
    // Automatic generation gets one high-level raster-program tool in addition
    // to the exact manual patch surface. This trades tool turns for local work.
    return R"JSON([
{"type":"function","name":"pixelforge_task","description":"PixelForge task lifecycle. Use get first; accept with canvas size; finish only when complete.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["begin","get","accept","reject","abort","finish"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"prompt":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"},"reason":{"type":"string"},"summary":{"type":"string"},"content_reference":{"type":"string"},"style_reference":{"type":"string"},"known_task":{"type":"integer"},"known_revision":{"type":"integer"},"known_state":{"type":"string"}},"required":["action"]}},
{"type":"function","name":"pixelforge_program","description":"Preferred automatic drawing tool. Run one stateful raster program against the current canvas, then commit its exact diff atomically. Geometry is clipped locally, so edge overshoot does not waste a model turn. Commands, one per line/semicolon: CLEAR c; P x y c; H x y len c; V x y len c; R x y w h c; BOX x y w h c; L x0 y0 x1 y1 c; ELLIPSE/FELLIPSE cx cy rx ry c; CIRCLE/FCIRCLE cx cy r c; Q x0 y0 cx cy x1 y1 c; C x0 y0 c1x c1y c2x c2y x1 y1 c; POLY/FPOLY c x0 y0 x1 y1 x2 y2 [...]; COPY/FLIPX/FLIPY/FLIPXY sx sy w h dx dy. Colors are palette index or #AARRGGBB. Use render_scale to inspect only after successful commit.","inputSchema":{"type":"object","properties":{"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"program":{"type":"string"},"render_scale":{"type":"integer","minimum":1,"maximum":32}},"required":["task_id","program"]}},
{"type":"function","name":"pixelforge_edit","description":"Exact cleanup patch only. Grammar: P,x,y,c; H,x,y,len,c; V,x,y,len,c; R,x,y,w,h,c; L,x0,y0,x1,y1,c. Use program for broad drawing. render_scale combines commit+observation.","inputSchema":{"type":"object","properties":{"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"patch":{"type":"string"},"render_scale":{"type":"integer","minimum":1,"maximum":32}},"required":["task_id","patch"]}},
{"type":"function","name":"pixelforge_view","description":"Observe only when visual judgment is needed. render returns canvas PNG; content_reference/style_reference return a capped reference once; inspect returns exact RLE pixels.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["render","content_reference","style_reference","inspect"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"scale":{"type":"integer"},"known_observation":{"type":"string"},"resend_image":{"type":"boolean"}},"required":["action","task_id"]}},
{"type":"function","name":"pixelforge_palette","description":"Get/set compact palette dictionary as comma-separated AARRGGBB colors.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["get","set"]},"colors":{"type":"string"}},"required":["action"]}},
{"type":"function","name":"pixelforge_history","description":"Revision-guarded undo/redo.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["undo","redo"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"}},"required":["action","task_id"]}},
{"type":"function","name":"pixelforge_io","description":"Export current canvas losslessly to native PNG.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["export"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"path":{"type":"string"}},"required":["action","task_id","path"]}},
{"type":"function","name":"pixelforge_record","description":"Local session recording; use only when explicitly requested.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","status","stop"]},"task_id":{"type":"integer"},"fps":{"type":"integer","minimum":1,"maximum":60}},"required":["action","task_id"]}}
])JSON";
}

} // namespace pixelforge::win32
