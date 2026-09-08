#include "ProjectLoader.hpp"

#include "ImageIO.hpp"
#include "MiniJson.hpp"

#include <commdlg.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

struct SavedCanvas {
    std::string name;
    std::string group;
    int frame = -1;
    int width = 0;
    int height = 0;
    std::string file;
    ImageData image;
};

std::wstring utf8_to_wide_project(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string wide_to_utf8_project(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::string q(std::string_view text) {
    return json_quote(text);
}

bool safe_component(std::string_view value) {
    if (value.empty() || value.size() > 96) return false;
    for (const unsigned char c : value)
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    return true;
}

bool read_text(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return static_cast<bool>(file) || file.eof();
}

bool find_unsigned(std::string_view json, std::string_view key, std::uint64_t& value) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto p = json.find(needle);
    if (p == std::string_view::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string_view::npos) return false;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    const auto begin = p;
    while (p < json.size() && std::isdigit(static_cast<unsigned char>(json[p]))) ++p;
    if (p == begin) return false;
    const auto parsed = std::from_chars(json.data() + begin, json.data() + p, value);
    return parsed.ec == std::errc{};
}

bool find_canvas_objects(std::string_view json, std::vector<std::string>& objects, std::string& error) {
    objects.clear();
    const auto key = json.find("\"canvases\"");
    if (key == std::string_view::npos) { error = "Project manifest has no canvases array."; return false; }
    auto p = json.find('[', key);
    if (p == std::string_view::npos) { error = "Project canvases array is malformed."; return false; }
    ++p;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t object_start = std::string_view::npos;
    for (; p < json.size(); ++p) {
        const char c = json[p];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') {
            if (depth == 0) object_start = p;
            ++depth;
            continue;
        }
        if (c == '}') {
            if (depth <= 0) { error = "Project canvases array contains an unmatched brace."; return false; }
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                objects.emplace_back(json.substr(object_start, p - object_start + 1));
                object_start = std::string_view::npos;
            }
            continue;
        }
        if (c == ']' && depth == 0) return true;
    }
    error = "Project canvases array is unterminated.";
    return false;
}

bool path_is_safe_relative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path)
        if (part == L"..") return false;
    return true;
}

std::uint32_t argb_at(const ImageData& image, int x, int y) {
    const auto i = (static_cast<std::size_t>(y) * image.width + x) * 4u;
    return (static_cast<std::uint32_t>(image.bgra[i + 3]) << 24) |
           (static_cast<std::uint32_t>(image.bgra[i + 2]) << 16) |
           (static_cast<std::uint32_t>(image.bgra[i + 1]) << 8) |
           static_cast<std::uint32_t>(image.bgra[i + 0]);
}

std::string color_token(std::uint32_t argb) {
    std::ostringstream out;
    out << '#' << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << argb;
    return out.str();
}

std::vector<std::string> encode_patch_chunks(const ImageData& image) {
    constexpr std::size_t kTargetChunkBytes = 384u * 1024u;
    std::vector<std::string> chunks;
    std::string current;
    current.reserve(std::min<std::size_t>(kTargetChunkBytes, static_cast<std::size_t>(image.width) * image.height * 8u));
    auto append = [&](std::string op) {
        if (!current.empty() && current.size() + op.size() + 1 > kTargetChunkBytes) {
            chunks.push_back(std::move(current));
            current.clear();
            current.reserve(kTargetChunkBytes);
        }
        if (!current.empty()) current.push_back(';');
        current += op;
    };

    for (int y = 0; y < image.height; ++y) {
        int x = 0;
        while (x < image.width) {
            const auto color = argb_at(image, x, y);
            int end = x + 1;
            while (end < image.width && argb_at(image, end, y) == color) ++end;
            const int length = end - x;
            // New pack canvases are transparent, so transparent source runs need no write.
            if ((color >> 24) != 0) {
                const auto c = color_token(color);
                if (length == 1)
                    append("P," + std::to_string(x) + "," + std::to_string(y) + "," + c);
                else
                    append("H," + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(length) + "," + c);
            }
            x = end;
        }
    }
    if (!current.empty()) chunks.push_back(std::move(current));
    return chunks;
}

bool parse_manifest(const std::filesystem::path& manifest,
                    std::uint64_t& task_id,
                    std::vector<SavedCanvas>& canvases,
                    std::wstring& error) {
    std::string text;
    if (!read_text(manifest, text)) {
        error = L"Could not read the selected project manifest.";
        return false;
    }
    if (!find_unsigned(text, "task_id", task_id) || task_id == 0) {
        error = L"This project manifest has no valid task_id. Re-save it with the current PixelForge project format first.";
        return false;
    }

    std::vector<std::string> objects;
    std::string parse_error;
    if (!find_canvas_objects(text, objects, parse_error)) {
        error = utf8_to_wide_project(parse_error);
        return false;
    }
    if (objects.empty()) {
        error = L"The project contains no canvases.";
        return false;
    }

    const auto root = manifest.parent_path();
    canvases.clear();
    canvases.reserve(objects.size());
    for (const auto& object : objects) {
        FlatJsonObject fields;
        if (!parse_flat_json_object(object, fields, parse_error)) {
            error = L"A canvas entry in project.json is malformed.";
            return false;
        }
        SavedCanvas canvas;
        canvas.name = fields.get("name");
        canvas.group = fields.get("group");
        canvas.frame = static_cast<int>(fields.get_i64("frame").value_or(-1));
        canvas.width = static_cast<int>(fields.get_i64("width").value_or(0));
        canvas.height = static_cast<int>(fields.get_i64("height").value_or(0));
        canvas.file = fields.get("file");
        if (!safe_component(canvas.name) || (!canvas.group.empty() && !safe_component(canvas.group)) ||
            canvas.width <= 0 || canvas.height <= 0) {
            error = L"The project contains an invalid canvas name, group, or dimension.";
            return false;
        }
        std::filesystem::path relative = utf8_to_wide_project(canvas.file);
        if (relative.empty()) {
            const auto folder = canvas.group.empty() ? L"ungrouped" : utf8_to_wide_project(canvas.group);
            relative = std::filesystem::path(L"canvases") / folder / (utf8_to_wide_project(canvas.name) + L".png");
        }
        if (!path_is_safe_relative(relative)) {
            error = L"The project contains a canvas path outside its project directory.";
            return false;
        }
        std::wstring image_error;
        if (!load_image_wic((root / relative).wstring(), canvas.image, image_error)) {
            error = L"Could not load canvas '" + utf8_to_wide_project(canvas.name) + L"': " + image_error;
            return false;
        }
        if (canvas.image.width != canvas.width || canvas.image.height != canvas.height) {
            error = L"Canvas '" + utf8_to_wide_project(canvas.name) + L"' does not match the dimensions recorded in project.json.";
            return false;
        }
        canvases.push_back(std::move(canvas));
    }
    return true;
}

std::string canvas_specs(const std::vector<SavedCanvas>& canvases) {
    std::string specs;
    for (std::size_t i = 0; i < canvases.size(); ++i) {
        if (i) specs.push_back('|');
        const auto& c = canvases[i];
        specs += c.name + "," + c.group + "," + std::to_string(c.width) + "," +
                 std::to_string(c.height) + "," + std::to_string(c.frame);
    }
    return specs;
}

std::string loaded_project_prompt(const std::vector<SavedCanvas>& canvases) {
    std::map<std::string, std::size_t> animations;
    for (const auto& canvas : canvases) {
        if (canvas.frame >= 0 && !canvas.group.empty()) ++animations[canvas.group];
    }

    std::ostringstream out;
    out << "Existing PixelForge project opened for inspection and repair.\n\n"
        << "The project already contains " << canvases.size() << " canvases.";
    if (!animations.empty()) {
        out << " Existing animation groups:";
        for (const auto& [group, count] : animations)
            out << " " << group << "=" << count << " frames;";
    }
    out << "\nPreserve existing work by default. Inspect the existing pack and existing animation before deciding what to change. "
           "Repair or refine existing canvases whenever practical. Do not call pixelforge_pack create merely to start over. "
           "Replacing an existing pack is destructive and requires replace_existing=true; use that only when a full replacement is intentionally necessary.";
    return out.str();
}

} // namespace

bool choose_project_manifest(HWND owner, std::wstring& manifest_path, std::wstring& error) {
    manifest_path.clear();
    error.clear();
    wchar_t file[MAX_PATH * 8]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"PixelForge project (project.json)\0project.json;*.pfg.json\0JSON files\0*.json\0All files\0*.*\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrTitle = L"Open PixelForge Project";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        const DWORD code = CommDlgExtendedError();
        if (code != 0) error = L"The Open Project dialog failed (common-dialog error " + std::to_wstring(code) + L").";
        return false;
    }
    manifest_path = file;
    return true;
}

bool load_project_into_workspace(const std::wstring& manifest_path,
                                 const std::wstring& repo_root,
                                 LocalAgentToolSession& tools,
                                 AgentTaskController& task,
                                 PixelDocument& primary,
                                 std::mutex& state_mutex,
                                 ProjectLoadSummary& summary,
                                 std::wstring& error) {
    summary = {};
    error.clear();
    const std::filesystem::path manifest(manifest_path);
    std::uint64_t restored_task_id = 0;
    std::vector<SavedCanvas> canvases;
    if (!parse_manifest(manifest, restored_task_id, canvases, error)) return false;

    {
        std::lock_guard lock(state_mutex);
        if (task.state() == TaskState::Accepted) task.abort("Existing task replaced by a project opened by the user.", nullptr);
        task.set_next_task_id_for_restore(restored_task_id);
        const auto actual = task.begin(loaded_project_prompt(canvases));
        if (actual != restored_task_id) {
            error = L"Could not restore the project's workspace id.";
            return false;
        }
    }

    // Project restoration is the one legitimate destructive pack construction path.
    // The normal agent-facing create path is protected once project.json exists.
    const std::string create_args = "{\"action\":\"create\",\"task_id\":" + std::to_string(restored_task_id) +
        ",\"canvases\":" + q(canvas_specs(canvases)) +
        ",\"seed_from_source\":false,\"replace_existing\":true}";
    auto created = tools.call("pixelforge_pack", create_args);
    if (!created.success) {
        error = L"Could not reconstruct the project's canvas pack: " + utf8_to_wide_project(created.text);
        return false;
    }

    for (const auto& canvas : canvases) {
        const auto chunks = encode_patch_chunks(canvas.image);
        for (const auto& patch : chunks) {
            const std::string edit_args = "{\"action\":\"edit\",\"task_id\":" + std::to_string(restored_task_id) +
                ",\"canvas\":" + q(canvas.name) + ",\"patch\":" + q(patch) + "}";
            auto edited = tools.call("pixelforge_pack", edit_args);
            if (!edited.success) {
                error = L"Could not restore canvas '" + utf8_to_wide_project(canvas.name) + L"': " + utf8_to_wide_project(edited.text);
                return false;
            }
        }
    }

    // The pack create operation accepted the task, so subsequent Astra turns can
    // work immediately without re-creating or re-seeding the saved project.
    summary.task_id = restored_task_id;
    summary.canvas_count = canvases.size();
    summary.manifest_path = manifest.wstring();
    summary.source_directory = manifest.parent_path().wstring();
    summary.active_directory = (std::filesystem::path(repo_root) / L"projects" /
                                (L"task_" + std::to_wstring(restored_task_id))).wstring();

    // Refresh the primary editor immediately from the first restored pack canvas.
    // pack create/edit already operates on the same primary PixelDocument object.
    (void)primary;
    return true;
}

} // namespace pixelforge::win32
