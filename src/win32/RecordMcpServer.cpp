#include "RecordMcpServer.hpp"

#include "MiniJson.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace pixelforge::win32 {

namespace {

constexpr DWORD kPipeBufferBytes = 256u * 1024u;
constexpr DWORD kBridgeChunkBytes = 64u * 1024u;

std::string json_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xf]);
                    out.push_back(hex[c & 0xf]);
                } else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

void skip_ws(std::string_view text, std::size_t& p) {
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
}

bool parse_string(std::string_view text, std::size_t& p, std::string& out) {
    if (p >= text.size() || text[p] != '"') return false;
    ++p;
    out.clear();
    while (p < text.size()) {
        const char c = text[p++];
        if (c == '"') return true;
        if (c != '\\') { out.push_back(c); continue; }
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
            default: return false;
        }
    }
    return false;
}

bool skip_value(std::string_view text, std::size_t& p) {
    skip_ws(text, p);
    if (p >= text.size()) return false;
    if (text[p] == '"') {
        std::string ignored;
        return parse_string(text, p, ignored);
    }
    if (text[p] == '{' || text[p] == '[') {
        const char open = text[p];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        for (; p < text.size(); ++p) {
            const char c = text[p];
            if (in_string) {
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; continue; }
            if (c == open) ++depth;
            else if (c == close && --depth == 0) { ++p; return true; }
        }
        return false;
    }
    while (p < text.size() && text[p] != ',' && text[p] != '}' && text[p] != ']') ++p;
    return true;
}

bool extract_member_raw(std::string_view object, std::string_view wanted, std::string& raw) {
    std::size_t p = 0;
    skip_ws(object, p);
    if (p >= object.size() || object[p] != '{') return false;
    ++p;
    for (;;) {
        skip_ws(object, p);
        if (p >= object.size() || object[p] == '}') return false;
        std::string key;
        if (!parse_string(object, p, key)) return false;
        skip_ws(object, p);
        if (p >= object.size() || object[p] != ':') return false;
        ++p;
        skip_ws(object, p);
        const std::size_t start = p;
        if (!skip_value(object, p)) return false;
        std::size_t end = p;
        while (end > start && (object[end - 1] == ' ' || object[end - 1] == '\t' || object[end - 1] == '\r' || object[end - 1] == '\n')) --end;
        if (key == wanted) {
            raw.assign(object.substr(start, end - start));
            return true;
        }
        skip_ws(object, p);
        if (p < object.size() && object[p] == ',') ++p;
    }
}

std::string decode_scalar_string(std::string_view raw) {
    std::size_t p = 0;
    skip_ws(raw, p);
    std::string out;
    return parse_string(raw, p, out) ? out : std::string{};
}

bool read_line(HANDLE input, std::string& line) {
    line.clear();
    char c = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, &c, 1, &read, nullptr) || read == 0) return !line.empty();
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
        if (line.size() > 1024u * 1024u) return false;
    }
}

bool write_line(HANDLE output, std::string_view line) {
    std::string framed(line);
    framed.push_back('\n');
    DWORD offset = 0;
    while (offset < framed.size()) {
        DWORD written = 0;
        if (!WriteFile(output, framed.data() + offset, static_cast<DWORD>(framed.size() - offset), &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

std::string make_response(std::string_view id, std::string_view result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"result\":" + std::string(result) + "}";
}

std::string make_error(std::string_view id, int code, std::string_view message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + (id.empty() ? std::string("null") : std::string(id)) +
           ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + json_quote(message) + "}}";
}

std::string tool_result(std::string_view text, bool error = false) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_quote(text) + "}],\"isError\":" +
           (error ? "true" : "false") + "}";
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

bool copy_bytes(HANDLE input, HANDLE output, std::atomic_bool& stopped) {
    std::array<char, kBridgeChunkBytes> buffer{};
    while (!stopped.load(std::memory_order_relaxed)) {
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) return true;
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(output, buffer.data() + offset, read - offset, &written, nullptr) || written == 0) return false;
            offset += written;
        }
    }
    return true;
}

std::string record_tool_call(const FlatJsonObject& args, const RecordMcpBindings& bindings, bool& error_flag) {
    error_flag = false;
    const std::string action = args.get("action");
    const auto task_id = args.get_i64("task_id");
    if (!task_id || *task_id < 0) {
        error_flag = true;
        return "{\"ok\":false,\"error\":\"task_id_required\"}";
    }

    AgentTaskSnapshot snap;
    {
        std::lock_guard lock(*bindings.state_mutex);
        snap = bindings.task->snapshot();
    }
    if (snap.id != static_cast<std::uint64_t>(*task_id)) {
        error_flag = true;
        return "{\"ok\":false,\"error\":\"stale_task\"}";
    }

    if (action == "start") {
        if (snap.state != TaskState::AwaitingAgentDecision && snap.state != TaskState::Accepted) {
            error_flag = true;
            return "{\"ok\":false,\"error\":\"invalid_state\"}";
        }
        const int fps = static_cast<int>(args.get_i64("fps").value_or(30));
        std::wstring recording_error;
        const auto path = default_recording_path(bindings.recording_directory, snap.id);
        if (!bindings.recorder->start(bindings.hwnd, path, fps, recording_error)) {
            error_flag = true;
            return "{\"ok\":false,\"error\":\"record_start_failed\"}";
        }
        return "{\"ok\":true,\"recording\":true,\"fps\":" + std::to_string(fps) + "}";
    }

    if (action == "status") {
        return "{\"ok\":true,\"recording\":" + std::string(bindings.recorder->active() ? "true" : "false") +
               ",\"frames\":" + std::to_string(bindings.recorder->frames_written()) + "}";
    }

    if (action == "stop") {
        std::wstring recording_error;
        const bool ok = bindings.recorder->stop(recording_error);
        if (!ok) {
            error_flag = true;
            return "{\"ok\":false,\"error\":\"record_finalize_failed\"}";
        }
        return "{\"ok\":true,\"recording\":false,\"saved\":true,\"frames\":" +
               std::to_string(bindings.recorder->frames_written()) + "}";
    }

    error_flag = true;
    return "{\"ok\":false,\"error\":\"unknown_record_action\"}";
}

void serve_record_mcp(HANDLE input, HANDLE output, const RecordMcpBindings& bindings) {
    const std::string tools =
        "{\"tools\":[{\"name\":\"pixelforge_record\",\"description\":\"Local-only PixelForge session recording. If and only if the user prompt asks for the work session to be recorded, call start before the first canvas mutation and stop after final visual inspection. The MP4 stays local and is never returned to the agent.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"status\",\"stop\"]},\"task_id\":{\"type\":\"integer\"},\"fps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":60}},\"required\":[\"action\",\"task_id\"]}}]}";

    std::string line;
    while (read_line(input, line)) {
        if (line.empty()) continue;
        std::string method_raw, id_raw, params_raw;
        if (!extract_member_raw(line, "method", method_raw)) {
            write_line(output, make_error("null", -32600, "Invalid request."));
            continue;
        }
        const std::string method = decode_scalar_string(method_raw);
        extract_member_raw(line, "id", id_raw);
        extract_member_raw(line, "params", params_raw);
        const bool notification = id_raw.empty();

        if (method == "notifications/initialized" || method == "notifications/cancelled") continue;
        if (method == "initialize") {
            if (!notification) {
                const std::string result = "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"PixelForge Recording\",\"version\":\"0.3.0\"}}";
                write_line(output, make_response(id_raw, result));
            }
            continue;
        }
        if (method == "ping") {
            if (!notification) write_line(output, make_response(id_raw, "{}"));
            continue;
        }
        if (method == "tools/list") {
            if (!notification) write_line(output, make_response(id_raw, tools));
            continue;
        }
        if (method == "tools/call") {
            if (notification) continue;
            std::string name_raw, args_raw;
            if (!extract_member_raw(params_raw, "name", name_raw)) {
                write_line(output, make_error(id_raw, -32602, "tools/call requires name."));
                continue;
            }
            const std::string name = decode_scalar_string(name_raw);
            if (name != "pixelforge_record") {
                write_line(output, make_response(id_raw, tool_result("{\"ok\":false,\"error\":\"unknown_tool\"}", true)));
                continue;
            }
            if (!extract_member_raw(params_raw, "arguments", args_raw)) args_raw = "{}";
            FlatJsonObject args;
            std::string parse_error;
            if (!parse_flat_json_object(args_raw, args, parse_error)) {
                write_line(output, make_response(id_raw, tool_result("{\"ok\":false,\"error\":\"invalid_arguments\"}", true)));
                continue;
            }
            bool call_error = false;
            const auto result = record_tool_call(args, bindings, call_error);
            write_line(output, make_response(id_raw, tool_result(result, call_error)));
            continue;
        }
        if (!notification) write_line(output, make_error(id_raw, -32601, "Method not found."));
    }
}

} // namespace

std::wstring make_record_pipe_name() {
    return L"\\\\.\\pipe\\PixelForge.Record." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(GetTickCount64());
}

RecordMcpHost::~RecordMcpHost() {
    stop();
}

bool RecordMcpHost::start(std::wstring pipe_name, RecordMcpBindings bindings, std::wstring& error) {
    if (thread_.joinable()) {
        error = L"PixelForge recording MCP host is already running.";
        return false;
    }
    if (pipe_name.empty() || !bindings.recorder || !bindings.task || !bindings.state_mutex || !bindings.hwnd) {
        error = L"PixelForge recording MCP host bindings are incomplete.";
        return false;
    }
    pipe_name_ = std::move(pipe_name);
    bindings_ = std::move(bindings);
    stop_requested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { serve(); });
    return true;
}

void RecordMcpHost::stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (!thread_.joinable()) return;
    HANDLE wake = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    CancelSynchronousIo(thread_.native_handle());
    thread_.join();
}

void RecordMcpHost::serve() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        HANDLE pipe = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                       kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected || stop_requested_.load(std::memory_order_relaxed)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        serve_record_mcp(pipe, pipe, bindings_);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

int run_record_bridge_stdio(const std::wstring& pipe_name) {
    if (pipe_name.empty()) return 2;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 60; ++attempt) {
        pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) return 3;
        WaitNamedPipeW(pipe_name.c_str(), 250);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 4;

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        return 5;
    }

    std::atomic_bool stopped{false};
    std::thread inbound([&] {
        copy_bytes(input, pipe, stopped);
        stopped.store(true, std::memory_order_relaxed);
        CancelIoEx(pipe, nullptr);
    });
    const bool output_ok = copy_bytes(pipe, output, stopped);
    stopped.store(true, std::memory_order_relaxed);
    CancelIoEx(pipe, nullptr);
    CancelSynchronousIo(inbound.native_handle());
    if (inbound.joinable()) inbound.join();
    CloseHandle(pipe);
    return output_ok ? 0 : 6;
}

} // namespace pixelforge::win32
