#include "LocalAgentToolSession.hpp"

#define call base_call
#define pixelforge_dynamic_tools_json base_pixelforge_dynamic_tools_json
#include "LocalAgentToolSession.cpp"
#undef pixelforge_dynamic_tools_json
#undef call

#include "AgentInteractionUi.hpp"
#include "ImageIO.hpp"
#include "MiniJson.hpp"
#include "PackWorkspaceUi.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pixelforge::win32 {
namespace {

std::string q(std::string_view text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(static_cast<char>(c));
    }
    out += '"';
    return out;
}

std::wstring utf8_to_wide_ws(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string wide_to_utf8_ws(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::uint32_t argb_at(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[3]) << 24) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[0]);
}

std::vector<std::uint32_t> argb_pixels(const ImageData& image) {
    std::vector<std::uint32_t> out;
    if (!image.valid()) return out;
    const auto count = static_cast<std::size_t>(image.width) * image.height;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = argb_at(image.bgra.data() + i * 4u);
    return out;
}

bool seed_document(PixelDocument& document, const ImageData& image, std::string& error) {
    if (!image.valid()) { error = "Reference image is unavailable."; return false; }
    if (document.width() != image.width || document.height() != image.height) {
        error = "Exact reference seeding requires matching canvas dimensions.";
        return false;
    }
    const auto pixels = argb_pixels(image);
    auto tx = document.begin_transaction();
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            if (!tx.set_pixel(x, y, pixels[static_cast<std::size_t>(y) * image.width + x])) {
                error = "Could not seed reference pixels.";
                return false;
            }
    tx.commit();
    return true;
}

std::string palette_for(const ImageData& image, std::size_t max_colors, std::size_t& unique) {
    std::map<std::uint32_t, std::uint64_t> counts;
    const auto count = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t i = 0; i < count; ++i) ++counts[argb_at(image.bgra.data() + i * 4u)];
    unique = counts.size();
    std::vector<std::pair<std::uint32_t, std::uint64_t>> ranked(counts.begin(), counts.end());
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (ranked.size() > max_colors) ranked.resize(max_colors);
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        if (i) out << ',';
        out << std::setw(8) << ranked[i].first;
    }
    return out.str();
}

bool get_reference(const AgentMcpBindings& bindings, std::string reference, ImageData& image) {
    std::transform(reference.begin(), reference.end(), reference.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (reference == "source") { std::wstring path; return agent_interaction_source_snapshot(image, path); }
    if (reference == "content" && bindings.content_reference && bindings.content_reference->valid()) { image = *bindings.content_reference; return true; }
    if (reference == "style" && bindings.style_reference && bindings.style_reference->valid()) { image = *bindings.style_reference; return true; }
    return false;
}

bool add_member(std::string& json, std::string member) {
    const auto end = json.find_last_of('}');
    if (end == std::string::npos) return false;
    json.insert(end, std::move(member));
    return true;
}

bool flag_false(const FlatJsonObject& args, std::string_view key) {
    return args.contains(key) && !args.get_bool(key).value_or(true);
}

std::vector<std::string> split_keep_empty_ws(std::string_view text, char delimiter) {
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

std::string join_pipe(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out.push_back('|');
        out += values[i];
    }
    return out;
}

std::vector<PackUiCanvasInfo> parse_pack_summary(std::string_view summary) {
    std::vector<PackUiCanvasInfo> out;
    for (const auto& row : split_keep_empty_ws(summary, '|')) {
        if (row.empty()) continue;
        const auto fields = split_keep_empty_ws(row, ',');
        if (fields.size() != 5) continue;
        PackUiCanvasInfo info;
        info.name = fields[0];
        info.group = fields[1];
        try {
            info.width = std::stoi(fields[2]);
            info.height = std::stoi(fields[3]);
            info.frame = std::stoi(fields[4]);
        } catch (...) { continue; }
        out.push_back(std::move(info));
    }
    return out;
}

std::string base64_encode_ws(const std::vector<std::uint8_t>& bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t n = (a << 16) | (b << 8) | c;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? table[n & 63] : '=');
    }
    return out;
}

bool encode_source_png(const ImageData& image, std::string& base64, std::string& error) {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / L"PixelForge" / L"source-observations";
    std::filesystem::create_directories(dir, ec);
    if (ec) { error = "Could not create the source observation directory."; return false; }
    const auto file = dir / (L"source-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".png");
    std::wstring save_error;
    if (!save_png_wic(file.wstring(), image.width, image.height, argb_pixels(image), save_error)) {
        error = wide_to_utf8_ws(save_error);
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) { error = "Could not read the source observation PNG."; return false; }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(file, ec);
    base64 = base64_encode_ws(bytes);
    return true;
}

bool set_json_string_member(std::string& json, std::string_view key, std::string_view value) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) {
        const auto end = json.find_last_of('}');
        if (end == std::string::npos) return false;
        json.insert(end, (end > 0 && json[end - 1] != '{' ? "," : "") + needle + ":" + q(value));
        return true;
    }
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return false;
    const auto start = p;
    ++p;
    bool escaped = false;
    while (p < json.size()) {
        const char c = json[p++];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') break;
    }
    json.replace(start, p - start, q(value));
    return true;
}

bool set_json_integer_member(std::string& json, std::string_view key, std::int64_t value) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto p = json.find(needle);
    const std::string replacement = std::to_string(value);
    if (p == std::string::npos) {
        const auto end = json.find_last_of('}');
        if (end == std::string::npos) return false;
        json.insert(end, (end > 0 && json[end - 1] != '{' ? "," : "") + needle + ":" + replacement);
        return true;
    }
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    const auto start = p;
    if (p < json.size() && (json[p] == '-' || json[p] == '+')) ++p;
    while (p < json.size() && std::isdigit(static_cast<unsigned char>(json[p]))) ++p;
    json.replace(start, p - start, replacement);
    return true;
}

std::string add_optional_i64(const FlatJsonObject& args, std::string_view key) {
    const auto value = args.get_i64(key);
    return value ? ",\"" + std::string(key) + "\":" + std::to_string(*value) : std::string{};
}

void replace_once(std::string& text, std::string_view from, std::string_view to) {
    const auto pos = text.find(from);
    if (pos != std::string::npos) text.replace(pos, from.size(), to);
}

} // namespace

LocalToolResult LocalAgentToolSession::call(std::string_view tool, std::string_view arguments_json) {
    agent_interaction_bind_task(bindings_.task, bindings_.state_mutex, bindings_.hwnd);

    FlatJsonObject args;
    std::string parse_error;
    if (!parse_flat_json_object(arguments_json.empty() ? "{}" : arguments_json, args, parse_error))
        return {false, "{\"ok\":false,\"error\":\"invalid_arguments\"}"};

    auto workspace_root = [&]() {
        std::filesystem::path recordings(record_bindings_.recording_directory);
        if (!recordings.empty() && !recordings.parent_path().empty()) return recordings.parent_path();
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) return cwd;
        return std::filesystem::temp_directory_path(ec) / L"PixelForge";
    };

    auto project_directory = [&](std::uint64_t task_id) {
        return workspace_root() / L"projects" / (L"task_" + std::to_wstring(task_id));
    };

    auto resolve_export_path = [&](std::uint64_t task_id, std::string_view requested_utf8) {
        std::filesystem::path requested(utf8_to_wide_ws(requested_utf8));
        if (requested.is_absolute()) return requested;
        std::filesystem::path tail;
        auto it = requested.begin();
        if (it != requested.end() && _wcsicmp(it->c_str(), L"exports") == 0) ++it;
        else it = requested.begin();
        for (; it != requested.end(); ++it) tail /= *it;
        auto target = project_directory(task_id) / L"exports" / tail;
        std::error_code ec;
        std::filesystem::create_directories((target.has_extension() ? target.parent_path() : target), ec);
        return target;
    };

    auto refresh_pack = [&](std::uint64_t task_id) {
        const auto list = base_call("pixelforge_pack", "{\"action\":\"list\",\"task_id\":" + std::to_string(task_id) + "}");
        if (!list.success) return;
        FlatJsonObject fields;
        std::string ignored;
        if (!parse_flat_json_object(list.text, fields, ignored)) return;
        const auto canvases = parse_pack_summary(fields.get("canvases"));
        const auto revision = static_cast<std::uint64_t>(std::max<std::int64_t>(0, fields.get_i64("revision").value_or(0)));
        const auto project = project_directory(task_id);
        std::error_code ec;
        std::filesystem::create_directories(project / L"canvases", ec);
        std::filesystem::create_directories(project / L"previews", ec);
        std::filesystem::create_directories(project / L"exports", ec);

        std::map<std::string, std::vector<std::string>> groups;
        std::set<std::string> animated;
        for (const auto& canvas : canvases) {
            groups[canvas.group].push_back(canvas.name);
            if (canvas.frame >= 0 && !canvas.group.empty()) animated.insert(canvas.group);
        }
        for (const auto& [group, names] : groups) {
            const std::wstring folder = group.empty() ? L"ungrouped" : utf8_to_wide_ws(group);
            const auto target = project / L"canvases" / folder;
            std::filesystem::create_directories(target, ec);
            const std::string request = "{\"action\":\"export\",\"scope\":\"group\",\"canvases\":" + q(join_pipe(names)) +
                ",\"path\":" + q(wide_to_utf8_ws(target.wstring())) + ",\"task_id\":" + std::to_string(task_id) + "}";
            base_call("pixelforge_pack", request);
        }
        for (const auto& group : animated) {
            const auto target = project / L"previews" / (utf8_to_wide_ws(group) + L"_strip.png");
            const std::string request = "{\"action\":\"export\",\"scope\":\"strip\",\"group\":" + q(group) +
                ",\"path\":" + q(wide_to_utf8_ws(target.wstring())) + ",\"scale\":1,\"task_id\":" + std::to_string(task_id) + "}";
            base_call("pixelforge_pack", request);
        }

        std::ofstream meta(project / L"project.json", std::ios::binary | std::ios::trunc);
        if (meta) {
            meta << "{\n  \"version\": 1,\n  \"task_id\": " << task_id
                 << ",\n  \"pack_revision\": " << revision << ",\n  \"canvases\": [\n";
            for (std::size_t i = 0; i < canvases.size(); ++i) {
                const auto& c = canvases[i];
                const std::string folder = c.group.empty() ? "ungrouped" : c.group;
                meta << "    {\"name\":" << q(c.name) << ",\"group\":" << q(c.group)
                     << ",\"frame\":" << c.frame << ",\"width\":" << c.width << ",\"height\":" << c.height
                     << ",\"file\":" << q("canvases/" + folder + "/" + c.name + ".png") << "}";
                if (i + 1 != canvases.size()) meta << ',';
                meta << '\n';
            }
            meta << "  ]\n}\n";
        }
        pack_workspace_ui_publish(bindings_.hwnd, task_id, revision, project.wstring(), canvases);
    };

    if (tool == "pixelforge_dialog") {
        const auto task_id = args.get_i64("task_id");
        if (!task_id) return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};
        const std::string reason = args.get("reason"), question = args.get("question"), suggestions = args.get("suggestions");
        std::string state_error;
        {
            std::lock_guard lock(*bindings_.state_mutex);
            if (bindings_.task->snapshot().id != static_cast<std::uint64_t>(*task_id))
                return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};
            if (!bindings_.task->request_user_input(reason, question, suggestions, &state_error))
                return {false, "{\"ok\":false,\"error\":\"dialog_rejected\",\"message\":" + q(state_error) + "}"};
        }
        std::string answer;
        bool cancelled = false;
        std::wstring ui_error;
        if (!agent_interaction_ask_user(reason, question, suggestions, answer, cancelled, ui_error)) {
            std::lock_guard lock(*bindings_.state_mutex);
            bindings_.task->user_answer_input("PixelForge could not open the user dialog.", nullptr);
            return {false, "{\"ok\":false,\"error\":\"dialog_ui_failed\"}"};
        }
        {
            std::lock_guard lock(*bindings_.state_mutex);
            bindings_.task->user_answer_input(cancelled ? "User dismissed the popup without an answer." : answer, nullptr);
        }
        return {true, "{\"ok\":true,\"cancelled\":" + std::string(cancelled ? "true" : "false") +
                      ",\"answer\":" + q(answer) +
                      ",\"message\":\"Continue this same turn using the user's answer.\"}"};
    }

    if (tool == "pixelforge_reference") {
        const auto task_id = args.get_i64("task_id");
        if (!task_id) return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};
        {
            std::lock_guard lock(*bindings_.state_mutex);
            if (bindings_.task->snapshot().id != static_cast<std::uint64_t>(*task_id))
                return {false, "{\"ok\":false,\"error\":\"stale_task\"}"};
        }
        const std::string reference = args.get("reference"), action = args.get("action");
        ImageData image;
        if (!get_reference(bindings_, reference, image))
            return {false, "{\"ok\":false,\"error\":\"reference_unavailable\"}"};
        if (action == "palette") {
            const auto n = static_cast<std::size_t>(std::clamp<std::int64_t>(args.get_i64("max_colors").value_or(20), 1, 256));
            std::size_t unique = 0;
            const auto colors = palette_for(image, n, unique);
            return {true, "{\"ok\":true,\"reference\":" + q(reference) + ",\"width\":" + std::to_string(image.width) +
                          ",\"height\":" + std::to_string(image.height) + ",\"unique_colors\":" + std::to_string(unique) +
                          ",\"colors\":" + q(colors) + "}"};
        }
        if (action == "seed") {
            std::string error;
            {
                std::lock_guard lock(*bindings_.state_mutex);
                if (bindings_.task->snapshot().state != TaskState::Accepted)
                    return {false, "{\"ok\":false,\"error\":\"task_not_accepted\"}"};
                if (!seed_document(*bindings_.document, image, error))
                    return {false, "{\"ok\":false,\"error\":\"seed_failed\",\"message\":" + q(error) + "}"};
                observed_revision_ = bindings_.document->revision();
                has_observed_revision_ = true;
            }
            return {true, "{\"ok\":true,\"seeded_from\":" + q(reference) + ",\"revision\":" + std::to_string(observed_revision_) + "}"};
        }
        if (action == "view") {
            if (reference == "content") return base_call("pixelforge_view", "{\"action\":\"content_reference\",\"task_id\":" + std::to_string(*task_id) + "}");
            if (reference == "style") return base_call("pixelforge_view", "{\"action\":\"style_reference\",\"task_id\":" + std::to_string(*task_id) + "}");
            std::string png, error;
            if (!encode_source_png(image, png, error))
                return {true, "{\"ok\":true,\"reference\":\"source\",\"width\":" + std::to_string(image.width) +
                              ",\"height\":" + std::to_string(image.height) + ",\"warning\":" + q(error) + "}"};
            return {true, "{\"ok\":true,\"reference\":\"source\",\"width\":" + std::to_string(image.width) +
                          ",\"height\":" + std::to_string(image.height) + ",\"observation\":\"source:image\"}",
                    std::move(png), "image/png"};
        }
        return {false, "{\"ok\":false,\"error\":\"unknown_reference_action\"}"};
    }

    if (tool == "pixelforge_task" && args.get("action") == "get") {
        auto result = base_call(tool, arguments_json);
        if (!result.success) return result;
        ImageData source; std::wstring path;
        if (agent_interaction_source_snapshot(source, path))
            add_member(result.text, ",\"source_present\":true,\"source_width\":" + std::to_string(source.width) +
                                    ",\"source_height\":" + std::to_string(source.height) +
                                    ",\"source_semantics\":\"editable image; seed and modify it unless instructed otherwise\"");
        else add_member(result.text, ",\"source_present\":false");
        return result;
    }

    if (tool == "pixelforge_task" && args.get("action") == "accept") {
        AgentTaskSnapshot before;
        { std::lock_guard lock(*bindings_.state_mutex); before = bindings_.task->snapshot(); }
        ImageData source; std::wstring path;
        const bool has_source = agent_interaction_source_snapshot(source, path);
        const bool fresh = before.state == TaskState::AwaitingAgentDecision && !before.continuation_pending;
        const bool seed_source = has_source && fresh && !flag_false(args, "seed_from_source");
        std::string forwarded(arguments_json);
        if (seed_source) {
            const auto requested_w = args.get_i64("width");
            const auto requested_h = args.get_i64("height");
            if ((requested_w && *requested_w != source.width) || (requested_h && *requested_h != source.height))
                return {false, "{\"ok\":false,\"error\":\"source_size_mismatch\",\"message\":\"Exact Source editing starts at Source dimensions. Ask the user with pixelforge_dialog or explicitly set seed_from_source=false if a different size is intentional.\"}"};
            if (!args.contains("width")) set_json_integer_member(forwarded, "width", source.width);
            if (!args.contains("height")) set_json_integer_member(forwarded, "height", source.height);
        }
        auto result = base_call(tool, forwarded);
        if (!result.success) return result;
        if (seed_source) {
            std::string error;
            std::lock_guard lock(*bindings_.state_mutex);
            if (!seed_document(*bindings_.document, source, error))
                return {false, "{\"ok\":false,\"error\":\"source_seed_failed\",\"message\":" + q(error) + "}"};
            observed_revision_ = bindings_.document->revision();
            has_observed_revision_ = true;
            add_member(result.text, ",\"seeded_from\":\"source\",\"canvas_blank\":false,\"actual_revision\":" + std::to_string(observed_revision_));
        } else if (fresh) add_member(result.text, ",\"canvas_blank\":true");
        return result;
    }

    if (tool == "pixelforge_task" && args.get("action") == "finish") {
        AgentTaskSnapshot snapshot;
        { std::lock_guard lock(*bindings_.state_mutex); snapshot = bindings_.task->snapshot(); }
        std::string forwarded = "{\"action\":\"finish\",\"task_id\":" + std::to_string(snapshot.id) +
            ",\"expected_revision\":" + std::to_string(snapshot.document_revision) +
            ",\"summary\":" + q(args.get("summary")) + "}";
        return base_call(tool, forwarded);
    }

    if (tool == "pixelforge_pack" && args.get("action") == "create") {
        const auto task_id_value = args.get_i64("task_id");
        ImageData source; std::wstring path;
        const bool seed_source = agent_interaction_source_snapshot(source, path) && !flag_false(args, "seed_from_source");
        auto result = base_call(tool, arguments_json);
        if (!result.success || !seed_source) {
            if (result.success && task_id_value) refresh_pack(static_cast<std::uint64_t>(*task_id_value));
            return result;
        }
        std::string error;
        {
            std::lock_guard lock(*bindings_.state_mutex);
            if (bindings_.document->width() != source.width || bindings_.document->height() != source.height)
                return {false, "{\"ok\":false,\"error\":\"source_size_mismatch\",\"message\":\"The first pack canvas must match Source dimensions when Source seeding is enabled. Ask the user or set seed_from_source=false if a different layout is intentional.\"}"};
            if (!seed_document(*bindings_.document, source, error))
                return {false, "{\"ok\":false,\"error\":\"source_seed_failed\",\"message\":" + q(error) + "}"};
        }
        add_member(result.text, ",\"primary_seeded_from\":\"source\"");
        if (task_id_value) refresh_pack(static_cast<std::uint64_t>(*task_id_value));
        return result;
    }

    if (tool == "pixelforge_pack") {
        const auto task_id_value = args.get_i64("task_id");
        if (!task_id_value || *task_id_value < 0)
            return {false, "{\"ok\":false,\"error\":\"task_id_required\"}"};
        const auto task_id = static_cast<std::uint64_t>(*task_id_value);
        const std::string action = args.get("action");

        if ((action == "clone" || action == "copy") && !args.get("dests").empty()) {
            std::vector<std::string> dests;
            if (!args.get("dest").empty()) dests.push_back(args.get("dest"));
            for (const auto& d : split_keep_empty_ws(args.get("dests"), '|'))
                if (!d.empty() && std::find(dests.begin(), dests.end(), d) == dests.end()) dests.push_back(d);
            if (dests.empty()) return {false, "{\"ok\":false,\"error\":\"dest_required\"}"};
            std::size_t changed_total = 0;
            for (const auto& dest : dests) {
                std::string request = "{\"action\":" + q(action) + ",\"task_id\":" + std::to_string(task_id) +
                    ",\"source\":" + q(args.get("source")) + ",\"dest\":" + q(dest);
                if (action == "copy") {
                    request += add_optional_i64(args, "sx") + add_optional_i64(args, "sy") +
                               add_optional_i64(args, "width") + add_optional_i64(args, "height") +
                               add_optional_i64(args, "dx") + add_optional_i64(args, "dy");
                }
                request += "}";
                auto one = base_call(tool, request);
                if (!one.success)
                    return {false, "{\"ok\":false,\"error\":\"multi_dest_failed\",\"dest\":" + q(dest) +
                                   ",\"message\":" + q(one.text) + "}"};
                FlatJsonObject one_fields; std::string ignored;
                if (parse_flat_json_object(one.text, one_fields, ignored))
                    changed_total += static_cast<std::size_t>(std::max<std::int64_t>(0, one_fields.get_i64("changed_pixels").value_or(0)));
            }
            refresh_pack(task_id);
            return {true, "{\"ok\":true,\"action\":" + q(action) + ",\"dest_count\":" + std::to_string(dests.size()) +
                          ",\"changed_pixels\":" + std::to_string(changed_total) + ",\"dests\":" + q(join_pipe(dests)) + "}"};
        }

        std::string forwarded(arguments_json);
        bool timeline = action == "view" && args.get("mode") == "timeline";
        const bool animation_view = action == "view" && args.get("mode") == "animation";
        const bool animation_export = action == "export" && args.get("scope") == "animation";
        const auto requested_scale = args.get_i64("scale").value_or(1);
        bool gif_scale_clamped = false;

        if (timeline) set_json_string_member(forwarded, "mode", "strip");
        if ((animation_view || animation_export) && requested_scale > 8) {
            set_json_integer_member(forwarded, "scale", 8);
            gif_scale_clamped = true;
        }
        if (action == "export") {
            const auto requested_path = args.get("path");
            if (!requested_path.empty()) {
                const auto resolved = resolve_export_path(task_id, requested_path);
                set_json_string_member(forwarded, "path", wide_to_utf8_ws(resolved.wstring()));
            }
        }

        auto result = base_call(tool, forwarded);
        if (!result.success) return result;

        if (timeline) {
            add_member(result.text, ",\"requested_mode\":\"timeline\",\"temporal_review\":true,\"message\":\"Ordered frames are shown left-to-right for model-visible motion review.\"");
        } else if (animation_view) {
            std::string strip_request(arguments_json);
            set_json_string_member(strip_request, "mode", "strip");
            set_json_integer_member(strip_request, "scale", std::clamp<std::int64_t>(requested_scale, 1, 16));
            auto strip = base_call(tool, strip_request);
            if (strip.success && !strip.image_base64.empty()) {
                result.image_base64 = std::move(strip.image_base64);
                result.image_mime = std::move(strip.image_mime);
                add_member(result.text, ",\"agent_observation\":\"ordered_frame_strip\",\"animation_preview_generated\":true,\"message\":\"The real GIF was composed, but the model receives the ordered frame strip because model image observation may display animated GIFs as a static frame.\"");
            }
        }
        if (gif_scale_clamped)
            add_member(result.text, ",\"requested_scale\":" + std::to_string(requested_scale) +
                                    ",\"actual_gif_scale\":8,\"warning\":\"GIF preview scale is capped at 8x; use strip/timeline up to 16x for model inspection.\"");

        if (action == "program" || action == "edit" || action == "clone" || action == "copy" || action == "history")
            refresh_pack(task_id);
        return result;
    }

    if (tool == "pixelforge_io" && args.get("action") == "export") {
        const auto task_id_value = args.get_i64("task_id");
        if (!task_id_value || *task_id_value < 0) return base_call(tool, arguments_json);
        std::string forwarded(arguments_json);
        const auto resolved = resolve_export_path(static_cast<std::uint64_t>(*task_id_value), args.get("path"));
        set_json_string_member(forwarded, "path", wide_to_utf8_ws(resolved.wstring()));
        return base_call(tool, forwarded);
    }

    return base_call(tool, arguments_json);
}

std::string pixelforge_dynamic_tools_json() {
    std::string tools = base_pixelforge_dynamic_tools_json();
    auto end = tools.find_last_of(']');
    if (end == std::string::npos) return tools;
    tools.insert(end, R"JSON(,
{"type":"function","name":"pixelforge_reference","description":"Inspect or mechanically reuse a supplied reference without shell/Python. source is the editable image; content is a visual target; style is style guidance. view returns the actual reference image, palette computes colors locally, and seed copies exact pixels into an accepted same-size canvas.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["view","palette","seed"]},"task_id":{"type":"integer"},"reference":{"type":"string","enum":["source","content","style"]},"max_colors":{"type":"integer","minimum":1,"maximum":256}},"required":["action","task_id","reference"]}},
{"type":"function","name":"pixelforge_dialog","description":"Ask the user a concrete question in PixelForge and receive the answer in this same turn. YOU decide whether a pause is warranted. Use it only for a material ambiguity, impossible/unsupported instruction, or consequential choice; explain exactly what is blocked and provide up to three alternatives separated by |. Do not ask about routine artistic choices you can decide yourself.","inputSchema":{"type":"object","properties":{"task_id":{"type":"integer"},"reason":{"type":"string"},"question":{"type":"string"},"suggestions":{"type":"string"}},"required":["task_id","reason","question"]}}
)JSON");

    replace_once(tools,
        "\"mode\":{\"type\":\"string\",\"enum\":[\"canvas\",\"sheet\",\"strip\",\"animation\"]}",
        "\"mode\":{\"type\":\"string\",\"enum\":[\"canvas\",\"sheet\",\"strip\",\"timeline\",\"animation\"]}");
    replace_once(tools,
        "\"dest\":{\"type\":\"string\"}",
        "\"dest\":{\"type\":\"string\"},\"dests\":{\"type\":\"string\",\"description\":\"Optional | separated destination canvases for clone/copy so one call can distribute a base frame or region to many canvases.\"}");
    return tools;
}

} // namespace pixelforge::win32
