#include "CodexSessionTrace.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pixelforge::win32 {
namespace {

std::mutex g_trace_mutex;
std::string g_tx_buffer;
std::string g_rx_buffer;
bool g_session_started = false;

std::filesystem::path find_repo_root() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    std::filesystem::path current = std::filesystem::path(std::wstring(buffer.data(), count)).parent_path();
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        std::error_code ec;
        if (std::filesystem::exists(current / L"CMakeLists.txt", ec)) return current;
        current = current.parent_path();
    }
    return {};
}

std::filesystem::path trace_path() {
    const auto root = find_repo_root();
    if (!root.empty()) return root / L"build" / L"pixelforge-codex-session.log";
    return std::filesystem::temp_directory_path() / L"pixelforge-codex-session.log";
}

std::string timestamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[32]{};
    sprintf_s(text, "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return text;
}

std::string scalar_string(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string_view::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size()) {
        const char c = json[p++];
        if (c == '"') return out;
        if (c == '\\') {
            if (p >= json.size()) return {};
            const char e = json[p++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back(' '); break;
                case 'r': break;
                case 't': out.push_back(' '); break;
                default: out.push_back('?'); break;
            }
        } else {
            out.push_back(c);
        }
        if (out.size() >= 160) {
            out += "...";
            return out;
        }
    }
    return {};
}

std::string scalar_number(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string_view::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    const std::size_t start = p;
    if (p < json.size() && json[p] == '-') ++p;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') ++p;
    return p > start ? std::string(json.substr(start, p - start)) : std::string{};
}

void append_locked(std::string_view text) {
    const auto path = trace_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (file) file << timestamp() << " " << text << "\n";
}

void reset_locked() {
    const auto path = trace_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) file << timestamp() << " session=start\n";
    g_session_started = true;
}

void trace_line_locked(std::string_view direction, std::string_view line) {
    const std::string method = scalar_string(line, "method");
    const std::string id = scalar_number(line, "id");

    // The first initialize request uniquely marks a new Generate run. Truncate
    // here so old sessions never contaminate diagnostics.
    if (direction == "TX" && method == "initialize") reset_locked();
    if (!g_session_started) reset_locked();

    std::string entry(direction);
    if (!method.empty()) entry += " method=" + method;
    else if (!id.empty()) entry += " response";
    else entry += " event";
    if (!id.empty()) entry += " id=" + id;

    // Keep only small scalar metadata. Never log params/input/result/data/patch.
    const auto type = scalar_string(line, "type");
    const auto server = scalar_string(line, "server");
    const auto tool = scalar_string(line, "tool");
    const auto name = scalar_string(line, "name");
    const auto status = scalar_string(line, "status");
    const auto model = scalar_string(line, "model");
    const auto code = scalar_string(line, "code");
    if (!type.empty()) entry += " type=" + type;
    if (!server.empty()) entry += " server=" + server;
    if (!tool.empty()) entry += " tool=" + tool;
    if (!name.empty() && name != method) entry += " name=" + name;
    if (!status.empty()) entry += " status=" + status;
    if (!model.empty()) entry += " model=" + model;
    if (!code.empty()) entry += " code=" + code;

    // Error text is useful diagnostically, but ordinary agent/user text is not
    // logged. Restrict message capture to explicit errors/failures.
    if (method == "error" || status == "failed" || line.find("\"error\"") != std::string_view::npos) {
        const auto message = scalar_string(line, "message");
        if (!message.empty()) entry += " message=" + message;
    }

    append_locked(entry);
}

void feed_locked(std::string& pending, std::string_view direction, const void* data, DWORD size) {
    if (!data || size == 0) return;
    pending.append(static_cast<const char*>(data), static_cast<std::size_t>(size));
    for (;;) {
        const auto newline = pending.find('\n');
        if (newline == std::string::npos) break;
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) trace_line_locked(direction, line);
    }
    if (pending.size() > 1024u * 1024u) pending.clear();
}

} // namespace

void codex_trace_tx_bytes(const void* data, DWORD size) {
    std::lock_guard lock(g_trace_mutex);
    feed_locked(g_tx_buffer, "TX", data, size);
}

void codex_trace_rx_bytes(const void* data, DWORD size) {
    std::lock_guard lock(g_trace_mutex);
    feed_locked(g_rx_buffer, "RX", data, size);
}

} // namespace pixelforge::win32
