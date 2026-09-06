#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pixelforge {

// Deliberately small JSON object parser for the agent transport. PixelForge's
// wire protocol is flat by design: complex edit data is carried in compact
// strings (patches/palettes) rather than verbose nested JSON arrays.
struct FlatJsonObject {
    std::unordered_map<std::string, std::string> values;

    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] std::string get(std::string_view key, std::string fallback = {}) const;
    [[nodiscard]] std::optional<std::int64_t> get_i64(std::string_view key) const;
    [[nodiscard]] std::optional<bool> get_bool(std::string_view key) const;
};

bool parse_flat_json_object(std::string_view text, FlatJsonObject& out, std::string& error);

std::string json_escape(std::string_view text);
std::string json_quote(std::string_view text);

} // namespace pixelforge
