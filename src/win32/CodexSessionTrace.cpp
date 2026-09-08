#include "CodexSessionTrace.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pixelforge::win32 {
namespace {

std::mutex g_trace_mutex;
std::string g_tx_buffer;
std::string g_rx_buffer;
bool g_session_started = false;
std::uint64_t g_asset_index = 0;
std::unordered_map<std::string, std::string> g_tool_calls;
std::unordered_map<std::string, std::string> g_asset_by_hash;

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
    wchar_t override_path[32768]{};
    const auto size = GetEnvironmentVariableW(L"PIXELFORGE_TRACE_PATH", override_path, 32768);
    if (size && size < 32768) return override_path;
    const auto root = find_repo_root();
    if (!root.empty()) return root / L"build" / L"pixelforge-codex-session.log";
    return std::filesystem::temp_directory_path() / L"pixelforge-codex-session.log";
}

std::filesystem::path conversation_path() {
    auto path = trace_path();
    path += L".conversation.jsonl";
    return path;
}

std::filesystem::path transcript_path() {
    auto path = trace_path();
    path += L".transcript.md";
    return path;
}

std::filesystem::path assets_path() {
    auto path = conversation_path();
    path += L".assets";
    return path;
}

std::string timestamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return text;
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

bool decode_json_string(std::string_view json, std::size_t& p, std::string& out, std::size_t limit = 0) {
    if (p >= json.size() || json[p] != '"') return false;
    ++p;
    out.clear();
    while (p < json.size()) {
        const char c = json[p++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
        } else {
            if (p >= json.size()) return false;
            const char e = json[p++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    out += "\\u";
                    for (int i = 0; i < 4 && p < json.size(); ++i) out.push_back(json[p++]);
                    break;
                default: out.push_back(e); break;
            }
        }
        if (limit && out.size() >= limit) {
            out += "...";
            return true;
        }
    }
    return false;
}

std::string scalar_string(std::string_view json, std::string_view key, std::size_t limit = 160) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string_view::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    std::string out;
    return decode_json_string(json, p, out, limit) ? out : std::string{};
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

std::string raw_json_value(std::string_view json, std::string_view key, std::size_t limit = 1024u * 1024u) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string_view::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
    const std::size_t start = p;
    if (p >= json.size()) return {};
    if (json[p] == '"') {
        std::string decoded;
        if (!decode_json_string(json, p, decoded, limit)) return {};
        return decoded;
    }
    if (json[p] == '{' || json[p] == '[') {
        const char open = json[p];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        for (; p < json.size(); ++p) {
            const char c = json[p];
            if (in_string) {
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; continue; }
            if (c == open) ++depth;
            else if (c == close && --depth == 0) {
                ++p;
                const auto length = std::min(limit, p - start);
                std::string value(json.substr(start, length));
                if (length < p - start) value += "...";
                return value;
            }
        }
        return {};
    }
    while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ']') ++p;
    return std::string(json.substr(start, std::min(limit, p - start)));
}

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64_decode(std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve((text.size() / 4) * 3);
    std::uint32_t value = 0;
    int bits = -8;
    for (unsigned char c : text) {
        if (c == '=') break;
        const int v = base64_value(c);
        if (v < 0) return false;
        value = (value << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xffu));
            bits -= 8;
        }
    }
    return true;
}

std::string extension_for_mime(std::string_view mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg" || mime == "image/jpg") return ".jpg";
    if (mime == "image/webp") return ".webp";
    if (mime == "image/gif") return ".gif";
    return ".bin";
}

std::string asset_hash_key(std::string_view mime, const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    char hex[32]{};
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(mime) + ':' + std::to_string(bytes.size()) + ':' + hex;
}

std::string materialize_data_images(std::string line) {
    std::size_t search_from = 0;
    for (;;) {
        const auto uri = line.find("data:image/", search_from);
        if (uri == std::string::npos) break;
        const auto marker = line.find(";base64,", uri);
        if (marker == std::string::npos) break;
        const auto end = line.find('"', marker + 8);
        if (end == std::string::npos) break;

        const std::string mime = line.substr(uri + 5, marker - (uri + 5));
        const std::size_t data_start = marker + 8;
        const std::string_view encoded(line.data() + data_start, end - data_start);
        std::vector<std::uint8_t> bytes;
        if (!base64_decode(encoded, bytes)) {
            search_from = end + 1;
            continue;
        }

        const std::string key = asset_hash_key(mime, bytes);
        std::string filename;
        const auto existing = g_asset_by_hash.find(key);
        if (existing != g_asset_by_hash.end()) {
            filename = existing->second;
        } else {
            std::error_code ec;
            const auto directory = assets_path();
            std::filesystem::create_directories(directory, ec);
            const auto index = ++g_asset_index;
            char number[16]{};
            std::snprintf(number, sizeof(number), "%04llu", static_cast<unsigned long long>(index));
            filename = std::string(number) + extension_for_mime(mime);
            const auto file_path = directory / std::filesystem::path(filename);
            std::ofstream asset(file_path, std::ios::binary | std::ios::trunc);
            if (asset && !bytes.empty())
                asset.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            g_asset_by_hash.emplace(key, filename);
        }

        const std::string replacement = "trace-asset:" + filename + ";mime=" + mime +
            ";bytes=" + std::to_string(bytes.size()) + ";base64_chars=" + std::to_string(encoded.size());
        line.replace(uri, end - uri, replacement);
        search_from = uri + replacement.size();
    }
    return line;
}

void rotate_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    auto previous = path;
    previous += L".previous";
    std::filesystem::copy_file(path, previous, std::filesystem::copy_options::overwrite_existing, ec);
}

void append_locked(std::string_view text) {
    const auto path = trace_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (file) file << timestamp() << " " << text << "\n";
}

void append_conversation_locked(std::string_view direction, std::string_view line) {
    const auto path = conversation_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) return;
    const std::string sanitized = materialize_data_images(std::string(line));
    file << "{\"trace_timestamp\":" << json_quote(timestamp())
         << ",\"direction\":" << json_quote(direction)
         << ",\"event\":" << sanitized << "}\n";
}

void append_transcript_locked(std::string_view heading, std::string_view body) {
    const auto path = transcript_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) return;
    file << "\n## " << timestamp() << " — " << heading << "\n\n";
    if (!body.empty()) file << body << "\n";
}

void append_transcript_event_locked(std::string_view direction,
                                    std::string_view line,
                                    std::string_view method,
                                    std::string_view id,
                                    std::string_view type,
                                    std::string_view tool) {
    if (method == "item/completed" && type == "userMessage") {
        const auto text = scalar_string(line, "text", 1024u * 1024u);
        append_transcript_locked("USER", text.empty() ? raw_json_value(line, "item") : text);
        return;
    }
    if (method == "item/completed" && type == "agentMessage") {
        const auto text = scalar_string(line, "text", 1024u * 1024u);
        append_transcript_locked("AGENT", text);
        return;
    }
    if (method == "item/tool/call") {
        const std::string tool_name = tool.empty() ? scalar_string(line, "tool", 256) : std::string(tool);
        if (!id.empty()) g_tool_calls[std::string(id)] = tool_name;
        auto arguments = raw_json_value(line, "arguments");
        if (arguments.empty()) arguments = "{}";
        append_transcript_locked("TOOL CALL — " + tool_name,
                                 "```json\n" + arguments + "\n```");
        return;
    }
    if (direction == "TX" && method.empty() && !id.empty()) {
        const auto it = g_tool_calls.find(std::string(id));
        if (it != g_tool_calls.end()) {
            auto text = scalar_string(line, "text", 1024u * 1024u);
            if (text.empty()) text = raw_json_value(line, "result");
            append_transcript_locked("TOOL RESULT — " + it->second,
                                     "```text\n" + text + "\n```");
            return;
        }
    }
    if (method == "item/completed" && type == "commandExecution") {
        const auto status = scalar_string(line, "status", 128);
        const auto command = scalar_string(line, "command", 1024u * 1024u);
        append_transcript_locked("COMMAND EXECUTION" + (status.empty() ? std::string{} : " — " + status), command);
        return;
    }
    if (method == "turn/completed") {
        append_transcript_locked("TURN COMPLETED", scalar_string(line, "status", 128));
    }
}

void reset_locked() {
    const auto path = trace_path();
    const auto conversation = conversation_path();
    const auto transcript = transcript_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    rotate_file(path);
    rotate_file(conversation);
    rotate_file(transcript);

    const auto assets = assets_path();
    auto previous_assets = assets;
    previous_assets += L".previous";
    std::filesystem::remove_all(previous_assets, ec);
    ec.clear();
    if (std::filesystem::exists(assets, ec)) {
        ec.clear();
        std::filesystem::rename(assets, previous_assets, ec);
        if (ec) {
            ec.clear();
            std::filesystem::remove_all(assets, ec);
        }
    }
    ec.clear();
    std::filesystem::create_directories(assets, ec);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) file << timestamp() << " session=start conversation=" << conversation.string()
                   << " transcript=" << transcript.string() << "\n";
    std::ofstream conversation_file(conversation, std::ios::binary | std::ios::trunc);
    std::ofstream transcript_file(transcript, std::ios::binary | std::ios::trunc);
    if (transcript_file) {
        transcript_file << "# PixelForge agent transcript\n\n"
                        << "Raw App Server events: `" << conversation.string() << "`\n\n"
                        << "This transcript preserves user/agent messages and tool activity for efficiency review. "
                           "The raw JSONL remains authoritative when exact protocol details are needed.\n";
    }
    g_tool_calls.clear();
    g_asset_by_hash.clear();
    g_asset_index = 0;
    g_session_started = true;
}

void trace_line_locked(std::string_view direction, std::string_view line) {
    const std::string method = scalar_string(line, "method");
    const std::string id = scalar_number(line, "id");

    if (direction == "TX" && method == "initialize") reset_locked();
    if (!g_session_started) reset_locked();

    append_conversation_locked(direction, line);

    std::string entry(direction);
    if (!method.empty()) entry += " method=" + method;
    else if (!id.empty()) entry += " response";
    else entry += " event";
    if (!id.empty()) entry += " id=" + id;

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
    if (method == "thread/tokenUsage/updated") {
        for (const auto key : {"totalTokens", "inputTokens", "cachedInputTokens", "outputTokens", "reasoningOutputTokens"}) {
            const auto value = scalar_number(line, key);
            if (!value.empty()) entry += " " + std::string(key) + "=" + value;
        }
    }
    if (method == "item/completed" && type == "agentMessage")
        entry += " text=" + scalar_string(line, "text", 1024);

    if (method == "error" || status == "failed" || line.find("\"error\"") != std::string_view::npos) {
        const auto message = scalar_string(line, "message", 1024);
        if (!message.empty()) entry += " message=" + message;
    }

    append_locked(entry);
    append_transcript_event_locked(direction, line, method, id, type, tool);
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
    if (pending.size() > 64u * 1024u * 1024u) pending.clear();
}

} // namespace

void codex_trace_detail(std::string_view text) {
    std::lock_guard lock(g_trace_mutex);
    std::string single_line(text);
    for (auto& c : single_line) if (c == '\n' || c == '\r') c = ' ';
    append_locked(single_line);
}

void codex_trace_tx_bytes(const void* data, DWORD size) {
    std::lock_guard lock(g_trace_mutex);
    feed_locked(g_tx_buffer, "TX", data, size);
}

void codex_trace_rx_bytes(const void* data, DWORD size) {
    std::lock_guard lock(g_trace_mutex);
    feed_locked(g_rx_buffer, "RX", data, size);
}

} // namespace pixelforge::win32
