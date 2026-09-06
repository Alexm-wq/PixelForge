#include "CodexAppClient.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pixelforge::win32 {

namespace {

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

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
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == open) ++depth;
            else if (c == close && --depth == 0) {
                ++p;
                return true;
            }
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

std::optional<std::int64_t> parse_i64(std::string_view raw) {
    std::size_t p = 0;
    skip_ws(raw, p);
    std::int64_t value = 0;
    const char* first = raw.data() + p;
    const char* last = raw.data() + raw.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr == first) return std::nullopt;
    return value;
}

std::wstring quote_windows_arg(std::wstring_view arg) {
    if (arg.empty()) return L"\"\"";
    const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(arg);

    std::wstring out;
    out.push_back(L'"');
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
            continue;
        }
        out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring toml_basic_string(std::wstring_view value) {
    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\"') out += L"\\\"";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else out.push_back(c);
    }
    out.push_back(L'\"');
    return out;
}

std::wstring get_env(std::wstring_view name) {
    const DWORD needed = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
    if (!needed) return {};
    std::wstring out(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(std::wstring(name).c_str(), out.data(), needed);
    if (!written) return {};
    out.resize(written);
    return out;
}

std::wstring search_path(std::wstring_view file) {
    DWORD needed = SearchPathW(nullptr, std::wstring(file).c_str(), nullptr, 0, nullptr, nullptr);
    if (!needed) return {};
    std::wstring out(needed + 1, L'\0');
    DWORD written = SearchPathW(nullptr, std::wstring(file).c_str(), nullptr,
                                static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (!written || written >= out.size()) return {};
    out.resize(written);
    return out;
}

std::wstring find_codex_launcher() {
    if (auto configured = get_env(L"PIXELFORGE_CODEX_EXE"); !configured.empty()) return configured;
    if (auto exe = search_path(L"codex.exe"); !exe.empty()) return exe;
    if (auto cmd = search_path(L"codex.cmd"); !cmd.empty()) return cmd;
    if (auto bat = search_path(L"codex.bat"); !bat.empty()) return bat;
    const auto appdata = get_env(L"APPDATA");
    if (!appdata.empty()) {
        const auto npm_cmd = std::filesystem::path(appdata) / L"npm" / L"codex.cmd";
        if (std::filesystem::exists(npm_cmd)) return npm_cmd.wstring();
    }
    return {};
}

bool has_batch_extension(const std::wstring& path) {
    const auto ext = std::filesystem::path(path).extension().wstring();
    return _wcsicmp(ext.c_str(), L".cmd") == 0 || _wcsicmp(ext.c_str(), L".bat") == 0;
}

std::wstring make_launch_command(const std::wstring& launcher,
                                 const std::vector<std::wstring>& args,
                                 std::wstring& application) {
    std::wstring invocation = quote_windows_arg(launcher);
    for (const auto& arg : args) {
        invocation.push_back(L' ');
        invocation += quote_windows_arg(arg);
    }

    if (!has_batch_extension(launcher)) {
        application = launcher;
        return invocation;
    }

    auto comspec = get_env(L"COMSPEC");
    if (comspec.empty()) comspec = L"C:\\Windows\\System32\\cmd.exe";
    application = comspec;
    return quote_windows_arg(comspec) + L" /d /s /c " + quote_windows_arg(invocation);
}

std::string nested_string(std::string_view object, std::initializer_list<std::string_view> path) {
    std::string current(object);
    for (auto key : path) {
        std::string next;
        if (!extract_member_raw(current, key, next)) return {};
        current = std::move(next);
    }
    return decode_scalar_string(current);
}

std::wstring rpc_error_message(std::string_view line) {
    std::string error_raw;
    if (!extract_member_raw(line, "error", error_raw)) return L"Codex App Server returned an unknown RPC error.";
    const auto message = nested_string(error_raw, {"message"});
    return message.empty() ? L"Codex App Server returned an RPC error." : utf8_to_wide(message);
}

} // namespace

CodexAppClient::~CodexAppClient() {
    shutdown();
}

bool CodexAppClient::generate_async(CodexGenerateRequest request,
                                    StatusCallback status,
                                    CompletionCallback completion,
                                    std::wstring& error) {
    if (shutting_down_.load(std::memory_order_relaxed)) {
        error = L"PixelForge is shutting down.";
        return false;
    }
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        error = L"Codex is already working on a PixelForge task.";
        return false;
    }
    if (request.repo_root.empty() || request.executable_path.empty() || request.pipe_name.empty()) {
        busy_.store(false, std::memory_order_relaxed);
        error = L"PixelForge Codex integration is missing its repo, executable, or bridge path.";
        return false;
    }

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, request = std::move(request), status = std::move(status), completion = std::move(completion)]() mutable {
        std::wstring run_error;
        const bool ok = run_generation(request, status, run_error);
        // A fresh app-server per art task prevents old image/tool history from
        // remaining resident and guarantees the named-pipe MCP connection is
        // released before the next task begins.
        stop_process();
        busy_.store(false, std::memory_order_relaxed);
        if (completion) completion(ok, ok ? L"Codex turn completed." : run_error);
    });
    return true;
}

void CodexAppClient::shutdown() {
    shutting_down_.store(true, std::memory_order_relaxed);
    stop_process();
    if (worker_.joinable()) worker_.join();
    busy_.store(false, std::memory_order_relaxed);
}

bool CodexAppClient::ensure_server(const CodexGenerateRequest& request, std::wstring& error) {
    {
        std::lock_guard lock(process_mutex_);
        if (process_) {
            DWORD code = 0;
            if (GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE &&
                configured_repo_root_ == request.repo_root &&
                configured_executable_ == request.executable_path &&
                configured_pipe_name_ == request.pipe_name) {
                return true;
            }
        }
    }

    stop_process();
    if (!launch_server(request, error)) return false;
    if (!initialize_server(error)) {
        stop_process();
        return false;
    }
    configured_repo_root_ = request.repo_root;
    configured_executable_ = request.executable_path;
    configured_pipe_name_ = request.pipe_name;
    return true;
}

bool CodexAppClient::launch_server(const CodexGenerateRequest& request, std::wstring& error) {
    const std::wstring launcher = find_codex_launcher();
    if (launcher.empty()) {
        error = L"Could not find the Codex CLI. Install Codex or set PIXELFORGE_CODEX_EXE to codex.exe/codex.cmd.";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE parent_stdin_write = nullptr;
    HANDLE parent_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    if (!CreatePipe(&child_stdin_read, &parent_stdin_write, &sa, 0) ||
        !CreatePipe(&parent_stdout_read, &child_stdout_write, &sa, 0)) {
        error = L"Could not create pipes for Codex App Server.";
        if (child_stdin_read) CloseHandle(child_stdin_read);
        if (parent_stdin_write) CloseHandle(parent_stdin_write);
        if (parent_stdout_read) CloseHandle(parent_stdout_read);
        if (child_stdout_write) CloseHandle(child_stdout_write);
        return false;
    }
    SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0);

    const auto log_path = std::filesystem::path(request.repo_root) / L"build" / L"pixelforge-codex-app-server.log";
    std::error_code ec;
    std::filesystem::create_directories(log_path.parent_path(), ec);
    HANDLE log = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) log = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                       &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::vector<std::wstring> args;
    // Clear inherited MCP servers for this dedicated art turn. Loading unrelated
    // browser/dev MCPs adds startup latency and can even stall an otherwise pure
    // PixelForge task if one of those servers is unhealthy.
    args.push_back(L"-c");
    args.push_back(L"mcp_servers={}");
    args.push_back(L"-c");
    args.push_back(L"mcp_servers.pixelforge.command=" + toml_basic_string(request.executable_path));
    args.push_back(L"-c");
    args.push_back(L"mcp_servers.pixelforge.args=[\"--bridge\"," + toml_basic_string(request.pipe_name) + L"]");
    args.push_back(L"-c");
    args.push_back(L"mcp_servers.pixelforge.startup_timeout_sec=20");
    args.push_back(L"-c");
    args.push_back(L"mcp_servers.pixelforge.tool_timeout_sec=180");
    args.push_back(L"app-server");
    args.push_back(L"--stdio");

    std::wstring application;
    std::wstring command_line = make_launch_command(launcher, args, application);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = log == INVALID_HANDLE_VALUE ? child_stdout_write : log;

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(application.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, request.repo_root.c_str(), &si, &pi);
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    if (!created) {
        const DWORD code = GetLastError();
        CloseHandle(parent_stdin_write);
        CloseHandle(parent_stdout_read);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        error = L"Could not start Codex App Server (Windows error " + std::to_wstring(code) + L").";
        return false;
    }

    CloseHandle(pi.hThread);
    {
        std::lock_guard lock(process_mutex_);
        process_ = pi.hProcess;
        process_id_ = pi.dwProcessId;
        stdin_write_ = parent_stdin_write;
        stdout_read_ = parent_stdout_read;
        stderr_log_ = log == INVALID_HANDLE_VALUE ? nullptr : log;
        receive_buffer_.clear();
        next_request_id_ = 1;
    }
    return true;
}

bool CodexAppClient::initialize_server(std::wstring& error) {
    const auto id = next_request_id_++;
    const std::string line = "{\"method\":\"initialize\",\"id\":" + std::to_string(id) +
        ",\"params\":{\"clientInfo\":{\"name\":\"pixelforge\",\"title\":\"PixelForge\",\"version\":\"0.3.0\"},"
        "\"capabilities\":{\"optOutNotificationMethods\":[\"item/agentMessage/delta\",\"item/reasoning/summaryTextDelta\"]}}}";
    if (!write_line(line)) {
        error = L"Could not write the Codex App Server initialize request.";
        return false;
    }
    std::string ignored;
    if (!wait_for_response(id, ignored, error)) return false;
    if (!write_line("{\"method\":\"initialized\",\"params\":{}}")) {
        error = L"Could not acknowledge Codex App Server initialization.";
        return false;
    }
    return true;
}

bool CodexAppClient::run_generation(const CodexGenerateRequest& request,
                                    const StatusCallback& status,
                                    std::wstring& error) {
    if (status) status(L"Codex: starting app server...");
    if (!ensure_server(request, error)) return false;

    if (status) status(L"Codex: creating art thread...");
    const std::string cwd = wide_to_utf8(request.repo_root);
    std::string developer = request.agent_contract;
    if (!developer.empty()) developer += "\n\n";
    developer +=
        "PixelForge automatic-generation host rules:\n"
        "- Use the pixelforge MCP tools as the only artwork editing interface.\n"
        "- First call pixelforge_task with action=get, then inspect supplied references.\n"
        "- You choose the canvas size through pixelforge_task accept.\n"
        "- Preserve quality: use large exact pixel batches for construction, then visual renders and exact regional cleanup.\n"
        "- Do not use image generation, external drawing programs, shell commands, or source-file edits for the artwork.\n"
        "- Do not ask the user follow-up questions. If the requested final medium is outside PixelForge scope, use task.reject.\n"
        "- If a hard technical blocker appears after acceptance, use task.abort.\n"
        "- Call task.finish only after a final visual inspection.";

    const std::string thread_params = "{\"cwd\":" + json_quote(cwd) +
        ",\"approvalPolicy\":\"never\",\"sandbox\":\"read-only\",\"ephemeral\":true,"
        "\"serviceName\":\"PixelForge\",\"developerInstructions\":" + json_quote(developer) + "}";
    std::string thread_result;
    if (!this->request("thread/start", thread_params, thread_result, error)) return false;
    const std::string thread_id = nested_string(thread_result, {"thread", "id"});
    if (thread_id.empty()) {
        error = L"Codex App Server did not return a thread id.";
        return false;
    }

    if (status) status(L"Codex: reading prompt and references...");
    const std::string turn_text =
        "Complete the current PixelForge task now. The user's requested artwork is:\n\n" + request.prompt +
        "\n\nThe same prompt and the active content/style references are available through the PixelForge MCP tools. "
        "Work autonomously until task.finish or task.reject/task.abort.";
    const std::string turn_params = "{\"threadId\":" + json_quote(thread_id) +
        ",\"input\":[{\"type\":\"text\",\"text\":" + json_quote(turn_text) + "}]}";
    std::string turn_result;
    if (!this->request("turn/start", turn_params, turn_result, error)) return false;

    if (status) status(L"Codex: working in PixelForge...");
    for (;;) {
        if (shutting_down_.load(std::memory_order_relaxed)) {
            error = L"PixelForge closed while Codex was working.";
            return false;
        }
        std::string line;
        if (!read_line(line)) {
            error = L"Codex App Server disconnected before the turn completed.";
            return false;
        }

        std::string method_raw;
        const bool has_method = extract_member_raw(line, "method", method_raw);
        const std::string method = has_method ? decode_scalar_string(method_raw) : std::string{};
        std::string id_raw;
        const bool has_id = extract_member_raw(line, "id", id_raw);
        if (has_method && has_id) {
            handle_server_request(line);
            continue;
        }
        if (method == "turn/completed") {
            std::string params_raw;
            extract_member_raw(line, "params", params_raw);
            const std::string turn_status = nested_string(params_raw, {"turn", "status"});
            if (turn_status == "failed") {
                std::string error_raw;
                if (extract_member_raw(params_raw, "turn", error_raw)) {
                    std::string inner;
                    if (extract_member_raw(error_raw, "error", inner)) {
                        const auto message = nested_string(inner, {"message"});
                        if (!message.empty()) error = utf8_to_wide(message);
                    }
                }
                if (error.empty()) error = L"Codex turn failed.";
                return false;
            }
            if (status) status(L"Codex: turn completed.");
            return true;
        }
        handle_notification(line, &status);
    }
}

bool CodexAppClient::request(std::string_view method, std::string_view params_json,
                             std::string& result_json, std::wstring& error) {
    const auto id = next_request_id_++;
    const std::string line = "{\"method\":" + json_quote(method) + ",\"id\":" + std::to_string(id) +
                             ",\"params\":" + std::string(params_json) + "}";
    if (!write_line(line)) {
        error = L"Could not write a request to Codex App Server.";
        return false;
    }
    return wait_for_response(id, result_json, error);
}

bool CodexAppClient::wait_for_response(std::int64_t id, std::string& result_json, std::wstring& error) {
    for (;;) {
        std::string line;
        if (!read_line(line)) {
            error = L"Codex App Server disconnected while waiting for a response.";
            return false;
        }
        std::string method_raw;
        const bool has_method = extract_member_raw(line, "method", method_raw);
        std::string id_raw;
        const bool has_id = extract_member_raw(line, "id", id_raw);
        if (has_method && has_id) {
            handle_server_request(line);
            continue;
        }
        if (has_method) {
            handle_notification(line, nullptr);
            continue;
        }
        if (!has_id) continue;
        const auto response_id = parse_i64(id_raw);
        if (!response_id || *response_id != id) continue;

        std::string error_raw;
        if (extract_member_raw(line, "error", error_raw)) {
            error = rpc_error_message(line);
            return false;
        }
        if (!extract_member_raw(line, "result", result_json)) {
            error = L"Codex App Server response did not contain a result.";
            return false;
        }
        return true;
    }
}

bool CodexAppClient::read_line(std::string& line) {
    line.clear();
    for (;;) {
        const auto newline = receive_buffer_.find('\n');
        if (newline != std::string::npos) {
            line.assign(receive_buffer_.data(), newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            receive_buffer_.erase(0, newline + 1);
            return true;
        }

        HANDLE output = nullptr;
        {
            std::lock_guard lock(process_mutex_);
            output = stdout_read_;
        }
        if (!output) return false;
        char buffer[16384];
        DWORD read = 0;
        if (!ReadFile(output, buffer, sizeof(buffer), &read, nullptr) || read == 0) return false;
        receive_buffer_.append(buffer, buffer + read);
        if (receive_buffer_.size() > 32u * 1024u * 1024u) return false;
    }
}

bool CodexAppClient::write_line(std::string_view line) {
    HANDLE input = nullptr;
    {
        std::lock_guard lock(process_mutex_);
        input = stdin_write_;
    }
    if (!input) return false;

    std::string framed(line);
    framed.push_back('\n');
    DWORD offset = 0;
    while (offset < framed.size()) {
        DWORD written = 0;
        if (!WriteFile(input, framed.data() + offset,
                       static_cast<DWORD>(framed.size() - offset), &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

void CodexAppClient::handle_server_request(std::string_view line) {
    std::string id_raw, method_raw;
    if (!extract_member_raw(line, "id", id_raw) || !extract_member_raw(line, "method", method_raw)) return;
    const std::string method = decode_scalar_string(method_raw);

    std::string result;
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval") {
        result = "{\"decision\":\"decline\"}";
    } else if (method == "mcpServer/elicitation/request") {
        result = "{\"action\":\"decline\",\"content\":null}";
    } else if (method == "item/permissions/requestApproval") {
        result = "{\"scope\":\"turn\",\"permissions\":{}}";
    } else {
        write_line("{\"id\":" + id_raw + ",\"error\":{\"code\":-32000,\"message\":\"PixelForge automatic generation is non-interactive.\"}}");
        return;
    }
    write_line("{\"id\":" + id_raw + ",\"result\":" + result + "}");
}

void CodexAppClient::handle_notification(std::string_view line, const StatusCallback* status) {
    if (!status || !*status) return;
    std::string method_raw;
    if (!extract_member_raw(line, "method", method_raw)) return;
    const std::string method = decode_scalar_string(method_raw);
    if (method == "turn/started") (*status)(L"Codex: planning pixel work...");
    else if (method == "item/started") (*status)(L"Codex: editing / inspecting...");
    else if (method == "error") {
        std::string params_raw;
        extract_member_raw(line, "params", params_raw);
        const auto message = nested_string(params_raw, {"error", "message"});
        if (!message.empty()) (*status)(L"Codex: " + utf8_to_wide(message));
    }
}

void CodexAppClient::stop_process() {
    HANDLE process = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE log = nullptr;
    {
        std::lock_guard lock(process_mutex_);
        process = std::exchange(process_, nullptr);
        input = std::exchange(stdin_write_, nullptr);
        output = std::exchange(stdout_read_, nullptr);
        log = std::exchange(stderr_log_, nullptr);
        process_id_ = 0;
        configured_repo_root_.clear();
        configured_executable_.clear();
        configured_pipe_name_.clear();
    }

    if (input) CloseHandle(input);
    if (process) {
        if (WaitForSingleObject(process, 500) == WAIT_TIMEOUT) TerminateProcess(process, 0);
        WaitForSingleObject(process, 1000);
        CloseHandle(process);
    }
    if (output) CloseHandle(output);
    if (log) CloseHandle(log);
    receive_buffer_.clear();
}

} // namespace pixelforge::win32
