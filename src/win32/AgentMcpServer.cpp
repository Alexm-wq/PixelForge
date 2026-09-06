#include "AgentMcpServer.hpp"

#include "AgentCommands.hpp"
#include "MiniJson.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pixelforge::win32 {

namespace {

constexpr std::uint64_t kMaxObservationPixels = 16ull * 1024ull * 1024ull;
constexpr std::size_t kMaxPatchOperations = 20000;
constexpr std::size_t kMaxInspectPixels = 4096;

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

bool read_line(HANDLE input, std::string& line) {
    static char buffer[16384];
    static DWORD available = 0, cursor = 0;
    line.clear();
    for (;;) {
        if (cursor == available) {
            if (!ReadFile(input, buffer, sizeof(buffer), &available, nullptr) || available == 0) return !line.empty();
            cursor = 0;
        }
        const char c = buffer[cursor++];
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
        if (line.size() > 8u * 1024u * 1024u) return false;
    }
}

bool write_line(HANDLE output, std::string_view line) {
    std::string framed(line);
    framed.push_back('\n');
    DWORD written = 0;
    return WriteFile(output, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr) &&
           written == framed.size();
}

bool parse_string_token(std::string_view text, std::size_t& p, std::string& decoded) {
    if (p >= text.size() || text[p] != '"') return false;
    ++p;
    decoded.clear();
    while (p < text.size()) {
        const char c = text[p++];
        if (c == '"') return true;
        if (c != '\\') {
            decoded.push_back(c);
            continue;
        }
        if (p >= text.size()) return false;
        const char e = text[p++];
        switch (e) {
            case '"': decoded.push_back('"'); break;
            case '\\': decoded.push_back('\\'); break;
            case '/': decoded.push_back('/'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            default: return false;
        }
    }
    return false;
}

void skip_ws(std::string_view text, std::size_t& p) {
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
}

bool skip_json_value(std::string_view text, std::size_t& p) {
    skip_ws(text, p);
    if (p >= text.size()) return false;
    if (text[p] == '"') {
        std::string ignored;
        return parse_string_token(text, p, ignored);
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
        if (!parse_string_token(object, p, key)) return false;
        skip_ws(object, p);
        if (p >= object.size() || object[p] != ':') return false;
        ++p;
        skip_ws(object, p);
        const std::size_t value_start = p;
        if (!skip_json_value(object, p)) return false;
        std::size_t value_end = p;
        while (value_end > value_start && std::isspace(static_cast<unsigned char>(object[value_end - 1]))) --value_end;
        if (key == wanted) {
            raw.assign(object.substr(value_start, value_end - value_start));
            return true;
        }
        skip_ws(object, p);
        if (p < object.size() && object[p] == ',') ++p;
    }
}

std::string decode_json_scalar_string(std::string_view raw) {
    FlatJsonObject obj;
    std::string error;
    const std::string wrapped = std::string("{\"v\":") + std::string(raw) + "}";
    if (!parse_flat_json_object(wrapped, obj, error)) return {};
    return obj.get("v");
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

std::string hex32(std::uint32_t value) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return ss.str();
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t a = data[i];
        const std::uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const std::uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const std::uint32_t n = (a << 16) | (b << 8) | c;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? table[n & 63] : '=');
    }
    return out;
}

bool read_file_bytes(const std::wstring& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file) || bytes.empty();
}

std::wstring observation_dir() {
    wchar_t temp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    std::filesystem::path dir = n > 0 ? std::filesystem::path(temp) : std::filesystem::temp_directory_path();
    dir /= L"PixelForge";
    dir /= L"observations";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::string state_name_lower(TaskState state) {
    switch (state) {
        case TaskState::Idle: return "idle";
        case TaskState::AwaitingAgentDecision: return "awaiting_agent";
        case TaskState::Accepted: return "accepted";
        case TaskState::Rejected: return "rejected";
        case TaskState::Aborted: return "aborted";
        case TaskState::Finished: return "finished";
    }
    return "unknown";
}

std::string compact_result(const AgentCommandResult& r, bool include_task = false) {
    std::string out = "{\"ok\":" + std::string(r.ok ? "true" : "false") +
        ",\"revision\":" + std::to_string(r.revision) +
        ",\"changed_pixels\":" + std::to_string(r.changed_pixels) +
        ",\"error\":" + json_quote(agent_error_code_name(r.error)) +
        ",\"message\":" + json_quote(r.message);
    if (include_task) {
        out += ",\"task_id\":" + std::to_string(r.task.id) +
               ",\"state\":" + json_quote(state_name_lower(r.task.state)) +
               ",\"prompt\":" + json_quote(r.task.prompt) +
               ",\"status_message\":" + json_quote(r.task.status_message) +
               ",\"canvas_width\":" + std::to_string(r.task.canvas_width) +
               ",\"canvas_height\":" + std::to_string(r.task.canvas_height) +
               ",\"content_reference\":" + json_quote(r.task.content_reference.present ? r.task.content_reference.path : "") +
               ",\"style_reference\":" + json_quote(r.task.style_reference.present ? r.task.style_reference.path : "") +
               ",\"max_width\":" + std::to_string(r.task.limits.max_width) +
               ",\"max_height\":" + std::to_string(r.task.limits.max_height) +
               ",\"max_pixels\":" + std::to_string(r.task.limits.max_pixels);
    }
    out += "}";
    return out;
}

bool parse_int(std::string_view token, int& value) {
    const auto* b = token.data();
    const auto* e = b + token.size();
    const auto result = std::from_chars(b, e, value);
    return result.ec == std::errc{} && result.ptr == e;
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool parse_hex_argb(std::string_view token, std::uint32_t& argb) {
    if (!token.empty() && token.front() == '#') token.remove_prefix(1);
    if (token.size() != 8) return false;
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return false;
    argb = value;
    return true;
}

bool resolve_color(std::string_view token, const std::vector<std::uint32_t>& palette,
                   std::uint32_t& argb, std::string& error) {
    if (!token.empty() && token.front() == '#') {
        if (parse_hex_argb(token, argb)) return true;
        error = "Invalid #AARRGGBB color in patch.";
        return false;
    }
    int index = -1;
    if (!parse_int(token, index) || index < 0 || static_cast<std::size_t>(index) >= palette.size()) {
        error = "Patch color must be a valid palette index or #AARRGGBB.";
        return false;
    }
    argb = palette[static_cast<std::size_t>(index)];
    return true;
}

bool parse_patch(std::string_view patch, const std::vector<std::uint32_t>& palette,
                 std::vector<AgentPixelOp>& operations, std::string& error) {
    operations.clear();
    if (patch.empty()) {
        error = "Patch is empty.";
        return false;
    }
    for (const auto op_text : split(patch, ';')) {
        if (op_text.empty()) continue;
        const auto fields = split(op_text, ',');
        if (fields.empty() || fields[0].size() != 1) {
            error = "Malformed patch operation.";
            return false;
        }
        AgentPixelOp op;
        const char kind = fields[0][0];
        std::uint32_t color = 0;
        auto i = [&](std::size_t n, int& dst) { return n < fields.size() && parse_int(fields[n], dst); };

        if (kind == 'P' && fields.size() == 4) {
            op.kind = AgentPixelOpKind::SetPixel;
            if (!i(1, op.x) || !i(2, op.y) || !resolve_color(fields[3], palette, color, error)) return false;
        } else if (kind == 'H' && fields.size() == 5) {
            op.kind = AgentPixelOpKind::HorizontalRun;
            if (!i(1, op.x) || !i(2, op.y) || !i(3, op.width) || !resolve_color(fields[4], palette, color, error)) return false;
        } else if (kind == 'V' && fields.size() == 5) {
            op.kind = AgentPixelOpKind::VerticalRun;
            if (!i(1, op.x) || !i(2, op.y) || !i(3, op.height) || !resolve_color(fields[4], palette, color, error)) return false;
        } else if (kind == 'R' && fields.size() == 6) {
            op.kind = AgentPixelOpKind::FillRect;
            if (!i(1, op.x) || !i(2, op.y) || !i(3, op.width) || !i(4, op.height) ||
                !resolve_color(fields[5], palette, color, error)) return false;
        } else if (kind == 'L' && fields.size() == 6) {
            op.kind = AgentPixelOpKind::Line;
            if (!i(1, op.x) || !i(2, op.y) || !i(3, op.x2) || !i(4, op.y2) ||
                !resolve_color(fields[5], palette, color, error)) return false;
        } else {
            error = "Unsupported patch operation. Use P/H/V/R/L with the documented field counts.";
            return false;
        }
        op.argb = color;
        operations.push_back(op);
        if (operations.size() > kMaxPatchOperations) {
            error = "Patch exceeds the 20,000-operation transport limit; split it into multiple revisioned batches.";
            return false;
        }
    }
    if (operations.empty()) {
        error = "Patch contains no operations.";
        return false;
    }
    return true;
}

std::vector<std::uint32_t> default_palette() {
    return {
        0x00000000u,
        0xff11181cu, 0xff36505au, 0xff557983u, 0xff78aab0u,
        0xff50c8c8u, 0xff8eece6u, 0xffd8d7cbu, 0xff9b5f66u
    };
}

std::string palette_string(const std::vector<std::uint32_t>& palette) {
    std::string out;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (i) out.push_back(',');
        out += hex32(palette[i]);
    }
    return out;
}

bool parse_palette(std::string_view text, std::vector<std::uint32_t>& palette, std::string& error) {
    std::vector<std::uint32_t> parsed;
    for (auto token : split(text, ',')) {
        if (token.empty()) continue;
        std::uint32_t c = 0;
        if (!parse_hex_argb(token, c)) {
            error = "Palette colors must be comma-separated AARRGGBB values.";
            return false;
        }
        parsed.push_back(c);
        if (parsed.size() > 256) {
            error = "Palette is limited to 256 entries.";
            return false;
        }
    }
    if (parsed.empty()) {
        error = "Palette cannot be empty.";
        return false;
    }
    palette = std::move(parsed);
    return true;
}

std::string color_token(std::uint32_t argb, const std::vector<std::uint32_t>& palette) {
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (palette[i] == argb) return std::to_string(i);
    }
    return "#" + hex32(argb);
}

std::string rle_region(const std::vector<std::uint32_t>& pixels, const std::vector<std::uint32_t>& palette) {
    if (pixels.empty()) return {};
    std::string out;
    std::uint32_t current = pixels[0];
    std::size_t count = 1;
    auto flush = [&]() {
        if (!out.empty()) out.push_back(',');
        out += std::to_string(count);
        out.push_back(':');
        out += color_token(current, palette);
    };
    for (std::size_t i = 1; i < pixels.size(); ++i) {
        if (pixels[i] == current) ++count;
        else {
            flush();
            current = pixels[i];
            count = 1;
        }
    }
    flush();
    return out;
}

std::vector<std::uint32_t> crop_and_scale(const std::vector<std::uint32_t>& source, int source_width,
                                          int x, int y, int width, int height, int scale) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width * scale) * static_cast<std::size_t>(height * scale));
    const int out_width = width * scale;
    for (int oy = 0; oy < height * scale; ++oy) {
        const int sy = y + oy / scale;
        for (int ox = 0; ox < width * scale; ++ox) {
            const int sx = x + ox / scale;
            out[static_cast<std::size_t>(oy) * out_width + ox] =
                source[static_cast<std::size_t>(sy) * source_width + sx];
        }
    }
    return out;
}

std::string mime_for_path(const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
    if (ext == L".bmp") return "image/bmp";
    return "image/png";
}

struct ToolOutput {
    bool error = false;
    std::string text;
    std::string image_base64;
    std::string image_mime;
};

std::string tool_result_json(const ToolOutput& output) {
    std::string content = "[{\"type\":\"text\",\"text\":" + json_quote(output.text) + "}";
    if (!output.image_base64.empty()) {
        content += ",{\"type\":\"image\",\"data\":" + json_quote(output.image_base64) +
                   ",\"mimeType\":" + json_quote(output.image_mime) + "}";
    }
    content += "]";
    return "{\"content\":" + content + ",\"isError\":" + (output.error ? "true" : "false") + "}";
}

std::string tools_list_json() {
    // Six broad tools keep Codex's tool catalog small. High-cardinality operations
    // live inside compact action/patch fields rather than becoming separate tools.
    return R"JSON({"tools":[
{"name":"pixelforge_task","description":"PixelForge task lifecycle. The AGENT, not the editor, decides semantic scope. Use get to read a GUI-created task; accept only pixel-art tasks and choose the canvas size; reject out-of-scope media; abort only after acceptance for a concrete technical blocker; finish when the sprite is complete. begin is available when the task originates from Codex rather than the GUI.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["begin","get","accept","reject","abort","finish"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"prompt":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"},"reason":{"type":"string"},"summary":{"type":"string"},"content_reference":{"type":"string"},"style_reference":{"type":"string"},"known_task":{"type":"integer"},"known_revision":{"type":"integer"},"known_state":{"type":"string"}},"required":["action"]}},
{"name":"pixelforge_edit","description":"Apply one atomic exact-pixel patch. Patch grammar: P,x,y,c; H,x,y,len,c; V,x,y,len,c; R,x,y,w,h,c; L,x0,y0,x1,y1,c. c is a palette index or exact #AARRGGBB. Batch aggressively: one call may contain up to 20,000 operations. Any malformed/out-of-bounds operation cancels the entire batch. Always pass the current revision.","inputSchema":{"type":"object","properties":{"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"patch":{"type":"string"}},"required":["task_id","expected_revision","patch"]}},
{"name":"pixelforge_view","description":"Visual/structural observation. render returns a lossless PNG image of the requested canvas crop with nearest-neighbor integer scaling. content_reference/style_reference return the original reference image without downscaling. inspect returns compact row-major RLE pixels. Pass known_observation to avoid retransmitting an unchanged image.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["render","content_reference","style_reference","inspect"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"scale":{"type":"integer"},"known_observation":{"type":"string"}},"required":["action","task_id"]}},
{"name":"pixelforge_palette","description":"Get or replace the agent's compact color dictionary. Colors are comma-separated AARRGGBB. Palette indices reduce patch size; exact #AARRGGBB colors remain available at all times, so palette compression never reduces color fidelity.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["get","set"]},"colors":{"type":"string"}},"required":["action"]}},
{"name":"pixelforge_history","description":"Revision-guarded undo/redo for the active accepted task.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["undo","redo"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"}},"required":["action","task_id","expected_revision"]}},
{"name":"pixelforge_io","description":"Export the current canvas losslessly to PNG. No resampling or palette quantization is applied.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["export"]},"task_id":{"type":"integer"},"expected_revision":{"type":"integer"},"path":{"type":"string"}},"required":["action","task_id","expected_revision","path"]}}
]})JSON";
}

class Server {
public:
    explicit Server(AgentMcpBindings bindings) : b_(bindings), palette_(default_palette()) {}

    ToolOutput call_tool(std::string_view name, const FlatJsonObject& args) {
        for (const auto key : {"x", "y", "width", "height", "scale"}) {
            if (!args.contains(key)) continue;
            const auto value = args.get_i64(key);
            if (!value || *value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max())
                return {true, "{\"ok\":false,\"error\":\"invalid_integer\"}"};
        }
        if (name == "pixelforge_task") return task(args);
        if (name == "pixelforge_edit") return edit(args);
        if (name == "pixelforge_view") return view(args);
        if (name == "pixelforge_palette") return palette(args);
        if (name == "pixelforge_history") return history(args);
        if (name == "pixelforge_io") return io(args);
        return {true, "{\"ok\":false,\"error\":\"unknown_tool\"}"};
    }

private:
    ToolOutput task(const FlatJsonObject& args) {
        const std::string action = args.get("action");
        if (action == "begin") {
            const std::string prompt = args.get("prompt");
            if (prompt.empty()) return {true, "{\"ok\":false,\"error\":\"prompt_required\"}"};

            ImageData content, style;
            std::wstring content_path, style_path;
            const std::string content_utf8 = args.get("content_reference");
            const std::string style_utf8 = args.get("style_reference");
            std::wstring load_error;
            if (!content_utf8.empty()) {
                content_path = utf8_to_wide(content_utf8);
                if (!load_image_wic(content_path, content, load_error))
                    return {true, "{\"ok\":false,\"error\":\"content_reference_load\",\"message\":" + json_quote(wide_to_utf8(load_error)) + "}"};
            }
            if (!style_utf8.empty()) {
                style_path = utf8_to_wide(style_utf8);
                if (!load_image_wic(style_path, style, load_error))
                    return {true, "{\"ok\":false,\"error\":\"style_reference_load\",\"message\":" + json_quote(wide_to_utf8(load_error)) + "}"};
            }

            std::lock_guard lock(*b_.state_mutex);
            *b_.content_reference = std::move(content);
            *b_.style_reference = std::move(style);
            *b_.content_path = content_path;
            *b_.style_path = style_path;
            ReferenceSlot content_slot, style_slot;
            if (b_.content_reference->valid()) {
                content_slot = {content_utf8, b_.content_reference->width, b_.content_reference->height, true};
            }
            if (b_.style_reference->valid()) {
                style_slot = {style_utf8, b_.style_reference->width, b_.style_reference->height, true};
            }
            b_.task->set_content_reference(std::move(content_slot));
            b_.task->set_style_reference(std::move(style_slot));
            const auto id = b_.task->begin(prompt);
            PostMessageW(b_.hwnd, WM_APP + 1, 0, 0);
            return {false, "{\"ok\":true,\"task_id\":" + std::to_string(id) + ",\"state\":\"awaiting_agent\"}"};
        }

        const auto task_id_opt = args.get_i64("task_id");
        std::lock_guard lock(*b_.state_mutex);
        AgentCommandRouter router(*b_.document, *b_.task);

        if (action == "get") {
            const auto r = router.task_get();
            if (!r.ok) return {true, compact_result(r, true)};
            const auto known_task = args.get_i64("known_task");
            const auto known_rev = args.get_i64("known_revision");
            const auto known_state = args.get("known_state");
            if (known_task && known_rev && *known_task == static_cast<std::int64_t>(r.task.id) &&
                *known_rev == static_cast<std::int64_t>(r.revision) && known_state == state_name_lower(r.task.state)) {
                return {false, "{\"ok\":true,\"unchanged\":true,\"task_id\":" + std::to_string(r.task.id) +
                               ",\"revision\":" + std::to_string(r.revision) + ",\"state\":" + json_quote(known_state) + "}"};
            }
            return {false, compact_result(r, true)};
        }

        if (!task_id_opt || *task_id_opt < 0) return {true, "{\"ok\":false,\"error\":\"task_id_required\"}"};
        const auto task_id = static_cast<std::uint64_t>(*task_id_opt);
        AgentCommandResult r;
        if (action == "accept") {
            const auto w = args.get_i64("width");
            const auto h = args.get_i64("height");
            if (!w || !h) return {true, "{\"ok\":false,\"error\":\"width_height_required\"}"};
            r = router.task_accept(task_id, static_cast<int>(*w), static_cast<int>(*h));
        } else if (action == "reject") {
            r = router.task_reject(task_id, args.get("reason"));
        } else if (action == "abort") {
            r = router.task_abort(task_id, args.get("reason"));
        } else if (action == "finish") {
            const auto rev = args.get_i64("expected_revision");
            if (!rev || *rev < 0) return {true, "{\"ok\":false,\"error\":\"expected_revision_required\"}"};
            r = router.task_finish(task_id, static_cast<std::uint64_t>(*rev), args.get("summary"));
        } else {
            return {true, "{\"ok\":false,\"error\":\"unknown_task_action\"}"};
        }
        PostMessageW(b_.hwnd, WM_APP + 1, 0, 0);
        return {!r.ok, compact_result(r, true)};
    }

    ToolOutput edit(const FlatJsonObject& args) {
        const auto task_id = args.get_i64("task_id");
        const auto rev = args.get_i64("expected_revision");
        if (!task_id || !rev || *task_id < 0 || *rev < 0)
            return {true, "{\"ok\":false,\"error\":\"task_id_and_revision_required\"}"};
        std::vector<AgentPixelOp> operations;
        std::string error;
        if (!parse_patch(args.get("patch"), palette_, operations, error))
            return {true, "{\"ok\":false,\"error\":\"invalid_patch\",\"message\":" + json_quote(error) + "}"};

        std::lock_guard lock(*b_.state_mutex);
        AgentCommandRouter router(*b_.document, *b_.task);
        const auto r = router.edit(static_cast<std::uint64_t>(*task_id), static_cast<std::uint64_t>(*rev), operations);
        PostMessageW(b_.hwnd, WM_APP + 1, 0, 0);
        return {!r.ok, compact_result(r)};
    }

    ToolOutput view(const FlatJsonObject& args) {
        const std::string action = args.get("action");
        const auto task_id = args.get_i64("task_id");
        if (!task_id || *task_id < 0) return {true, "{\"ok\":false,\"error\":\"task_id_required\"}"};

        if (action == "content_reference" || action == "style_reference") {
            ImageData image;
            std::wstring path;
            {
                std::lock_guard lock(*b_.state_mutex);
                if (b_.task->snapshot().id != static_cast<std::uint64_t>(*task_id))
                    return {true, "{\"ok\":false,\"error\":\"stale_task\"}"};
                const bool style = action == "style_reference";
                image = style ? *b_.style_reference : *b_.content_reference;
                path = style ? *b_.style_path : *b_.content_path;
            }
            if (!image.valid() || path.empty()) return {true, "{\"ok\":false,\"error\":\"reference_missing\"}"};
            auto hash = fnv1a(image.bgra.data(), image.bgra.size());
            hash = fnv1a(&image.width, sizeof(image.width), hash);
            hash = fnv1a(&image.height, sizeof(image.height), hash);
            const std::string obs = std::string(style_name(action)) + ":" + hex64(hash);
            if (args.get("known_observation") == obs)
                return {false, "{\"ok\":true,\"unchanged\":true,\"observation\":" + json_quote(obs) + "}"};
            // Serve the decoded snapshot shown in the GUI, even if the source
            // file changes or disappears after loading it.
            std::filesystem::path snapshot = observation_dir();
            snapshot /= utf8_to_wide("reference-" + hex64(hash) + ".png");
            if (!std::filesystem::exists(snapshot)) {
                std::vector<std::uint32_t> pixels(image.bgra.size() / 4);
                for (std::size_t i = 0; i < pixels.size(); ++i) {
                    const auto* p = image.bgra.data() + i * 4;
                    pixels[i] = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
                }
                std::wstring error;
                if (!save_png_wic(snapshot.wstring(), image.width, image.height, pixels, error))
                    return {true, "{\"ok\":false,\"error\":\"reference_encode_failed\"}"};
            }
            std::vector<std::uint8_t> bytes;
            if (!read_file_bytes(snapshot.wstring(), bytes)) return {true, "{\"ok\":false,\"error\":\"reference_read_failed\"}"};
            ToolOutput out;
            out.text = "{\"ok\":true,\"observation\":" + json_quote(obs) +
                       ",\"width\":" + std::to_string(image.width) + ",\"height\":" + std::to_string(image.height) +
                       ",\"path\":" + json_quote(wide_to_utf8(path)) + "}";
            out.image_base64 = base64_encode(bytes);
            out.image_mime = "image/png";
            return out;
        }

        const auto rev = args.get_i64("expected_revision");
        if (!rev || *rev < 0) return {true, "{\"ok\":false,\"error\":\"expected_revision_required\"}"};

        if (action == "inspect") {
            const int x = static_cast<int>(args.get_i64("x").value_or(0));
            const int y = static_cast<int>(args.get_i64("y").value_or(0));
            int w = static_cast<int>(args.get_i64("width").value_or(0));
            int h = static_cast<int>(args.get_i64("height").value_or(0));
            std::lock_guard lock(*b_.state_mutex);
            if (w == 0) w = b_.document->width();
            if (h == 0) h = b_.document->height();
            if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaxInspectPixels)
                return {true, "{\"ok\":false,\"error\":\"inspect_too_large\",\"message\":\"Use render for regions over 4096 pixels.\"}"};
            AgentCommandRouter router(*b_.document, *b_.task);
            const auto obs = router.inspect_region(static_cast<std::uint64_t>(*task_id), static_cast<std::uint64_t>(*rev), x, y, w, h);
            if (!obs.ok) return {true, "{\"ok\":false,\"error\":" + json_quote(agent_error_code_name(obs.error)) +
                                           ",\"message\":" + json_quote(obs.message) + "}"};
            return {false, "{\"ok\":true,\"revision\":" + std::to_string(obs.revision) +
                           ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                           ",\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) +
                           ",\"encoding\":\"row_major_rle\",\"rle\":" + json_quote(rle_region(obs.pixels, palette_)) + "}"};
        }

        if (action == "render") {
            const int x = static_cast<int>(args.get_i64("x").value_or(0));
            const int y = static_cast<int>(args.get_i64("y").value_or(0));
            const int scale = static_cast<int>(args.get_i64("scale").value_or(8));
            int w = static_cast<int>(args.get_i64("width").value_or(0));
            int h = static_cast<int>(args.get_i64("height").value_or(0));
            std::vector<std::uint32_t> pixels;
            int source_width = 0;
            {
                std::lock_guard lock(*b_.state_mutex);
                AgentCommandRouter router(*b_.document, *b_.task);
                const auto check = router.task_get();
                if (!check.ok || check.task.id != static_cast<std::uint64_t>(*task_id))
                    return {true, "{\"ok\":false,\"error\":\"stale_task\"}"};
                if (check.task.state != TaskState::Accepted && check.task.state != TaskState::Finished)
                    return {true, "{\"ok\":false,\"error\":\"invalid_state\"}"};
                if (check.revision != static_cast<std::uint64_t>(*rev))
                    return {true, "{\"ok\":false,\"error\":\"stale_revision\",\"revision\":" + std::to_string(check.revision) + "}"};
                source_width = b_.document->width();
                if (w == 0) w = source_width;
                if (h == 0) h = b_.document->height();
                if (scale < 1 || scale > 32 || x < 0 || y < 0 || w <= 0 || h <= 0 ||
                    w > b_.document->width() || h > b_.document->height() ||
                    x > b_.document->width() - w || y > b_.document->height() - h)
                    return {true, "{\"ok\":false,\"error\":\"invalid_render_region\"}"};
                const std::uint64_t out_pixels = static_cast<std::uint64_t>(w) * h * scale * scale;
                if (out_pixels > kMaxObservationPixels)
                    return {true, "{\"ok\":false,\"error\":\"render_too_large\",\"message\":\"Crop the render or use a smaller integer scale.\"}"};
                pixels = b_.document->pixels();
            }

            const std::string key = hex64(fnv1a(pixels.data(), pixels.size() * sizeof(std::uint32_t))) + ":" +
                std::to_string(source_width) + ":" + std::to_string(*task_id) + ":" + std::to_string(*rev) + ":" +
                std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(w) + ":" +
                std::to_string(h) + ":" + std::to_string(scale);
            const std::uint64_t key_hash = fnv1a(key.data(), key.size());
            const std::string obs = "canvas:" + std::to_string(*rev) + ":" + hex64(key_hash);
            if (args.get("known_observation") == obs)
                return {false, "{\"ok\":true,\"unchanged\":true,\"observation\":" + json_quote(obs) +
                               ",\"revision\":" + std::to_string(*rev) + "}"};

            const auto scaled = crop_and_scale(pixels, source_width, x, y, w, h, scale);
            std::filesystem::path file = observation_dir();
            file /= utf8_to_wide(hex64(key_hash) + ".png");
            if (!std::filesystem::exists(file)) {
                std::wstring error;
                if (!save_png_wic(file.wstring(), w * scale, h * scale, scaled, error))
                    return {true, "{\"ok\":false,\"error\":\"render_write_failed\",\"message\":" + json_quote(wide_to_utf8(error)) + "}"};
            }
            std::vector<std::uint8_t> bytes;
            if (!read_file_bytes(file.wstring(), bytes)) return {true, "{\"ok\":false,\"error\":\"render_read_failed\"}"};
            ToolOutput out;
            out.text = "{\"ok\":true,\"observation\":" + json_quote(obs) +
                       ",\"revision\":" + std::to_string(*rev) + ",\"width\":" + std::to_string(w * scale) +
                       ",\"height\":" + std::to_string(h * scale) + ",\"source_x\":" + std::to_string(x) +
                       ",\"source_y\":" + std::to_string(y) + ",\"source_width\":" + std::to_string(w) +
                       ",\"source_height\":" + std::to_string(h) + ",\"scale\":" + std::to_string(scale) +
                       ",\"path\":" + json_quote(wide_to_utf8(file.wstring())) + "}";
            out.image_base64 = base64_encode(bytes);
            out.image_mime = "image/png";
            return out;
        }

        return {true, "{\"ok\":false,\"error\":\"unknown_view_action\"}"};
    }

    static const char* style_name(const std::string& action) {
        return action == "style_reference" ? "style" : "content";
    }

    ToolOutput palette(const FlatJsonObject& args) {
        const std::string action = args.get("action");
        if (action == "get")
            return {false, "{\"ok\":true,\"count\":" + std::to_string(palette_.size()) +
                           ",\"colors\":" + json_quote(palette_string(palette_)) + "}"};
        if (action == "set") {
            std::string error;
            if (!parse_palette(args.get("colors"), palette_, error))
                return {true, "{\"ok\":false,\"error\":\"invalid_palette\",\"message\":" + json_quote(error) + "}"};
            return {false, "{\"ok\":true,\"count\":" + std::to_string(palette_.size()) + "}"};
        }
        return {true, "{\"ok\":false,\"error\":\"unknown_palette_action\"}"};
    }

    ToolOutput history(const FlatJsonObject& args) {
        const auto task_id = args.get_i64("task_id");
        const auto rev = args.get_i64("expected_revision");
        if (!task_id || !rev || *task_id < 0 || *rev < 0)
            return {true, "{\"ok\":false,\"error\":\"task_id_and_revision_required\"}"};
        std::lock_guard lock(*b_.state_mutex);
        AgentCommandRouter router(*b_.document, *b_.task);
        AgentCommandResult r;
        if (args.get("action") == "undo") r = router.history_undo(static_cast<std::uint64_t>(*task_id), static_cast<std::uint64_t>(*rev));
        else if (args.get("action") == "redo") r = router.history_redo(static_cast<std::uint64_t>(*task_id), static_cast<std::uint64_t>(*rev));
        else return {true, "{\"ok\":false,\"error\":\"unknown_history_action\"}"};
        PostMessageW(b_.hwnd, WM_APP + 1, 0, 0);
        return {!r.ok, compact_result(r)};
    }

    ToolOutput io(const FlatJsonObject& args) {
        if (args.get("action") != "export") return {true, "{\"ok\":false,\"error\":\"unknown_io_action\"}"};
        const auto task_id = args.get_i64("task_id");
        const auto rev = args.get_i64("expected_revision");
        const std::string path_utf8 = args.get("path");
        if (!task_id || !rev || *task_id < 0 || *rev < 0 || path_utf8.empty())
            return {true, "{\"ok\":false,\"error\":\"task_revision_path_required\"}"};
        int width = 0, height = 0;
        std::vector<std::uint32_t> pixels;
        {
            std::lock_guard lock(*b_.state_mutex);
            const auto snap = b_.task->snapshot();
            if (snap.id != static_cast<std::uint64_t>(*task_id)) return {true, "{\"ok\":false,\"error\":\"stale_task\"}"};
            if (snap.state != TaskState::Accepted && snap.state != TaskState::Finished)
                return {true, "{\"ok\":false,\"error\":\"invalid_state\"}"};
            if (snap.document_revision != static_cast<std::uint64_t>(*rev))
                return {true, "{\"ok\":false,\"error\":\"stale_revision\",\"revision\":" + std::to_string(snap.document_revision) + "}"};
            width = b_.document->width();
            height = b_.document->height();
            pixels = b_.document->pixels();
        }
        std::wstring error;
        const auto path = utf8_to_wide(path_utf8);
        if (!save_png_wic(path, width, height, pixels, error))
            return {true, "{\"ok\":false,\"error\":\"export_failed\",\"message\":" + json_quote(wide_to_utf8(error)) + "}"};
        return {false, "{\"ok\":true,\"path\":" + json_quote(path_utf8) + ",\"width\":" +
                       std::to_string(width) + ",\"height\":" + std::to_string(height) + "}"};
    }

    AgentMcpBindings b_;
    std::vector<std::uint32_t> palette_;
};

std::string make_response(std::string_view id_raw, std::string_view result_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id_raw) + ",\"result\":" + std::string(result_json) + "}";
}

std::string make_error(std::string_view id_raw, int code, std::string_view message) {
    const std::string id = id_raw.empty() ? "null" : std::string(id_raw);
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":" + json_quote(message) + "}}";
}

} // namespace

int run_mcp_stdio(AgentMcpBindings bindings) {
    if (!bindings.document || !bindings.task || !bindings.content_reference || !bindings.style_reference ||
        !bindings.content_path || !bindings.style_path || !bindings.state_mutex) return 2;

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) return 3;

    Server server(bindings);
    std::string line;
    while (read_line(input, line)) {
        if (line.empty()) continue;
        std::string method_raw, id_raw, params_raw;
        if (!extract_member_raw(line, "method", method_raw)) {
            write_line(output, make_error("null", -32600, "Invalid MCP request: method missing."));
            continue;
        }
        const std::string method = decode_json_scalar_string(method_raw);
        extract_member_raw(line, "id", id_raw);
        extract_member_raw(line, "params", params_raw);

        // Notifications do not have ids. They are intentionally silent.
        const bool notification = id_raw.empty();
        if (method == "notifications/initialized" || method == "notifications/cancelled") continue;

        if (method == "initialize") {
            if (notification) continue;
            std::string version = "2025-11-25";
            if (!params_raw.empty()) {
                std::string requested;
                if (extract_member_raw(params_raw, "protocolVersion", requested)) {
                    const auto decoded = decode_json_scalar_string(requested);
                    if (!decoded.empty()) version = decoded;
                }
            }
            const std::string result = "{\"protocolVersion\":" + json_quote(version) +
                ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"PixelForge\",\"version\":\"0.2.0\"}}";
            write_line(output, make_response(id_raw, result));
            continue;
        }

        if (method == "server/discover") {
            if (!notification)
                write_line(output, make_response(id_raw, "{\"protocolVersion\":\"2026-07-28\",\"capabilities\":{\"tools\":{}}}"));
            continue;
        }

        if (method == "ping") {
            if (!notification) write_line(output, make_response(id_raw, "{}"));
            continue;
        }

        if (method == "tools/list") {
            if (!notification) write_line(output, make_response(id_raw, tools_list_json()));
            continue;
        }

        if (method == "tools/call") {
            if (notification) continue;
            std::string name_raw, args_raw;
            if (params_raw.empty() || !extract_member_raw(params_raw, "name", name_raw)) {
                write_line(output, make_error(id_raw, -32602, "tools/call requires params.name."));
                continue;
            }
            const std::string name = decode_json_scalar_string(name_raw);
            if (!extract_member_raw(params_raw, "arguments", args_raw)) args_raw = "{}";
            FlatJsonObject args;
            std::string parse_error;
            if (!parse_flat_json_object(args_raw, args, parse_error)) {
                ToolOutput out{true, "{\"ok\":false,\"error\":\"invalid_arguments\",\"message\":" + json_quote(parse_error) + "}"};
                write_line(output, make_response(id_raw, tool_result_json(out)));
                continue;
            }
            const auto out = server.call_tool(name, args);
            write_line(output, make_response(id_raw, tool_result_json(out)));
            continue;
        }

        if (!notification) write_line(output, make_error(id_raw, -32601, "Method not found."));
    }

    if (bindings.hwnd) PostMessageW(bindings.hwnd, WM_CLOSE, 0, 0);
    return 0;
}

} // namespace pixelforge::win32
