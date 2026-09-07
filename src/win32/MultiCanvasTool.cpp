#include "LocalAgentToolSession.hpp"

#include "ImageIO.hpp"
#include "MiniJson.hpp"
#include "PixelProgram.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

constexpr std::size_t kMaxPackCanvases = 1024;
constexpr std::uint64_t kMaxPackPixels = 64ull * 1024ull * 1024ull;
constexpr int kMaxPreviewScale = 16;

struct PackCanvas {
    std::string name;
    std::string group;
    int frame = -1;
    PixelDocument* document = nullptr;
    std::unique_ptr<PixelDocument> owned;
};

struct PackHistoryGroup {
    std::vector<std::string> canvases;
};

struct PackWorkspace {
    std::uint64_t task_id = 0;
    PixelDocument* primary = nullptr;
    std::vector<PackCanvas> canvases;
    std::uint64_t revision = 0;
    std::vector<PackHistoryGroup> undo_groups;
    std::vector<PackHistoryGroup> redo_groups;
};

struct CanvasSpec {
    std::string name;
    std::string group;
    int width = 0;
    int height = 0;
    int frame = -1;
};

struct ProgramSection {
    std::string canvas;
    std::string code;
};

struct RenderedMedia {
    bool ok = false;
    std::string error;
    std::string mime;
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    std::vector<std::string> names;
};

std::mutex g_pack_mutex;
std::unordered_map<AgentTaskController*, PackWorkspace> g_pack_workspaces;
std::atomic_uint64_t g_preview_counter{0};

std::string quote_json(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
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

std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string upper_copy(std::string value) {
    for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::vector<std::string> split_keep_empty(std::string_view text, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            break;
        }
        out.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool parse_int_value(std::string_view text, int& value) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool safe_name(std::string_view name) {
    if (name.empty() || name.size() > 96) return false;
    for (const unsigned char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

std::wstring utf8_to_wide_pack(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string join_names(const std::vector<std::string>& names, char separator = '|') {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out.push_back(separator);
        out += names[i];
    }
    return out;
}

std::string workspace_summary(const PackWorkspace& workspace) {
    std::string out;
    for (std::size_t i = 0; i < workspace.canvases.size(); ++i) {
        const auto& canvas = workspace.canvases[i];
        if (i) out.push_back('|');
        out += canvas.name + "," + canvas.group + "," + std::to_string(canvas.document->width()) + "," +
               std::to_string(canvas.document->height()) + "," + std::to_string(canvas.frame);
    }
    return out;
}

bool parse_specs(std::string_view text, std::vector<CanvasSpec>& specs, std::string& error) {
    specs.clear();
    if (text.empty()) {
        error = "canvases is required. Use name,group,width,height[,frame] entries separated by |.";
        return false;
    }
    std::unordered_set<std::string> names;
    std::uint64_t total_pixels = 0;
    for (const auto& entry_raw : split_keep_empty(text, '|')) {
        const auto entry = trim_copy(entry_raw);
        if (entry.empty()) continue;
        const auto fields = split_keep_empty(entry, ',');
        if (fields.size() < 4 || fields.size() > 5) {
            error = "Each canvas must be name,group,width,height[,frame].";
            return false;
        }
        CanvasSpec spec;
        spec.name = trim_copy(fields[0]);
        spec.group = trim_copy(fields[1]);
        if (!safe_name(spec.name)) {
            error = "Canvas names may contain only letters, digits, _, -, and .";
            return false;
        }
        if (!spec.group.empty() && !safe_name(spec.group)) {
            error = "Canvas groups may contain only letters, digits, _, -, and .";
            return false;
        }
        if (!names.insert(upper_copy(spec.name)).second) {
            error = "Canvas names must be unique.";
            return false;
        }
        if (!parse_int_value(trim_copy(fields[2]), spec.width) ||
            !parse_int_value(trim_copy(fields[3]), spec.height) || spec.width <= 0 || spec.height <= 0) {
            error = "Canvas width/height must be positive integers.";
            return false;
        }
        if (fields.size() == 5 && !trim_copy(fields[4]).empty() &&
            !parse_int_value(trim_copy(fields[4]), spec.frame)) {
            error = "Frame must be an integer when supplied.";
            return false;
        }
        total_pixels += static_cast<std::uint64_t>(spec.width) * static_cast<std::uint64_t>(spec.height);
        if (total_pixels > kMaxPackPixels) {
            error = "Canvas pack exceeds the 64-megapixel safety limit.";
            return false;
        }
        specs.push_back(std::move(spec));
        if (specs.size() > kMaxPackCanvases) {
            error = "Canvas pack exceeds the 1024-canvas safety limit.";
            return false;
        }
    }
    if (specs.empty()) {
        error = "At least one canvas is required.";
        return false;
    }
    return true;
}

PackCanvas* find_canvas(PackWorkspace& workspace, std::string_view name) {
    if (name.empty()) return workspace.canvases.empty() ? nullptr : &workspace.canvases.front();
    const auto wanted = upper_copy(std::string(name));
    for (auto& canvas : workspace.canvases) {
        if (upper_copy(canvas.name) == wanted) return &canvas;
    }
    return nullptr;
}

const PackCanvas* find_canvas(const PackWorkspace& workspace, std::string_view name) {
    if (name.empty()) return workspace.canvases.empty() ? nullptr : &workspace.canvases.front();
    const auto wanted = upper_copy(std::string(name));
    for (const auto& canvas : workspace.canvases) {
        if (upper_copy(canvas.name) == wanted) return &canvas;
    }
    return nullptr;
}

std::vector<PackCanvas*> select_canvases(PackWorkspace& workspace,
                                         std::string_view explicit_names,
                                         std::string_view group,
                                         std::string_view single) {
    std::vector<PackCanvas*> selected;
    if (!single.empty()) {
        if (auto* canvas = find_canvas(workspace, single)) selected.push_back(canvas);
        return selected;
    }
    if (!explicit_names.empty()) {
        for (const auto& name : split_keep_empty(explicit_names, '|')) {
            if (auto* canvas = find_canvas(workspace, trim_copy(name))) selected.push_back(canvas);
        }
    } else if (!group.empty()) {
        const auto wanted = upper_copy(std::string(group));
        for (auto& canvas : workspace.canvases)
            if (upper_copy(canvas.group) == wanted) selected.push_back(&canvas);
    } else {
        for (auto& canvas : workspace.canvases) selected.push_back(&canvas);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const PackCanvas* a, const PackCanvas* b) {
        if (a->frame >= 0 && b->frame >= 0 && a->frame != b->frame) return a->frame < b->frame;
        if ((a->frame >= 0) != (b->frame >= 0)) return a->frame >= 0;
        return a->name < b->name;
    });
    return selected;
}

std::vector<std::uint32_t> parse_palette(std::string_view text) {
    std::vector<std::uint32_t> palette;
    for (const auto& token_raw : split_keep_empty(text, ',')) {
        auto token = trim_copy(token_raw);
        if (token.empty()) continue;
        if (token.front() == '#') token.erase(token.begin());
        if (token.size() != 8) return {};
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return {};
        palette.push_back(value);
    }
    return palette;
}

bool resolve_patch_color(std::string_view token,
                         const std::vector<std::uint32_t>& palette,
                         std::uint32_t& color,
                         std::string& error) {
    if (!token.empty() && token.front() == '#') {
        if (token.size() != 9) {
            error = "Exact colors must use #AARRGGBB.";
            return false;
        }
        const auto parsed = std::from_chars(token.data() + 1, token.data() + token.size(), color, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) return true;
        error = "Invalid #AARRGGBB color.";
        return false;
    }
    int index = -1;
    if (!parse_int_value(token, index) || index < 0 || static_cast<std::size_t>(index) >= palette.size()) {
        error = "Palette index is out of range.";
        return false;
    }
    color = palette[static_cast<std::size_t>(index)];
    return true;
}

bool rect_fits(const PixelDocument& document, int x, int y, int width, int height) {
    return width > 0 && height > 0 && x >= 0 && y >= 0 &&
           x <= document.width() - width && y <= document.height() - height;
}

bool tx_line(PixelDocument::Transaction& tx, int x0, int y0, int x1, int y1, std::uint32_t color) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (!tx.set_pixel(x0, y0, color)) return false;
        if (x0 == x1 && y0 == y1) return true;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

bool apply_patch(PixelDocument& document,
                 std::string_view patch,
                 const std::vector<std::uint32_t>& palette,
                 std::size_t& changed,
                 std::string& error) {
    auto tx = document.begin_transaction();
    for (const auto& op_raw : split_keep_empty(patch, ';')) {
        const auto op_text = trim_copy(op_raw);
        if (op_text.empty()) continue;
        const auto fields = split_keep_empty(op_text, ',');
        if (fields.empty() || fields[0].size() != 1) {
            tx.cancel();
            error = "Malformed compiled patch operation.";
            return false;
        }
        auto number = [&](std::size_t i, int& out) {
            return i < fields.size() && parse_int_value(trim_copy(fields[i]), out);
        };
        std::uint32_t color = 0;
        bool ok = false;
        const char kind = fields[0][0];
        if (kind == 'P' && fields.size() == 4) {
            int x = 0, y = 0;
            ok = number(1, x) && number(2, y) && resolve_patch_color(trim_copy(fields[3]), palette, color, error) &&
                 tx.set_pixel(x, y, color);
        } else if (kind == 'H' && fields.size() == 5) {
            int x = 0, y = 0, length = 0;
            ok = number(1, x) && number(2, y) && number(3, length) && length > 0 &&
                 resolve_patch_color(trim_copy(fields[4]), palette, color, error) && rect_fits(document, x, y, length, 1);
            if (ok) for (int dx = 0; dx < length; ++dx) ok = ok && tx.set_pixel(x + dx, y, color);
        } else if (kind == 'V' && fields.size() == 5) {
            int x = 0, y = 0, length = 0;
            ok = number(1, x) && number(2, y) && number(3, length) && length > 0 &&
                 resolve_patch_color(trim_copy(fields[4]), palette, color, error) && rect_fits(document, x, y, 1, length);
            if (ok) for (int dy = 0; dy < length; ++dy) ok = ok && tx.set_pixel(x, y + dy, color);
        } else if (kind == 'R' && fields.size() == 6) {
            int x = 0, y = 0, width = 0, height = 0;
            ok = number(1, x) && number(2, y) && number(3, width) && number(4, height) &&
                 resolve_patch_color(trim_copy(fields[5]), palette, color, error) &&
                 rect_fits(document, x, y, width, height) && tx.fill_rect(x, y, width, height, color);
        } else if (kind == 'L' && fields.size() == 6) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            ok = number(1, x0) && number(2, y0) && number(3, x1) && number(4, y1) &&
                 resolve_patch_color(trim_copy(fields[5]), palette, color, error) &&
                 rect_fits(document, x0, y0, 1, 1) && rect_fits(document, x1, y1, 1, 1) &&
                 tx_line(tx, x0, y0, x1, y1, color);
        } else {
            error = "Unsupported compiled patch operation.";
        }
        if (!ok) {
            tx.cancel();
            if (error.empty()) error = "Compiled patch operation is out of bounds or malformed.";
            return false;
        }
    }
    changed = tx.pending_changes();
    if (changed == 0) {
        tx.cancel();
        return true;
    }
    if (!tx.commit()) {
        error = "Could not commit canvas transaction.";
        return false;
    }
    return true;
}

bool parse_program_sections(std::string_view program,
                            std::string default_canvas,
                            std::vector<ProgramSection>& sections,
                            std::string& error) {
    sections.clear();
    std::unordered_map<std::string, std::size_t> indexes;
    std::string current = std::move(default_canvas);
    for (const auto& statement_raw : split_keep_empty(program, ';')) {
        std::size_t line_start = 0;
        const std::string statement_chunk = statement_raw;
        while (line_start <= statement_chunk.size()) {
            const auto newline = statement_chunk.find('\n', line_start);
            const auto raw = newline == std::string::npos
                ? std::string_view(statement_chunk).substr(line_start)
                : std::string_view(statement_chunk).substr(line_start, newline - line_start);
            const auto statement = trim_copy(raw);
            if (!statement.empty()) {
                const auto space = statement.find_first_of(" \t,");
                const auto command = upper_copy(statement.substr(0, space));
                if (command == "CANVAS") {
                    if (space == std::string::npos) {
                        error = "CANVAS requires a canvas name.";
                        return false;
                    }
                    current = trim_copy(statement.substr(space + 1));
                    if (!safe_name(current)) {
                        error = "Invalid CANVAS name.";
                        return false;
                    }
                } else {
                    if (current.empty()) {
                        error = "Program needs a target canvas or a CANVAS directive.";
                        return false;
                    }
                    const auto key = upper_copy(current);
                    auto it = indexes.find(key);
                    if (it == indexes.end()) {
                        indexes.emplace(key, sections.size());
                        sections.push_back({current, statement});
                    } else {
                        sections[it->second].code += ";" + statement;
                    }
                }
            }
            if (newline == std::string::npos) break;
            line_start = newline + 1;
        }
    }
    if (sections.empty()) {
        error = "Program contains no drawing commands.";
        return false;
    }
    return true;
}

std::vector<std::uint32_t> scale_nearest(const PixelDocument& document, int scale, int& out_w, int& out_h) {
    scale = std::clamp(scale, 1, kMaxPreviewScale);
    out_w = document.width() * scale;
    out_h = document.height() * scale;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(out_w) * out_h, 0);
    for (int y = 0; y < out_h; ++y) {
        const int sy = y / scale;
        for (int x = 0; x < out_w; ++x)
            out[static_cast<std::size_t>(y) * out_w + x] = document.pixel(x / scale, sy);
    }
    return out;
}

std::vector<std::uint32_t> compose_sheet(const std::vector<PackCanvas*>& selected,
                                         int scale,
                                         int columns,
                                         bool strip,
                                         int& width,
                                         int& height) {
    scale = std::clamp(scale, 1, kMaxPreviewScale);
    if (selected.empty()) { width = height = 0; return {}; }
    int max_w = 0, max_h = 0;
    for (const auto* canvas : selected) {
        max_w = std::max(max_w, canvas->document->width());
        max_h = std::max(max_h, canvas->document->height());
    }
    const int padding = std::max(1, scale);
    if (strip) columns = static_cast<int>(selected.size());
    else if (columns <= 0) {
        columns = 1;
        while (columns * columns < static_cast<int>(selected.size())) ++columns;
    }
    columns = std::clamp(columns, 1, static_cast<int>(selected.size()));
    const int rows = (static_cast<int>(selected.size()) + columns - 1) / columns;
    const int cell_w = max_w * scale;
    const int cell_h = max_h * scale;
    width = columns * cell_w + (columns - 1) * padding;
    height = rows * cell_h + (rows - 1) * padding;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width) * height, 0x00000000u);
    for (std::size_t i = 0; i < selected.size(); ++i) {
        int frame_w = 0, frame_h = 0;
        const auto frame = scale_nearest(*selected[i]->document, scale, frame_w, frame_h);
        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const int ox = col * (cell_w + padding) + (cell_w - frame_w) / 2;
        const int oy = row * (cell_h + padding) + (cell_h - frame_h) / 2;
        for (int y = 0; y < frame_h; ++y)
            std::copy_n(frame.data() + static_cast<std::size_t>(y) * frame_w, frame_w,
                        out.data() + static_cast<std::size_t>(oy + y) * width + ox);
    }
    return out;
}

bool read_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file) || bytes.empty();
}

std::string base64_pack(const std::vector<std::uint8_t>& data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t a = data[i];
        const std::uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const std::uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;
        out.push_back(table[(value >> 18) & 63]);
        out.push_back(table[(value >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? table[value & 63] : '=');
    }
    return out;
}

std::filesystem::path preview_path(std::wstring_view extension) {
    std::error_code ec;
    auto directory = std::filesystem::temp_directory_path() / L"PixelForge" / L"pack-previews";
    std::filesystem::create_directories(directory, ec);
    const auto index = ++g_preview_counter;
    return directory / (L"preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(index) + std::wstring(extension));
}

RenderedMedia encode_png(const std::vector<std::uint32_t>& pixels, int width, int height) {
    RenderedMedia out;
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<std::size_t>(width) * height) {
        out.error = "Invalid PNG preview dimensions.";
        return out;
    }
    const auto path = preview_path(L".png");
    std::wstring error;
    if (!save_png_wic(path.wstring(), width, height, pixels, error)) {
        out.error = "Could not encode PNG preview.";
        return out;
    }
    if (!read_bytes(path, out.bytes)) {
        out.error = "Could not read encoded PNG preview.";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return out;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    out.ok = true;
    out.mime = "image/png";
    out.width = width;
    out.height = height;
    return out;
}

void push_u16(std::vector<std::uint8_t>& bytes, int value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_gif_subblocks(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto count = std::min<std::size_t>(255, payload.size() - offset);
        out.push_back(static_cast<std::uint8_t>(count));
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                   payload.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    out.push_back(0);
}

std::vector<std::uint8_t> gif_lzw_literal_stream(const std::vector<std::uint8_t>& indices) {
    constexpr int clear_code = 256;
    constexpr int end_code = 257;
    constexpr int code_bits = 9;
    std::vector<std::uint8_t> out;
    std::uint32_t accumulator = 0;
    int bits = 0;
    auto write_code = [&](int code) {
        accumulator |= static_cast<std::uint32_t>(code) << bits;
        bits += code_bits;
        while (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(accumulator & 0xffu));
            accumulator >>= 8;
            bits -= 8;
        }
    };
    for (const auto index : indices) {
        write_code(clear_code);
        write_code(index);
    }
    write_code(end_code);
    if (bits > 0) out.push_back(static_cast<std::uint8_t>(accumulator & 0xffu));
    return out;
}

RenderedMedia encode_gif(const std::vector<PackCanvas*>& selected, int scale, int fps) {
    RenderedMedia out;
    if (selected.empty()) { out.error = "Animation selection is empty."; return out; }
    scale = std::clamp(scale, 1, 8);
    fps = std::clamp(fps, 1, 60);
    int max_w = 0, max_h = 0;
    for (const auto* canvas : selected) {
        max_w = std::max(max_w, canvas->document->width() * scale);
        max_h = std::max(max_h, canvas->document->height() * scale);
        out.names.push_back(canvas->name);
    }
    if (max_w <= 0 || max_h <= 0 || max_w > 65535 || max_h > 65535) {
        out.error = "Animation dimensions are invalid for GIF preview.";
        return out;
    }

    std::vector<std::uint32_t> unique;
    std::unordered_map<std::uint32_t, std::uint8_t> exact_map;
    bool exact = true;
    for (const auto* canvas : selected) {
        for (const auto color : canvas->document->pixels()) {
            if ((color >> 24) == 0) continue;
            const auto rgb = color & 0x00ffffffu;
            if (exact_map.find(rgb) != exact_map.end()) continue;
            if (unique.size() >= 255) { exact = false; break; }
            const auto index = static_cast<std::uint8_t>(unique.size() + 1);
            unique.push_back(rgb);
            exact_map.emplace(rgb, index);
        }
        if (!exact) break;
    }

    std::vector<std::uint8_t> bytes;
    const char header[] = "GIF89a";
    bytes.insert(bytes.end(), header, header + 6);
    push_u16(bytes, max_w);
    push_u16(bytes, max_h);
    bytes.push_back(0xF7);
    bytes.push_back(0);
    bytes.push_back(0);

    std::array<std::array<std::uint8_t, 3>, 256> table{};
    if (exact) {
        for (std::size_t i = 0; i < unique.size(); ++i) {
            const auto rgb = unique[i];
            table[i + 1] = {static_cast<std::uint8_t>((rgb >> 16) & 0xffu),
                            static_cast<std::uint8_t>((rgb >> 8) & 0xffu),
                            static_cast<std::uint8_t>(rgb & 0xffu)};
        }
    } else {
        for (int i = 1; i < 256; ++i) {
            const int code = i;
            const int r = (code >> 5) & 0x7;
            const int g = (code >> 2) & 0x7;
            const int b = code & 0x3;
            table[i] = {static_cast<std::uint8_t>((r * 255) / 7),
                        static_cast<std::uint8_t>((g * 255) / 7),
                        static_cast<std::uint8_t>((b * 255) / 3)};
        }
    }
    for (const auto& rgb : table) bytes.insert(bytes.end(), rgb.begin(), rgb.end());

    const std::uint8_t loop_extension[] = {
        0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0',
        0x03, 0x01, 0x00, 0x00, 0x00
    };
    bytes.insert(bytes.end(), std::begin(loop_extension), std::end(loop_extension));

    const int delay = std::max(1, static_cast<int>(100.0 / fps + 0.5));
    auto palette_index = [&](std::uint32_t color) -> std::uint8_t {
        if ((color >> 24) == 0) return 0;
        const auto rgb = color & 0x00ffffffu;
        if (exact) {
            const auto it = exact_map.find(rgb);
            return it == exact_map.end() ? 0 : it->second;
        }
        const int r = static_cast<int>((rgb >> 16) & 0xffu) * 7 / 255;
        const int g = static_cast<int>((rgb >> 8) & 0xffu) * 7 / 255;
        const int b = static_cast<int>(rgb & 0xffu) * 3 / 255;
        int index = (r << 5) | (g << 2) | b;
        if (index == 0) index = 1;
        return static_cast<std::uint8_t>(index);
    };

    for (const auto* canvas : selected) {
        bytes.push_back(0x21); bytes.push_back(0xF9); bytes.push_back(0x04);
        bytes.push_back(0x05);
        push_u16(bytes, delay);
        bytes.push_back(0);
        bytes.push_back(0);

        bytes.push_back(0x2C);
        push_u16(bytes, 0); push_u16(bytes, 0);
        push_u16(bytes, max_w); push_u16(bytes, max_h);
        bytes.push_back(0x00);
        bytes.push_back(8);

        std::vector<std::uint8_t> indices(static_cast<std::size_t>(max_w) * max_h, 0);
        int fw = 0, fh = 0;
        const auto frame = scale_nearest(*canvas->document, scale, fw, fh);
        const int ox = (max_w - fw) / 2;
        const int oy = (max_h - fh) / 2;
        for (int y = 0; y < fh; ++y) {
            for (int x = 0; x < fw; ++x) {
                indices[static_cast<std::size_t>(oy + y) * max_w + ox + x] =
                    palette_index(frame[static_cast<std::size_t>(y) * fw + x]);
            }
        }
        append_gif_subblocks(bytes, gif_lzw_literal_stream(indices));
    }
    bytes.push_back(0x3B);
    out.ok = true;
    out.mime = "image/gif";
    out.bytes = std::move(bytes);
    out.width = max_w;
    out.height = max_h;
    return out;
}

std::uint64_t fnv_media(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64_media(std::uint64_t value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) { out[static_cast<std::size_t>(i)] = hex[value & 0xfu]; value >>= 4; }
    return out;
}

RenderedMedia render_selection(std::vector<PackCanvas*> selected,
                               std::string mode,
                               int scale,
                               int columns,
                               int fps) {
    RenderedMedia out;
    if (selected.empty()) { out.error = "No canvases matched the requested selection."; return out; }
    mode = upper_copy(std::move(mode));
    for (const auto* canvas : selected) out.names.push_back(canvas->name);
    if (mode == "ANIMATION" || mode == "GIF") return encode_gif(selected, scale, fps);

    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    if (mode == "CANVAS" || mode == "SINGLE") {
        pixels = scale_nearest(*selected.front()->document, scale, width, height);
        out.names.resize(1);
    } else if (mode == "STRIP") {
        pixels = compose_sheet(selected, scale, static_cast<int>(selected.size()), true, width, height);
    } else {
        pixels = compose_sheet(selected, scale, columns, false, width, height);
    }
    out = encode_png(pixels, width, height);
    if (out.ok) {
        out.names.clear();
        if (mode == "CANVAS" || mode == "SINGLE") out.names.push_back(selected.front()->name);
        else for (const auto* canvas : selected) out.names.push_back(canvas->name);
    }
    return out;
}

std::string rle_region(const PixelDocument& document, int x, int y, int width, int height) {
    std::ostringstream out;
    bool first = true;
    std::uint32_t current = 0;
    std::size_t count = 0;
    auto flush = [&] {
        if (!count) return;
        if (!first) out << ',';
        first = false;
        out << std::hex << std::uppercase << current << std::dec << 'x' << count;
        count = 0;
    };
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            const auto value = document.pixel(px, py);
            if (count && value != current) flush();
            if (!count) current = value;
            ++count;
        }
    }
    flush();
    return out.str();
}

std::vector<std::string> unique_names(const std::vector<PackCanvas*>& canvases) {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (const auto* canvas : canvases) {
        const auto key = upper_copy(canvas->name);
        if (seen.insert(key).second) names.push_back(canvas->name);
    }
    return names;
}

void push_history(PackWorkspace& workspace, std::vector<std::string> names) {
    if (names.empty()) return;
    workspace.undo_groups.push_back({std::move(names)});
    workspace.redo_groups.clear();
    ++workspace.revision;
}

LocalToolResult media_result(const RenderedMedia& media, std::uint64_t revision, std::string_view mode) {
    if (!media.ok) return {false, "{\"ok\":false,\"error\":\"render_failed\",\"message\":" + quote_json(media.error) + "}"};
    const auto observation = "pack:" + std::string(mode) + ":" + hex64_media(fnv_media(media.bytes));
    const std::string text = "{\"ok\":true,\"revision\":" + std::to_string(revision) +
        ",\"mode\":" + quote_json(mode) + ",\"canvas_count\":" + std::to_string(media.names.size()) +
        ",\"canvases\":" + quote_json(join_names(media.names)) +
        ",\"width\":" + std::to_string(media.width) + ",\"height\":" + std::to_string(media.height) +
        ",\"observation\":" + quote_json(observation) + "}";
    return {true, text, base64_pack(media.bytes), media.mime};
}

} // namespace

LocalToolResult LocalAgentToolSession::call_pack_tool(std::string_view arguments_json) {
    FlatJsonObject args;
    std::string parse_error;
    if (!parse_flat_json_object(arguments_json.empty() ? "{}" : arguments_json, args, parse_error))
        return {false, "{\"ok\":false,\"error\":\"invalid_arguments\"}"};

    const auto task_id_value = args.get_i64("task_id");
    if (!task_id_value || *task_id_value < 0)
        return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};
    const auto task_id = static_cast<std::uint64_t>(*task_id_value);
    const auto action = upper_copy(args.get("action"));

    AgentTaskSnapshot snapshot;
    {
        std::lock_guard state_lock(*bindings_.state_mutex);
        snapshot = bindings_.task->snapshot();
    }
    if (snapshot.id != task_id)
        return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};

    if (action == "CREATE") {
        std::vector<CanvasSpec> specs;
        std::string error;
        if (!parse_specs(args.get("canvases"), specs, error))
            return {false, "{\"ok\":false,\"error\":\"invalid_canvases\",\"message\":" + quote_json(error) + "}"};

        if (snapshot.state == TaskState::AwaitingAgentDecision) {
            const auto accept = call_art_tool("pixelforge_task",
                "{\"action\":\"accept\",\"task_id\":" + std::to_string(task_id) +
                ",\"width\":" + std::to_string(specs.front().width) +
                ",\"height\":" + std::to_string(specs.front().height) + "}");
            if (!accept.success) return accept;
        } else if (snapshot.state != TaskState::Accepted) {
            return {false, "{\"ok\":false,\"error\":\"invalid_state\",\"message\":\"Create a pack while the task is awaiting acceptance or active.\"}"};
        } else {
            std::lock_guard state_lock(*bindings_.state_mutex);
            if (bindings_.document->width() != specs.front().width || bindings_.document->height() != specs.front().height) {
                bool blank = true;
                for (const auto pixel : bindings_.document->pixels()) if (pixel != 0) { blank = false; break; }
                if (!blank)
                    return {false, "{\"ok\":false,\"error\":\"primary_canvas_size_mismatch\",\"message\":\"Primary canvas already contains artwork at a different size.\"}"};
                std::string resize_error;
                if (!bindings_.document->resize(specs.front().width, specs.front().height, &resize_error))
                    return {false, "{\"ok\":false,\"error\":\"resize_failed\",\"message\":" + quote_json(resize_error) + "}"};
            }
        }

        std::lock_guard pack_lock(g_pack_mutex);
        PackWorkspace workspace;
        workspace.task_id = task_id;
        workspace.primary = bindings_.document;
        workspace.revision = 1;
        for (std::size_t i = 0; i < specs.size(); ++i) {
            PackCanvas canvas;
            canvas.name = specs[i].name;
            canvas.group = specs[i].group;
            canvas.frame = specs[i].frame;
            if (i == 0) {
                canvas.document = bindings_.document;
            } else {
                canvas.owned = std::make_unique<PixelDocument>(bindings_.document->limits());
                std::string resize_error;
                if (!canvas.owned->resize(specs[i].width, specs[i].height, &resize_error))
                    return {false, "{\"ok\":false,\"error\":\"resize_failed\",\"message\":" + quote_json(resize_error) + "}"};
                canvas.document = canvas.owned.get();
            }
            workspace.canvases.push_back(std::move(canvas));
        }
        const auto summary = workspace_summary(workspace);
        const auto count = workspace.canvases.size();
        g_pack_workspaces[bindings_.task] = std::move(workspace);
        PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
        return {true, "{\"ok\":true,\"revision\":1,\"canvas_count\":" + std::to_string(count) +
                      ",\"canvases\":" + quote_json(summary) + "}"};
    }

    std::lock_guard pack_lock(g_pack_mutex);
    auto workspace_it = g_pack_workspaces.find(bindings_.task);
    if (workspace_it == g_pack_workspaces.end()) {
        if (snapshot.state != TaskState::Accepted && snapshot.state != TaskState::Finished)
            return {false, "{\"ok\":false,\"error\":\"pack_not_created\"}"};
        PackWorkspace workspace;
        workspace.task_id = task_id;
        workspace.primary = bindings_.document;
        workspace.revision = 1;
        workspace.canvases.push_back({"canvas", "", -1, bindings_.document, nullptr});
        workspace_it = g_pack_workspaces.emplace(bindings_.task, std::move(workspace)).first;
    }
    auto& workspace = workspace_it->second;
    workspace.task_id = task_id;

    if (action == "LIST") {
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"canvas_count\":" + std::to_string(workspace.canvases.size()) +
                      ",\"canvases\":" + quote_json(workspace_summary(workspace)) + "}"};
    }

    if (snapshot.state != TaskState::Accepted && action != "VIEW" && action != "INSPECT" && action != "EXPORT")
        return {false, "{\"ok\":false,\"error\":\"invalid_state\",\"message\":\"Pack edits require an active accepted task.\"}"};

    if (action == "PROGRAM") {
        const auto program = args.get("program");
        std::vector<ProgramSection> sections;
        std::string error;
        const auto default_canvas = args.get("canvas", workspace.canvases.front().name);
        if (!parse_program_sections(program, default_canvas, sections, error))
            return {false, "{\"ok\":false,\"error\":\"invalid_program\",\"message\":" + quote_json(error) + "}"};

        const auto palette_result = call_art_tool("pixelforge_palette", "{\"action\":\"get\"}");
        FlatJsonObject palette_fields;
        std::string ignored;
        std::vector<std::uint32_t> palette;
        if (palette_result.success && parse_flat_json_object(palette_result.text, palette_fields, ignored))
            palette = parse_palette(palette_fields.get("colors"));

        struct Compiled { PackCanvas* canvas; PixelProgramResult result; };
        std::vector<Compiled> compiled;
        {
            std::lock_guard state_lock(*bindings_.state_mutex);
            for (const auto& section : sections) {
                auto* canvas = find_canvas(workspace, section.canvas);
                if (!canvas)
                    return {false, "{\"ok\":false,\"error\":\"unknown_canvas\",\"message\":" + quote_json(section.canvas) + "}"};
                auto result = compile_pixel_program(section.code, canvas->document->width(), canvas->document->height(), canvas->document->pixels());
                if (!result.ok)
                    return {false, "{\"ok\":false,\"error\":\"invalid_program\",\"canvas\":" + quote_json(canvas->name) +
                                  ",\"message\":" + quote_json(result.error) + "}"};
                compiled.push_back({canvas, std::move(result)});
            }

            std::vector<PixelDocument*> applied;
            std::size_t changed_total = 0;
            std::size_t commands = 0;
            std::size_t patch_operations = 0;
            std::size_t clipped = 0;
            for (auto& item : compiled) {
                std::size_t changed = 0;
                if (!apply_patch(*item.canvas->document, item.result.patch, palette, changed, error)) {
                    for (auto it = applied.rbegin(); it != applied.rend(); ++it) (*it)->undo();
                    return {false, "{\"ok\":false,\"error\":\"edit_rejected\",\"canvas\":" + quote_json(item.canvas->name) +
                                  ",\"message\":" + quote_json(error) + "}"};
                }
                if (changed) applied.push_back(item.canvas->document);
                changed_total += changed;
                commands += item.result.commands;
                patch_operations += item.result.patch_operations;
                clipped += item.result.clipped_writes;
            }

            std::vector<PackCanvas*> touched;
            for (auto& item : compiled) touched.push_back(item.canvas);
            const auto names = unique_names(touched);
            if (changed_total) push_history(workspace, names);
            if (std::find_if(touched.begin(), touched.end(), [&](const PackCanvas* c) { return c->document == bindings_.document; }) != touched.end())
                PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);

            LocalToolResult response{true,
                "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                ",\"changed_pixels\":" + std::to_string(changed_total) +
                ",\"commands\":" + std::to_string(commands) +
                ",\"patch_operations\":" + std::to_string(patch_operations) +
                ",\"clipped_writes\":" + std::to_string(clipped) +
                ",\"touched_canvases\":" + std::to_string(names.size()) +
                ",\"canvases\":" + quote_json(join_names(names)) + "}"};

            const auto render_scale = static_cast<int>(args.get_i64("render_scale").value_or(0));
            if (render_scale > 0) {
                auto selected = select_canvases(workspace, join_names(names), "", "");
                auto media = render_selection(std::move(selected), names.size() == 1 ? "canvas" : "sheet", render_scale,
                                              static_cast<int>(args.get_i64("columns").value_or(0)),
                                              static_cast<int>(args.get_i64("fps").value_or(8)));
                if (media.ok) {
                    auto visual = media_result(media, workspace.revision, names.size() == 1 ? "canvas" : "sheet");
                    response.image_base64 = std::move(visual.image_base64);
                    response.image_mime = std::move(visual.image_mime);
                }
            }
            return response;
        }
    }

    if (action == "EDIT") {
        auto* canvas = find_canvas(workspace, args.get("canvas"));
        if (!canvas) return {false, "{\"ok\":false,\"error\":\"unknown_canvas\"}"};
        const auto palette_result = call_art_tool("pixelforge_palette", "{\"action\":\"get\"}");
        FlatJsonObject palette_fields;
        std::string ignored;
        std::vector<std::uint32_t> palette;
        if (palette_result.success && parse_flat_json_object(palette_result.text, palette_fields, ignored))
            palette = parse_palette(palette_fields.get("colors"));
        std::string error;
        std::size_t changed = 0;
        {
            std::lock_guard state_lock(*bindings_.state_mutex);
            if (!apply_patch(*canvas->document, args.get("patch"), palette, changed, error))
                return {false, "{\"ok\":false,\"error\":\"edit_rejected\",\"message\":" + quote_json(error) + "}"};
        }
        if (changed) push_history(workspace, {canvas->name});
        if (canvas->document == bindings_.document) PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"canvas\":" + quote_json(canvas->name) + ",\"changed_pixels\":" + std::to_string(changed) + "}"};
    }

    if (action == "CLONE") {
        auto* source = find_canvas(workspace, args.get("source"));
        auto* dest = find_canvas(workspace, args.get("dest"));
        if (!source || !dest) return {false, "{\"ok\":false,\"error\":\"unknown_canvas\"}"};
        if (source->document->width() != dest->document->width() || source->document->height() != dest->document->height())
            return {false, "{\"ok\":false,\"error\":\"size_mismatch\"}"};
        std::size_t changed = 0;
        {
            std::lock_guard state_lock(*bindings_.state_mutex);
            auto tx = dest->document->begin_transaction();
            for (int y = 0; y < source->document->height(); ++y)
                for (int x = 0; x < source->document->width(); ++x)
                    tx.set_pixel(x, y, source->document->pixel(x, y));
            changed = tx.pending_changes();
            if (changed && !tx.commit()) return {false, "{\"ok\":false,\"error\":\"clone_failed\"}"};
            if (!changed) tx.cancel();
        }
        if (changed) push_history(workspace, {dest->name});
        if (dest->document == bindings_.document) PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"changed_pixels\":" + std::to_string(changed) + "}"};
    }

    if (action == "COPY") {
        auto* source = find_canvas(workspace, args.get("source"));
        auto* dest = find_canvas(workspace, args.get("dest"));
        if (!source || !dest) return {false, "{\"ok\":false,\"error\":\"unknown_canvas\"}"};
        const int sx = static_cast<int>(args.get_i64("sx").value_or(0));
        const int sy = static_cast<int>(args.get_i64("sy").value_or(0));
        const int width = static_cast<int>(args.get_i64("width").value_or(source->document->width()));
        const int height = static_cast<int>(args.get_i64("height").value_or(source->document->height()));
        const int dx = static_cast<int>(args.get_i64("dx").value_or(0));
        const int dy = static_cast<int>(args.get_i64("dy").value_or(0));
        if (!rect_fits(*source->document, sx, sy, width, height) || !rect_fits(*dest->document, dx, dy, width, height))
            return {false, "{\"ok\":false,\"error\":\"copy_out_of_bounds\"}"};
        std::vector<std::uint32_t> source_pixels(static_cast<std::size_t>(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                source_pixels[static_cast<std::size_t>(y) * width + x] = source->document->pixel(sx + x, sy + y);
        std::size_t changed = 0;
        {
            std::lock_guard state_lock(*bindings_.state_mutex);
            auto tx = dest->document->begin_transaction();
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    tx.set_pixel(dx + x, dy + y, source_pixels[static_cast<std::size_t>(y) * width + x]);
            changed = tx.pending_changes();
            if (changed && !tx.commit()) return {false, "{\"ok\":false,\"error\":\"copy_failed\"}"};
            if (!changed) tx.cancel();
        }
        if (changed) push_history(workspace, {dest->name});
        if (dest->document == bindings_.document) PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"changed_pixels\":" + std::to_string(changed) + "}"};
    }

    if (action == "HISTORY") {
        const auto direction = upper_copy(args.get("direction"));
        if (direction == "UNDO") {
            if (workspace.undo_groups.empty()) return {false, "{\"ok\":false,\"error\":\"nothing_to_undo\"}"};
            auto group = std::move(workspace.undo_groups.back());
            workspace.undo_groups.pop_back();
            std::vector<PackCanvas*> changed;
            {
                std::lock_guard state_lock(*bindings_.state_mutex);
                for (const auto& name : group.canvases) {
                    if (auto* canvas = find_canvas(workspace, name); canvas && canvas->document->undo()) changed.push_back(canvas);
                }
            }
            workspace.redo_groups.push_back(std::move(group));
            ++workspace.revision;
            if (std::any_of(changed.begin(), changed.end(), [&](const PackCanvas* c) { return c->document == bindings_.document; }))
                PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
            return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                          ",\"canvases\":" + quote_json(join_names(unique_names(changed))) + "}"};
        }
        if (direction == "REDO") {
            if (workspace.redo_groups.empty()) return {false, "{\"ok\":false,\"error\":\"nothing_to_redo\"}"};
            auto group = std::move(workspace.redo_groups.back());
            workspace.redo_groups.pop_back();
            std::vector<PackCanvas*> changed;
            {
                std::lock_guard state_lock(*bindings_.state_mutex);
                for (const auto& name : group.canvases) {
                    if (auto* canvas = find_canvas(workspace, name); canvas && canvas->document->redo()) changed.push_back(canvas);
                }
            }
            workspace.undo_groups.push_back(std::move(group));
            ++workspace.revision;
            if (std::any_of(changed.begin(), changed.end(), [&](const PackCanvas* c) { return c->document == bindings_.document; }))
                PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
            return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                          ",\"canvases\":" + quote_json(join_names(unique_names(changed))) + "}"};
        }
        return {false, "{\"ok\":false,\"error\":\"unknown_history_direction\"}"};
    }

    if (action == "VIEW") {
        const auto mode = args.get("mode", "sheet");
        auto selected = select_canvases(workspace, args.get("canvases"), args.get("group"),
                                        upper_copy(mode) == "CANVAS" || upper_copy(mode) == "SINGLE" ? args.get("canvas") : "");
        const int scale = static_cast<int>(args.get_i64("scale").value_or(4));
        const int columns = static_cast<int>(args.get_i64("columns").value_or(0));
        const int fps = static_cast<int>(args.get_i64("fps").value_or(8));
        std::lock_guard state_lock(*bindings_.state_mutex);
        return media_result(render_selection(std::move(selected), mode, scale, columns, fps), workspace.revision, mode);
    }

    if (action == "INSPECT") {
        auto* canvas = find_canvas(workspace, args.get("canvas"));
        if (!canvas) return {false, "{\"ok\":false,\"error\":\"unknown_canvas\"}"};
        const int x = static_cast<int>(args.get_i64("x").value_or(0));
        const int y = static_cast<int>(args.get_i64("y").value_or(0));
        const int width = static_cast<int>(args.get_i64("width").value_or(canvas->document->width()));
        const int height = static_cast<int>(args.get_i64("height").value_or(canvas->document->height()));
        if (!rect_fits(*canvas->document, x, y, width, height) ||
            static_cast<std::uint64_t>(width) * height > 4096)
            return {false, "{\"ok\":false,\"error\":\"inspect_region_invalid\"}"};
        std::lock_guard state_lock(*bindings_.state_mutex);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"canvas\":" + quote_json(canvas->name) +
                      ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                      ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
                      ",\"rle\":" + quote_json(rle_region(*canvas->document, x, y, width, height)) + "}"};
    }

    if (action == "EXPORT") {
        const auto scope = upper_copy(args.get("scope", "all"));
        const auto path_text = args.get("path");
        if (path_text.empty()) return {false, "{\"ok\":false,\"error\":\"path_required\"}"};
        const std::filesystem::path target(utf8_to_wide_pack(path_text));
        auto selected = select_canvases(workspace, args.get("canvases"), args.get("group"),
                                        scope == "CANVAS" ? args.get("canvas") : "");
        if (selected.empty()) return {false, "{\"ok\":false,\"error\":\"empty_selection\"}"};
        std::error_code ec;
        std::size_t saved = 0;
        std::wstring save_error;
        std::lock_guard state_lock(*bindings_.state_mutex);
        if (scope == "ALL" || scope == "GROUP") {
            std::filesystem::create_directories(target, ec);
            if (ec) return {false, "{\"ok\":false,\"error\":\"create_directory_failed\"}"};
            for (const auto* canvas : selected) {
                const auto file = target / utf8_to_wide_pack(canvas->name + ".png");
                if (!save_png_wic(file.wstring(), canvas->document->width(), canvas->document->height(), canvas->document->pixels(), save_error))
                    return {false, "{\"ok\":false,\"error\":\"export_failed\"}"};
                ++saved;
            }
        } else if (scope == "ANIMATION" || scope == "GIF") {
            auto media = encode_gif(selected, static_cast<int>(args.get_i64("scale").value_or(1)),
                                    static_cast<int>(args.get_i64("fps").value_or(8)));
            if (!media.ok) return {false, "{\"ok\":false,\"error\":\"export_failed\",\"message\":" + quote_json(media.error) + "}"};
            if (!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path(), ec);
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            if (!file) return {false, "{\"ok\":false,\"error\":\"export_failed\"}"};
            file.write(reinterpret_cast<const char*>(media.bytes.data()), static_cast<std::streamsize>(media.bytes.size()));
            ++saved;
        } else if (scope == "SHEET" || scope == "STRIP") {
            int width = 0, height = 0;
            auto pixels = compose_sheet(selected, static_cast<int>(args.get_i64("scale").value_or(1)),
                                        static_cast<int>(args.get_i64("columns").value_or(0)), scope == "STRIP", width, height);
            if (!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path(), ec);
            if (!save_png_wic(target.wstring(), width, height, pixels, save_error))
                return {false, "{\"ok\":false,\"error\":\"export_failed\"}"};
            ++saved;
        } else {
            const auto* canvas = selected.front();
            if (!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path(), ec);
            if (!save_png_wic(target.wstring(), canvas->document->width(), canvas->document->height(), canvas->document->pixels(), save_error))
                return {false, "{\"ok\":false,\"error\":\"export_failed\"}"};
            ++saved;
        }
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"saved\":" + std::to_string(saved) + ",\"path\":" + quote_json(path_text) + "}"};
    }

    return {false, "{\"ok\":false,\"error\":\"unknown_pack_action\"}"};
}

} // namespace pixelforge::win32
