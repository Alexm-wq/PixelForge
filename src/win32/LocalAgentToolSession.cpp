#include "LocalAgentToolSession.hpp"

#include "MiniJson.hpp"

#include <windows.h>

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
    return call_art_tool(tool, arguments_json);
}

LocalToolResult LocalAgentToolSession::call_art_tool(std::string_view tool, std::string_view arguments_json) {
    if (!running_.load(std::memory_order_relaxed) || !client_input_write_ || !client_output_read_)
        return {false, "{\"ok\":false,\"error\":\"local_tool_session_unavailable\"}"};

    const auto id = next_id_++;
    const std::string args = arguments_json.empty() ? "{}" : std::string(arguments_json);
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
        FlatJsonObject args; std::string ignored;
        parse_flat_json_object(arguments_json, args, ignored);
        const bool resend = args.get_bool("resend_image").value_or(false);
        if (!observation.empty() && !delivered_observations_.insert(observation).second && !resend) {
            result.image_base64.clear();
            result.image_mime.clear();
            result.text = "{\"ok\":true,\"unchanged\":true,\"observation\":" + json_quote(observation) +
                ",\"message\":\"This exact image was already delivered in this session. Use it from context; continue drawing. Set resend_image=true only if you need it delivered again.\"}";
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
    // Keep these definitions aligned with the manual MCP surface. Dynamic tools
    // are used only by automatic App Server generation; manual --mcp remains.
    return R"JSON([
{"type":"function","name":"pixelforge_task","description":"PixelForge task lifecycle. Use get first; accept pixel-art tasks with a chosen canvas size; reject incompatible requests; abort technical blockers after acceptance; finish only when complete.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["begin","get","accept","reject","abort","finish"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"prompt":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"},"reason":{"type":"string"},"summary":{"type":"string"},"content_reference":{"type":"string"},"style_reference":{"type":"string"},"known_task":{"type":"integer"},"known_revision":{"type":"integer"},"known_state":{"type":"string"}},"required":["action"]}},
{"type":"function","name":"pixelforge_edit","description":"Apply one atomic exact-pixel patch. Grammar: P,x,y,c; H,x,y,len,c; V,x,y,len,c; R,x,y,w,h,c; L,x0,y0,x1,y1,c. c is palette index or #AARRGGBB. Always pass current task_id and revision.","inputSchema":{"type":"object","properties":{"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"patch":{"type":"string"}},"required":["task_id","expected_revision","patch"]}},
{"type":"function","name":"pixelforge_view","description":"Observe PixelForge. render returns a lossless canvas PNG; content_reference/style_reference return reference images capped to 262144 delivered pixels; inspect returns compact exact pixels.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["render","content_reference","style_reference","inspect"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"scale":{"type":"integer"},"known_observation":{"type":"string"},"resend_image":{"type":"boolean","description":"Explicitly redeliver an image already seen this session; normally omit."}},"required":["action","task_id"]}},
{"type":"function","name":"pixelforge_palette","description":"Get or replace the compact palette dictionary. Colors are comma-separated AARRGGBB; exact #AARRGGBB colors remain available in patches.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["get","set"]},"colors":{"type":"string"}},"required":["action"]}},
{"type":"function","name":"pixelforge_history","description":"Revision-guarded undo/redo for the accepted task.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["undo","redo"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"}},"required":["action","task_id","expected_revision"]}},
{"type":"function","name":"pixelforge_io","description":"Export the current canvas losslessly to native-resolution PNG.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["export"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"path":{"type":"string"}},"required":["action","task_id","expected_revision","path"]}},
{"type":"function","name":"pixelforge_record","description":"Local-only session recording. Use only when the user explicitly asks to record. start before first canvas mutation, stop after final inspection. Video stays local and is never returned.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","status","stop"]},"task_id":{"type":"integer"},"fps":{"type":"integer","minimum":1,"maximum":60}},"required":["action","task_id"]}}
])JSON";
}

} // namespace pixelforge::win32
