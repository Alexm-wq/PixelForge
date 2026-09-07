#include "CodexAppClient.hpp"
#include "CodexSessionTrace.hpp"
#include "LocalAgentToolSession.hpp"
#include "MiniJson.hpp"

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

std::wstring quote_windows_arg(std::wstring_view arg) {
    if (arg.empty()) return L"\"\"";
    const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(arg);
    std::wstring out;
    out.push_back(L'"');
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
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
    const DWORD needed = SearchPathW(nullptr, std::wstring(file).c_str(), nullptr, 0, nullptr, nullptr);
    if (!needed) return {};
    std::wstring out(needed + 1, L'\0');
    const DWORD written = SearchPathW(nullptr, std::wstring(file).c_str(), nullptr,
                                      static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (!written || written >= out.size()) return {};
    out.resize(written);
    return out;
}

std::wstring module_directory() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path().wstring();
}

std::wstring find_codex_launcher() {
    if (auto configured = get_env(L"PIXELFORGE_CODEX_EXE"); !configured.empty()) return configured;
    if (auto configured = get_env(L"CODEX_CLI_PATH"); !configured.empty()) return configured;
    if (auto exe = search_path(L"codex.exe"); !exe.empty()) return exe;
    const auto module_dir = module_directory();
    if (!module_dir.empty()) {
        const auto bundled = std::filesystem::path(module_dir) / L"codex.cmd";
        if (std::filesystem::exists(bundled)) return bundled.wstring();
    }
    if (auto cmd = search_path(L"codex.cmd"); !cmd.empty()) return cmd;
    if (auto bat = search_path(L"codex.bat"); !bat.empty()) return bat;
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

} // namespace

CodexAppClient::~CodexAppClient() { shutdown(); }

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
    if (request.repo_root.empty() || request.executable_path.empty() || !request.tool_session) {
        busy_.store(false, std::memory_order_relaxed);
        error = L"PixelForge Codex integration is missing its repo, executable, or local dynamic-tool session.";
        return false;
    }

    if (worker_.joinable()) worker_.join();
    cancel_requested_.store(false);
    worker_ = std::thread([this, request = std::move(request), status = std::move(status), completion = std::move(completion)]() mutable {
        active_tool_session_ = request.tool_session;
        active_status_ = status;
        failure_reason_.clear();
        consecutive_tool_errors_ = 0;
        received_edit_ = false;
        progress_timeout_ms_ = request.progress_timeout_ms;
        progress_deadline_ = GetTickCount64() + progress_timeout_ms_;
        total_deadline_ = GetTickCount64() + request.total_timeout_ms;
        std::wstring run_error;
        const bool ok = run_generation(request, status, run_error);
        if (!ok && !failure_reason_.empty()) run_error = failure_reason_;
        codex_trace_detail("session=end success=" + std::string(ok ? "true" : "false") + " message=" + wide_to_utf8(run_error));
        active_tool_session_ = nullptr;
        active_status_ = {};
        stop_process();
        if (completion) completion(ok, ok ? L"Codex turn completed." : run_error);
        busy_.store(false, std::memory_order_relaxed);
    });
    return true;
}

void CodexAppClient::shutdown() {
    shutting_down_.store(true, std::memory_order_relaxed);
    cancel();
    if (worker_.joinable()) worker_.join();
    stop_process();
    active_tool_session_ = nullptr;
    busy_.store(false, std::memory_order_relaxed);
}

bool CodexAppClient::ensure_server(const CodexGenerateRequest& request, std::wstring& error) {
    {
        std::lock_guard lock(process_mutex_);
        if (process_) {
            DWORD code = 0;
            if (GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE &&
                configured_repo_root_ == request.repo_root && configured_executable_ == request.executable_path)
                return true;
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
    return true;
}

bool CodexAppClient::launch_server(const CodexGenerateRequest& request, std::wstring& error) {
    const std::wstring launcher = find_codex_launcher();
    if (launcher.empty()) {
        error = L"Could not find the Codex CLI. Set CODEX_CLI_PATH or PIXELFORGE_CODEX_EXE only if your install is non-standard.";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE child_stdin_read = nullptr, parent_stdin_write = nullptr;
    HANDLE parent_stdout_read = nullptr, child_stdout_write = nullptr;
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
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        log = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::vector<std::wstring> args;
    for (const auto* setting : {
             L"mcp_servers={}", L"features.apps=false", L"features.plugins=false", L"features.browser_use=false",
             L"features.browser_use_external=false", L"features.computer_use=false"}) {
        args.push_back(L"-c");
        args.push_back(setting);
    }
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
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job && (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job, pi.hProcess))) { CloseHandle(job); job = nullptr; }
    {
        std::lock_guard lock(process_mutex_);
        process_ = pi.hProcess;
        process_job_ = job;
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
        ",\"params\":{\"clientInfo\":{\"name\":\"pixelforge\",\"title\":\"PixelForge\",\"version\":\"0.4.0\"},"
        "\"capabilities\":{\"experimentalApi\":true}}}";
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

    if (status) status(L"Codex: registering PixelForge tools...");
    const std::string developer = request.agent_contract;

    const std::string thread_params = "{\"cwd\":" + json_quote(wide_to_utf8(request.repo_root)) +
        ",\"approvalPolicy\":\"never\",\"sandbox\":\"read-only\",\"ephemeral\":true,"
        "\"baseInstructions\":\"You are the PixelForge pixel artist. Create the user's requested pixel artwork with the supplied PixelForge tools. Study supplied references visually, inspect your work, and use your own judgment. Report failures accurately.\","
        "\"serviceName\":\"PixelForge\",\"developerInstructions\":" + json_quote(developer) +
        ",\"dynamicTools\":" + pixelforge_dynamic_tools_json() + "}";
    std::string thread_result;
    if (!this->request("thread/start", thread_params, thread_result, error)) return false;
    const std::string thread_id = nested_string(thread_result, {"thread", "id"});
    if (thread_id.empty()) {
        error = L"Codex App Server did not return a thread id.";
        return false;
    }

    std::string model = request.model.empty() ? "gpt-6-astra" : request.model;
    std::string effort = request.reasoning_effort;
    if (effort != "low" && effort != "medium" && effort != "high" && effort != "xhigh" && effort != "max")
        effort = "medium";
    codex_trace_detail("turn model=" + model + " reasoning_effort=" + effort);

    if (status) status(L"Codex: reading prompt and references (" + utf8_to_wide(model) + L" / " + utf8_to_wide(effort) + L")...");
    const std::string turn_text =
        "Create the requested artwork:\n\n" + request.prompt +
        "\n\nUse the supplied PixelForge tools and work autonomously.";
    const std::string turn_params = "{\"threadId\":" + json_quote(thread_id) +
        ",\"model\":" + json_quote(model) +
        ",\"effort\":" + json_quote(effort) +
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
            if (turn_status != "completed") {
                const auto message = nested_string(params_raw, {"turn", "error", "message"});
                error = message.empty() ? L"Codex turn failed." : utf8_to_wide(message);
                return false;
            }
            const auto task = active_tool_session_->call("pixelforge_task", "{\"action\":\"get\"}");
            const auto state = nested_string(task.text, {"state"});
            if (!task.success || (state != "finished" && state != "rejected" && state != "aborted")) {
                error = L"Codex ended without finishing the drawing. See build/pixelforge-codex-session.log for agent messages and tool results.";
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
        std::string method_raw, id_raw;
        const bool has_method = extract_member_raw(line, "method", method_raw);
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
        const auto now = GetTickCount64();
        if (cancel_requested_.load() || shutting_down_.load()) failure_reason_ = L"Stopped by user.";
        else if (total_deadline_ && now >= total_deadline_) failure_reason_ = L"Stopped: generation exceeded its 10 minute time budget.";
        else if (progress_deadline_ && now >= progress_deadline_) {
            const auto seconds = std::to_wstring(progress_timeout_ms_ / 1000);
            failure_reason_ = received_edit_ ? L"Stopped: no drawing progress for " + seconds + L" seconds. Existing canvas preserved."
                                            : L"Stopped: no pixel edits within " + seconds + L" seconds. Check the session log for the last tool result.";
        }
        if (!failure_reason_.empty()) return false;
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
        DWORD available = 0;
        if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (!available) { Sleep(20); continue; }
        char buffer[16384];
        DWORD read = 0;
        if (!ReadFile(output, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || read == 0) return false;
        codex_trace_rx_bytes(buffer, read);
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
    framed.erase(std::remove_if(framed.begin(), framed.end(), [](char c) { return c == '\r' || c == '\n'; }), framed.end());
    framed.push_back('\n');
    codex_trace_tx_bytes(framed.data(), static_cast<DWORD>(framed.size()));
    DWORD offset = 0;
    while (offset < framed.size()) {
        DWORD written = 0;
        if (!WriteFile(input, framed.data() + offset,
                       static_cast<DWORD>(framed.size() - offset), &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

void CodexAppClient::handle_server_request(std::string_view line) {
    std::string id_raw, method_raw;
    if (!extract_member_raw(line, "id", id_raw) || !extract_member_raw(line, "method", method_raw)) return;
    const std::string method = decode_scalar_string(method_raw);

    if (method == "item/tool/call") {
        std::string params_raw;
        if (!extract_member_raw(line, "params", params_raw) || !active_tool_session_) {
            write_line("{\"id\":" + id_raw + ",\"error\":{\"code\":-32000,\"message\":\"PixelForge dynamic tool session is unavailable.\"}}");
            return;
        }
        const std::string tool = nested_string(params_raw, {"tool"});
        std::string arguments;
        if (!extract_member_raw(params_raw, "arguments", arguments)) arguments = "{}";
        const auto action = nested_string(arguments, {"action"});
        codex_trace_detail("TOOL start id=" + id_raw + " tool=" + tool + " action=" + action + " argument_bytes=" + std::to_string(arguments.size()));
        const auto started = GetTickCount64();
        const auto output = active_tool_session_->call(tool, arguments);
        auto diagnostic = output.text.substr(0, 4096);
        if (tool == "pixelforge_task") {
            FlatJsonObject fields; std::string ignored;
            if (parse_flat_json_object(output.text, fields, ignored)) {
                diagnostic.clear();
                for (const auto key : {"task_id", "state", "revision", "canvas_width", "canvas_height", "error", "message", "status_message"})
                    if (fields.contains(key)) diagnostic += std::string(key) + "=" + fields.get(key) + " ";
            }
        }
        codex_trace_detail("TOOL result id=" + id_raw + " tool=" + tool + " success=" + (output.success ? "true" : "false") +
            " elapsed_ms=" + std::to_string(GetTickCount64() - started) + " image_bytes=" + std::to_string(output.image_base64.size()) + " result=" + diagnostic);
        if (active_status_) active_status_(L"Codex: " + utf8_to_wide(tool + " " + action + (output.success ? " succeeded" : " FAILED: " + diagnostic)));
        consecutive_tool_errors_ = output.success ? 0 : consecutive_tool_errors_ + 1;
        if (consecutive_tool_errors_ >= 3) failure_reason_ = L"Stopped after three consecutive tool failures. See the session log for exact errors.";
        FlatJsonObject result_fields; std::string parse_error;
        const bool drawing_tool = tool == "pixelforge_edit" || tool == "pixelforge_program";
        if (output.success && drawing_tool && parse_flat_json_object(output.text, result_fields, parse_error) &&
            result_fields.get_i64("changed_pixels").value_or(0) > 0) {
            received_edit_ = true;
            progress_deadline_ = GetTickCount64() + progress_timeout_ms_;
        }
        std::string content = "[{\"type\":\"inputText\",\"text\":" + json_quote(output.text) + "}";
        if (!output.image_base64.empty()) {
            const std::string mime = output.image_mime.empty() ? "image/png" : output.image_mime;
            content += ",{\"type\":\"inputImage\",\"imageUrl\":" +
                       json_quote("data:" + mime + ";base64," + output.image_base64) + "}";
        }
        content += "]";
        write_line("{\"id\":" + id_raw + ",\"result\":{\"contentItems\":" + content +
                   ",\"success\":" + std::string(output.success ? "true" : "false") + "}}");
        return;
    }

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
    const StatusCallback quiet = [](std::wstring) {};
    if (!status || !*status) status = &quiet;
    std::string method_raw;
    if (!extract_member_raw(line, "method", method_raw)) return;
    const std::string method = decode_scalar_string(method_raw);
    std::string params;
    extract_member_raw(line, "params", params);
    if (method == "thread/tokenUsage/updated") {
        std::string usage;
        if (extract_member_raw(params, "tokenUsage", usage)) {
            for (const auto scope : {"total", "last"}) {
                std::string breakdown; FlatJsonObject fields; std::string ignored;
                if (!extract_member_raw(usage, scope, breakdown) || !parse_flat_json_object(breakdown, fields, ignored)) continue;
                std::string detail = std::string("USAGE scope=") + scope;
                for (const auto key : {"inputTokens", "cachedInputTokens", "cacheWriteInputTokens", "outputTokens", "reasoningOutputTokens", "totalTokens"})
                    if (fields.contains(key)) detail += " " + std::string(key) + "=" + fields.get(key);
                const auto input = fields.get_i64("inputTokens"), cached = fields.get_i64("cachedInputTokens");
                if (input && cached) detail += " uncachedInputTokens=" + std::to_string(std::max<std::int64_t>(0, *input - *cached));
                codex_trace_detail(detail);
            }
        }
    }
    if (method == "turn/started") (*status)(L"Codex: planning first drawing batch...");
    else if (method == "item/completed" && nested_string(params, {"item", "type"}) == "agentMessage") {
        const auto text = nested_string(params, {"item", "text"});
        codex_trace_detail("AGENT " + text.substr(0, 8192));
        if (!text.empty()) (*status)(L"Codex: " + utf8_to_wide(text));
    }
    else if (method == "item/started") {
        const auto type = nested_string(params, {"item", "type"});
        if (type == "reasoning") (*status)(L"Codex: reasoning (canvas unchanged until an edit commits)...");
        else if (type == "dynamicToolCall") (*status)(L"Codex: " + utf8_to_wide(nested_string(params, {"item", "tool"})));
    }
    else if (method == "error") {
        std::string params_raw;
        extract_member_raw(line, "params", params_raw);
        const auto message = nested_string(params_raw, {"error", "message"});
        if (!message.empty()) (*status)(L"Codex: " + utf8_to_wide(message));
    }
}

void CodexAppClient::stop_process() {
    HANDLE process = nullptr, input = nullptr, output = nullptr, log = nullptr, job = nullptr;
    {
        std::lock_guard lock(process_mutex_);
        process = std::exchange(process_, nullptr);
        job = std::exchange(process_job_, nullptr);
        input = std::exchange(stdin_write_, nullptr);
        output = std::exchange(stdout_read_, nullptr);
        log = std::exchange(stderr_log_, nullptr);
        process_id_ = 0;
        configured_repo_root_.clear();
        configured_executable_.clear();
    }
    if (input) CloseHandle(input);
    if (job) CloseHandle(job);
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