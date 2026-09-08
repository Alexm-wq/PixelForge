#include "AgentTask.hpp"
#include "ImageIO.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

constexpr UINT WM_AGENT_UPDATED = WM_APP + 1;
constexpr UINT_PTR CODEX_REFRESH_TIMER = 77;
constexpr UINT_PTR DISPLAY_ANIMATION_TIMER = 78;
constexpr int ID_INTELLIGENCE = 1021;
constexpr int ID_REVIEW_ACCEPT = 1031;
constexpr int ID_REVIEW_CHANGES = 1032;
constexpr int ID_REVIEW_REVISION_LEVEL = 1091;

HWND g_automatic_window = nullptr;
WNDPROC g_original_proc = nullptr;
HHOOK g_attach_hook = nullptr;

HWND g_review_window = nullptr;
HWND g_revision_level = nullptr;
WNDPROC g_review_original_proc = nullptr;
std::wstring g_last_persisted_project;

bool window_has_class(HWND hwnd, const wchar_t* wanted) {
    wchar_t class_name[96]{};
    if (!GetClassNameW(hwnd, class_name, 96)) return false;
    return std::wcscmp(class_name, wanted) == 0;
}

bool is_automatic_window(HWND hwnd) {
    return window_has_class(hwnd, L"PixelForgeAutomaticWindow");
}

bool is_review_window(HWND hwnd) {
    return window_has_class(hwnd, L"PixelForgeUserReviewWindow");
}

std::wstring environment_value(std::wstring_view name) {
    const std::wstring key(name);
    const DWORD needed = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!needed) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(key.c_str(), out.data(), needed);
    if (!written || written >= out.size()) return {};
    out.resize(written);
    return out;
}

std::filesystem::path find_repo_root_from(std::filesystem::path current) {
    for (int depth = 0; depth < 12 && !current.empty(); ++depth) {
        std::error_code ec;
        const bool has_cmake = std::filesystem::exists(current / L"CMakeLists.txt", ec);
        ec.clear();
        const bool has_source = std::filesystem::exists(current / L"src" / L"win32" / L"main_auto.cpp", ec);
        if (has_cmake && has_source) return current;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

std::filesystem::path resolve_repo_root() {
    if (const auto configured = environment_value(L"PIXELFORGE_REPO_ROOT"); !configured.empty()) {
        if (auto root = find_repo_root_from(configured); !root.empty()) return root;
    }

    std::wstring executable(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (count && count < executable.size()) {
        executable.resize(count);
        if (auto root = find_repo_root_from(std::filesystem::path(executable).parent_path()); !root.empty())
            return root;
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) return find_repo_root_from(cwd);
    return {};
}

std::string trim_ascii(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    if (first == text.end()) return {};
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    return std::string(first, last);
}

std::string json_quote(std::string_view text) {
    std::string out = "\"";
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
                    out.push_back(hex[(c >> 4) & 0x0f]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string project_name_from(const pixelforge::AgentTaskSnapshot& snapshot) {
    constexpr std::string_view marker = "PROJECT_NAME:";
    std::size_t start = 0;
    while (start <= snapshot.review_summary.size()) {
        const auto end = snapshot.review_summary.find('\n', start);
        std::string line = trim_ascii(snapshot.review_summary.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (line.rfind(marker, 0) == 0) {
            std::string name = trim_ascii(line.substr(marker.size()));
            if (!name.empty()) return name;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    // A name is optional for compatibility. If an older agent did not provide
    // PROJECT_NAME, derive a readable fallback from the first prompt line.
    std::string fallback = snapshot.prompt.substr(0, snapshot.prompt.find('\n'));
    fallback = trim_ascii(std::move(fallback));
    if (fallback.size() > 80) fallback.resize(80);
    if (!fallback.empty()) return fallback;
    return "PixelForge task " + std::to_string(snapshot.id);
}

bool replace_or_insert_project_name(std::string& manifest, std::string_view project_name) {
    const std::string key = "\"project_name\"";
    const auto key_pos = manifest.find(key);
    const std::string encoded = json_quote(project_name);
    if (key_pos == std::string::npos) {
        const auto object = manifest.find('{');
        if (object == std::string::npos) return false;
        manifest.insert(object + 1, "\n  \"project_name\": " + encoded + ",");
        return true;
    }

    auto colon = manifest.find(':', key_pos + key.size());
    if (colon == std::string::npos) return false;
    auto value = manifest.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos || manifest[value] != '"') return false;
    std::size_t end = value + 1;
    bool escaped = false;
    for (; end < manifest.size(); ++end) {
        const char c = manifest[end];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') { ++end; break; }
    }
    if (end > manifest.size()) return false;
    manifest.replace(value, end - value, encoded);
    return true;
}

bool write_text_atomic(const std::filesystem::path& path, const std::string& text, std::wstring& error) {
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = L"Could not create temporary project manifest: " + temporary.wstring();
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = L"Could not write project manifest: " + temporary.wstring();
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = L"Could not finalize project.json (Windows error " + std::to_wstring(code) + L").";
        return false;
    }
    return true;
}

std::size_t manifest_canvas_count(std::string_view manifest) {
    constexpr std::string_view needle = "\"file\":";
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = manifest.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::size_t persisted_canvas_file_count(const std::filesystem::path& canvas_root) {
    std::error_code ec;
    if (!std::filesystem::exists(canvas_root, ec) || ec) return 0;
    std::size_t count = 0;
    for (std::filesystem::recursive_directory_iterator it(canvas_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec) ++count;
    }
    return ec ? 0 : count;
}

bool persist_reviewed_project(std::wstring& saved_directory, std::wstring& error) {
    saved_directory.clear();
    error.clear();

    auto* task = pixelforge::AgentTaskController::active_instance_for_ui();
    if (!task) {
        error = L"PixelForge could not locate the active task for project persistence.";
        return false;
    }
    const auto snapshot = task->snapshot();
    if (snapshot.state != pixelforge::TaskState::Finished || !snapshot.awaiting_user_review) {
        error = L"The current artwork is not awaiting review, so there is nothing to persist for acceptance.";
        return false;
    }
    auto* document = task->document_for_ui();
    if (!document || document->width() <= 0 || document->height() <= 0) {
        error = L"The current task has no valid canvas to persist.";
        return false;
    }

    const auto repo_root = resolve_repo_root();
    if (repo_root.empty()) {
        error = L"PixelForge could not resolve the repository root for project persistence. Set PIXELFORGE_REPO_ROOT if needed.";
        return false;
    }
    const auto project = repo_root / L"projects" / (L"task_" + std::to_wstring(snapshot.id));
    const auto manifest_path = project / L"project.json";
    const auto canvases = project / L"canvases";
    const std::string project_name = project_name_from(snapshot);

    std::error_code ec;
    if (std::filesystem::exists(manifest_path, ec) && !ec) {
        std::ifstream in(manifest_path, std::ios::binary);
        if (!in) {
            error = L"Could not reopen the existing project manifest for final persistence.";
            return false;
        }
        std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) {
            error = L"Could not read the existing project manifest.";
            return false;
        }
        const std::size_t expected = manifest_canvas_count(manifest);
        const std::size_t present = persisted_canvas_file_count(canvases);
        if (expected == 0 || present < expected) {
            error = L"The existing pack autosave is incomplete (project.json references " +
                    std::to_wstring(expected) + L" canvases but only " + std::to_wstring(present) +
                    L" persisted canvas files are present). Acceptance was blocked to avoid losing the pack.";
            return false;
        }
        if (!replace_or_insert_project_name(manifest, project_name)) {
            error = L"The existing project.json is malformed and could not be finalized.";
            return false;
        }
        if (!write_text_atomic(manifest_path, manifest, error)) return false;
        saved_directory = project.wstring();
        return true;
    }
    if (ec) {
        error = L"Could not inspect the project directory before acceptance.";
        return false;
    }

    // If pack export started but failed before project.json was written, do not
    // silently collapse that partial pack into a one-canvas project.
    const std::size_t orphaned_files = persisted_canvas_file_count(canvases);
    if (orphaned_files > 0) {
        error = L"PixelForge found canvas files for this task but no project.json. This looks like an incomplete pack autosave; acceptance was blocked instead of discarding the missing pack metadata.";
        return false;
    }

    std::filesystem::create_directories(canvases / L"ungrouped", ec);
    if (!ec) std::filesystem::create_directories(project / L"previews", ec);
    if (!ec) std::filesystem::create_directories(project / L"exports", ec);
    if (ec) {
        error = L"Could not create the PixelForge project directory: " + project.wstring();
        return false;
    }

    const auto canvas_file = canvases / L"ungrouped" / L"canvas.png";
    std::wstring png_error;
    if (!pixelforge::win32::save_png_wic(canvas_file.wstring(), document->width(), document->height(),
                                         document->pixels(), png_error)) {
        error = L"Could not persist the accepted canvas: " + png_error;
        return false;
    }

    std::string manifest;
    manifest += "{\n";
    manifest += "  \"version\": 1,\n";
    manifest += "  \"project_name\": " + json_quote(project_name) + ",\n";
    manifest += "  \"task_id\": " + std::to_string(snapshot.id) + ",\n";
    manifest += "  \"pack_revision\": " + std::to_string(document->revision()) + ",\n";
    manifest += "  \"canvases\": [\n";
    manifest += "    {\"name\":\"canvas\",\"group\":\"\",\"frame\":-1,\"width\":" +
                std::to_string(document->width()) + ",\"height\":" + std::to_string(document->height()) +
                ",\"file\":\"canvases/ungrouped/canvas.png\"}\n";
    manifest += "  ]\n}\n";
    if (!write_text_atomic(manifest_path, manifest, error)) return false;

    saved_directory = project.wstring();
    return true;
}

RECT live_canvas_region(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    // Match the legacy editor's center column, but start below the permanent
    // workspace tab bar so parent repaints never touch the tab chrome.
    RECT region{340, 52, client.right - 330, client.bottom - 8};
    if (region.right < region.left) region.right = region.left;
    if (region.bottom < region.top) region.bottom = region.top;
    return region;
}

RECT task_state_region() {
    return RECT{0, 296, 340, 455};
}

void rescope_full_invalidation(HWND hwnd, bool include_task_state) {
    RECT dirty{};
    if (!GetUpdateRect(hwnd, &dirty, FALSE)) return;

    // The legacy window invalidates its whole client area for every playback
    // tick / agent status update. Clear that broad update and replace it with
    // only the regions whose pixels can actually have changed.
    ValidateRect(hwnd, nullptr);
    const RECT canvas = live_canvas_region(hwnd);
    InvalidateRect(hwnd, &canvas, FALSE);
    if (include_task_state) {
        const RECT task = task_state_region();
        InvalidateRect(hwnd, &task, FALSE);
    }
}

LRESULT CALLBACK repaint_guard_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_original_proc) return DefWindowProcW(hwnd, msg, wparam, lparam);

    if (msg == WM_TIMER && wparam == CODEX_REFRESH_TIMER) {
        RECT before{};
        const BOOL already_dirty = GetUpdateRect(hwnd, &before, FALSE);
        const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
        // CODEX_REFRESH_TIMER exists to service review/status state. It should
        // never force a visual refresh by itself. Preserve any update that was
        // already pending before this timer fired.
        if (!already_dirty) ValidateRect(hwnd, nullptr);
        return result;
    }

    if (msg == WM_TIMER && wparam == DISPLAY_ANIMATION_TIMER) {
        RECT before{};
        const BOOL already_dirty = GetUpdateRect(hwnd, &before, FALSE);
        const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
        if (!already_dirty) rescope_full_invalidation(hwnd, false);
        return result;
    }

    if (msg == WM_AGENT_UPDATED) {
        RECT before{};
        const BOOL already_dirty = GetUpdateRect(hwnd, &before, FALSE);
        const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
        if (!already_dirty) rescope_full_invalidation(hwnd, true);
        return result;
    }

    const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
    if (msg == WM_NCDESTROY && hwnd == g_automatic_window) {
        g_automatic_window = nullptr;
        g_original_proc = nullptr;
    }
    return result;
}

void attach_repaint_guard(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || hwnd == g_automatic_window) return;

    // Parent painting must never bleed through the Canvases/Animation child
    // pages. This also prevents needless redraw work behind those pages.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0)
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN);

    g_automatic_window = hwnd;
    g_original_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(repaint_guard_proc)));
}

LRESULT CALLBACK review_intelligence_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const auto original = g_review_original_proc;
    if (!original) return DefWindowProcW(hwnd, msg, wparam, lparam);

    // Acceptance is a persistence boundary. Serialize/validate the complete
    // project first; only forward the click to main_auto.cpp when that succeeds.
    if (msg == WM_COMMAND && LOWORD(wparam) == ID_REVIEW_ACCEPT) {
        std::wstring persistence_error;
        std::wstring saved;
        if (!persist_reviewed_project(saved, persistence_error)) {
            MessageBoxW(hwnd, persistence_error.c_str(), L"Could not save PixelForge project", MB_OK | MB_ICONERROR);
            return 0;
        }
        g_last_persisted_project = std::move(saved);
    }

    // The normal revision path reads the owner's LEVEL combo immediately after
    // the review window accepts the change request. Mirror the popup's selection
    // into that existing control before main_auto.cpp handles Request changes.
    if (msg == WM_COMMAND && LOWORD(wparam) == ID_REVIEW_CHANGES) {
        const LRESULT selection = g_revision_level
            ? SendMessageW(g_revision_level, CB_GETCURSEL, 0, 0)
            : CB_ERR;
        const HWND owner = GetWindow(hwnd, GW_OWNER);
        const HWND owner_level = owner ? GetDlgItem(owner, ID_INTELLIGENCE) : nullptr;
        if (selection != CB_ERR && owner_level)
            SendMessageW(owner_level, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }

    const LRESULT result = CallWindowProcW(original, hwnd, msg, wparam, lparam);
    if (msg == WM_NCDESTROY && hwnd == g_review_window) {
        g_review_window = nullptr;
        g_revision_level = nullptr;
        g_review_original_proc = nullptr;
    }
    return result;
}

void attach_review_intelligence(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || hwnd == g_review_window) return;

    const HWND owner = GetWindow(hwnd, GW_OWNER);
    const HWND owner_level = owner ? GetDlgItem(owner, ID_INTELLIGENCE) : nullptr;
    LRESULT initial_selection = owner_level ? SendMessageW(owner_level, CB_GETCURSEL, 0, 0) : 1;
    if (initial_selection == CB_ERR) initial_selection = 1;

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    // Make room below the feedback box for a per-revision intelligence choice.
    RECT window_rect{};
    if (GetWindowRect(hwnd, &window_rect)) {
        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        SetWindowPos(hwnd, nullptr, 0, 0, width, height + 54,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (HWND accept = GetDlgItem(hwnd, ID_REVIEW_ACCEPT))
        MoveWindow(accept, 18, 326, 140, 32, TRUE);
    if (HWND changes = GetDlgItem(hwnd, ID_REVIEW_CHANGES))
        MoveWindow(changes, 170, 326, 302, 32, TRUE);

    HWND label = CreateWindowW(L"STATIC", L"REVISION LEVEL", WS_CHILD | WS_VISIBLE,
                               18, 274, 104, 20, hwnd, nullptr, nullptr, nullptr);
    if (label) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    g_revision_level = CreateWindowW(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        126, 270, 150, 160, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REVIEW_REVISION_LEVEL)), nullptr, nullptr);
    if (g_revision_level) {
        SendMessageW(g_revision_level, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        for (const wchar_t* level : {L"Low", L"Medium", L"High", L"Extra High", L"Max"})
            SendMessageW(g_revision_level, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(level));
        SendMessageW(g_revision_level, CB_SETDROPPEDWIDTH, 170, 0);
        SendMessageW(g_revision_level, CB_SETCURSEL, static_cast<WPARAM>(initial_selection), 0);
    }

    HWND hint = CreateWindowW(L"STATIC", L"Use Low/Medium for small targeted fixes.",
                              WS_CHILD | WS_VISIBLE, 288, 274, 184, 36,
                              hwnd, nullptr, nullptr, nullptr);
    if (hint) SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    g_review_window = hwnd;
    g_review_original_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(review_intelligence_proc)));
}

LRESULT CALLBACK attach_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message->hwnd && is_automatic_window(message->hwnd) &&
            (message->message == WM_CREATE || message->message == WM_SHOWWINDOW)) {
            attach_repaint_guard(message->hwnd);
        }
        if (message->hwnd && is_review_window(message->hwnd) &&
            (message->message == WM_CREATE || message->message == WM_SHOWWINDOW)) {
            attach_review_intelligence(message->hwnd);
        }
    }
    return CallNextHookEx(g_attach_hook, code, wparam, lparam);
}

struct AutoInstallRepaintGuard {
    AutoInstallRepaintGuard() {
        g_attach_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, attach_hook_proc, nullptr, GetCurrentThreadId());
    }
    ~AutoInstallRepaintGuard() {
        if (g_attach_hook) UnhookWindowsHookEx(g_attach_hook);
        g_attach_hook = nullptr;
    }
};

AutoInstallRepaintGuard g_auto_install_repaint_guard;

} // namespace