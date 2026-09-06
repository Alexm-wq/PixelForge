#include "MiniJson.hpp"

#include <charconv>
#include <cctype>

namespace pixelforge {

namespace {

void skip_ws(std::string_view text, std::size_t& p) {
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
}

bool parse_string(std::string_view text, std::size_t& p, std::string& out, std::string& error) {
    if (p >= text.size() || text[p] != '"') {
        error = "Expected JSON string.";
        return false;
    }
    ++p;
    out.clear();
    while (p < text.size()) {
        const char c = text[p++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (p >= text.size()) {
            error = "Unterminated JSON escape.";
            return false;
        }
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
                error = "Unsupported JSON escape. Use UTF-8 directly; \\u escapes are not required by the PixelForge protocol.";
                return false;
        }
    }
    error = "Unterminated JSON string.";
    return false;
}

bool parse_scalar(std::string_view text, std::size_t& p, std::string& out, std::string& error) {
    skip_ws(text, p);
    if (p >= text.size()) {
        error = "Missing JSON value.";
        return false;
    }
    if (text[p] == '"') return parse_string(text, p, out, error);
    if (text[p] == '{' || text[p] == '[') {
        error = "Nested JSON is intentionally unsupported; use PixelForge compact string fields for patches and palettes.";
        return false;
    }
    const std::size_t start = p;
    while (p < text.size() && text[p] != ',' && text[p] != '}') ++p;
    std::size_t end = p;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (end == start) {
        error = "Missing JSON scalar value.";
        return false;
    }
    out.assign(text.substr(start, end - start));
    return true;
}

} // namespace

bool FlatJsonObject::contains(std::string_view key) const {
    return values.find(std::string(key)) != values.end();
}

std::string FlatJsonObject::get(std::string_view key, std::string fallback) const {
    const auto it = values.find(std::string(key));
    return it == values.end() ? std::move(fallback) : it->second;
}

std::optional<std::int64_t> FlatJsonObject::get_i64(std::string_view key) const {
    const auto it = values.find(std::string(key));
    if (it == values.end()) return std::nullopt;
    std::int64_t value = 0;
    const auto* begin = it->second.data();
    const auto* end = begin + it->second.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}

std::optional<bool> FlatJsonObject::get_bool(std::string_view key) const {
    const auto it = values.find(std::string(key));
    if (it == values.end()) return std::nullopt;
    if (it->second == "true" || it->second == "1") return true;
    if (it->second == "false" || it->second == "0") return false;
    return std::nullopt;
}

bool parse_flat_json_object(std::string_view text, FlatJsonObject& out, std::string& error) {
    out.values.clear();
    std::size_t p = 0;
    skip_ws(text, p);
    if (p >= text.size() || text[p] != '{') {
        error = "Expected a JSON object.";
        return false;
    }
    ++p;
    skip_ws(text, p);
    if (p < text.size() && text[p] == '}') return true;

    for (;;) {
        skip_ws(text, p);
        std::string key;
        if (!parse_string(text, p, key, error)) return false;
        skip_ws(text, p);
        if (p >= text.size() || text[p] != ':') {
            error = "Expected ':' after JSON key.";
            return false;
        }
        ++p;
        std::string value;
        if (!parse_scalar(text, p, value, error)) return false;
        out.values[std::move(key)] = std::move(value);
        skip_ws(text, p);
        if (p >= text.size()) {
            error = "Unterminated JSON object.";
            return false;
        }
        if (text[p] == '}') {
            ++p;
            break;
        }
        if (text[p] != ',') {
            error = "Expected ',' or '}' in JSON object.";
            return false;
        }
        ++p;
    }
    skip_ws(text, p);
    if (p != text.size()) {
        error = "Unexpected trailing data after JSON object.";
        return false;
    }
    return true;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out.push_back(' ');
                else out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

std::string json_quote(std::string_view text) {
    return "\"" + json_escape(text) + "\"";
}

} // namespace pixelforge
