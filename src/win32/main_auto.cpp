#include "CodexAppClient.hpp"
#include "LocalAgentToolSession.hpp"
#include "PipeBridge.hpp"
#include "RecordMcpServer.hpp"
#include "SessionRecorder.hpp"

#define wWinMain pixelforge_legacy_wWinMain
#define wndproc pixelforge_legacy_wndproc
#include "main_mcp.cpp"
#undef wndproc
#undef wWinMain

#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_CODEX_STATUS = WM_APP + 20;
constexpr UINT_PTR CODEX_REFRESH_TIMER = 77;
constexpr int ID_STOP_CODEX = 1020;

pixelforge::win32::CodexAppClient g_codex_client;
pixelforge::win32::SessionRecorder g_session_recorder;
pixelforge::win32::LocalAgentToolSession g_local_tool_session;
std::wstring g_repo_root;
std::wstring g_executable_path;
HWND g_codex_status = nullptr;

std::wstring executable_path() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::wstring(buffer.data(), count);
}

std::wstring find_repo_root_from(std::filesystem::path current) {
    for (int depth = 0; depth < 12 && !current.empty(); ++depth) {
        std::error_code ec;
        const bool has_cmake = std::filesystem::exists(current / L"CMakeLists.txt", ec);
        ec.clear();
        const bool has_config = std::filesystem::exists(current / L"config" / L"agent_system_prompt.md", ec);
        if (has_cmake && has_config) return current.wstring();
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

std::wstring find_repo_root(const std::wstring& exe) {
    if (!exe.empty()) {
        if (auto root = find_repo_root_from(std::filesystem::path(exe).parent_path()); !root.empty())
            return root;
    }
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        if (auto root = find_repo_root_from(cwd); !root.empty()) return root;
    }
    return {};
}

std::string read_utf8_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void post_codex_status(HWND hwnd, std::wstring text) {
    auto* owned = new std::wstring(std::move(text));
    if (!PostMessageW(hwnd, WM_CODEX_STATUS, 0, reinterpret_cast<LPARAM>(owned))) delete owned;
}

void finalize_recording_if_needed(HWND hwnd) {
    if (!g_session_recorder.active()) return;
    std::wstring recording_error;
    if (!g_session_recorder.stop(recording_error))
        post_codex_status(hwnd, L"Recording finalize error: " + recording_error);
}

bool start_local_tools(HWND hwnd, std::wstring& error) {
    pixelforge::win32::AgentMcpBindings bindings;
    bindings.document = &g_app.document;
    bindings.task = &g_app.task;
    bindings.content_reference = &g_app.content_reference;
    bindings.style_reference = &g_app.style_reference;
    bindings.content_path = &g_app.content_path;
    bindings.style_path = &g_app.style_path;
    bindings.state_mutex = &g_state_mutex;
    bindings.hwnd = hwnd;

    pixelforge::win32::LocalRecordBindings record;
    record.recorder = &g_session_recorder;
    record.task = &g_app.task;
    record.state_mutex = &g_state_mutex;
    record.hwnd = hwnd;
    record.recording_directory = (std::filesystem::path(g_repo_root) / L"recordings").wstring();

    return g_local_tool_session.start(bindings, std::move(record), error);
}

void start_automatic_generation(HWND hwnd) {
    if (g_codex_client.busy()) {
        show_error(hwnd, L"Codex is already working on the current PixelForge task.");
        return;
    }

    const std::wstring prompt_w = get_window_text(g_prompt);
    const auto first_non_ws = prompt_w.find_first_not_of(L" \t\r\n");
    if (first_non_ws == std::wstring::npos) {
        show_error(hwnd, L"Enter a PixelForge prompt before generating.");
        return;
    }
    if (g_repo_root.empty()) {
        show_error(hwnd, L"PixelForge could not resolve its repository root. Run the executable from inside the PixelForge checkout or build directory.");
        return;
    }

    g_local_tool_session.stop();
    std::wstring local_error;
    if (!start_local_tools(hwnd, local_error)) {
        show_error(hwnd, local_error);
        return;
    }

    const std::string prompt = wide_to_utf8(prompt_w);
    {
        std::lock_guard lock(g_state_mutex);
        g_app.task.begin(prompt);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    post_codex_status(hwnd, L"Codex: queued...");

    pixelforge::win32::CodexGenerateRequest request;
    request.repo_root = g_repo_root;
    request.executable_path = g_executable_path.empty() ? g_repo_root : g_executable_path;
    request.prompt = prompt;
    request.agent_contract = read_utf8_file(std::filesystem::path(g_repo_root) / L"config" / L"agent_system_prompt.md");
    request.tool_session = &g_local_tool_session;

    std::wstring error;
    const bool started = g_codex_client.generate_async(
        std::move(request),
        [hwnd](std::wstring status) {
            post_codex_status(hwnd, std::move(status));
            PostMessageW(hwnd, WM_AGENT_UPDATED, 0, 0);
        },
        [hwnd](bool ok, std::wstring message) {
            g_local_tool_session.stop();
            finalize_recording_if_needed(hwnd);

            AgentTaskSnapshot snapshot;
            {
                std::lock_guard lock(g_state_mutex);
                snapshot = g_app.task.snapshot();
            }
            if (!ok) {
                post_codex_status(hwnd, L"Codex error: " + message);
            } else if (snapshot.state == TaskState::Finished) {
                post_codex_status(hwnd, L"Codex: complete.");
            } else if (snapshot.state == TaskState::Rejected) {
                post_codex_status(hwnd, L"Codex rejected task: " + utf8_to_wide(snapshot.status_message));
            } else if (snapshot.state == TaskState::Aborted) {
                post_codex_status(hwnd, L"Codex aborted task: " + utf8_to_wide(snapshot.status_message));
            } else {
                post_codex_status(hwnd, L"Codex turn ended without task.finish; review the canvas/task state.");
            }
            PostMessageW(hwnd, WM_AGENT_UPDATED, 0, 0);
        },
        error);

    if (!started) {
        g_local_tool_session.stop();
        finalize_recording_if_needed(hwnd);
        post_codex_status(hwnd, L"Codex: idle.");
        show_error(hwnd, error);
    }
}

LRESULT CALLBACK automatic_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CREATE) {
        const LRESULT result = pixelforge_legacy_wndproc(hwnd, msg, wparam, lparam);
        if (HWND generate = GetDlgItem(hwnd, ID_BEGIN)) {
            SetWindowTextW(generate, L"Generate with Codex");
            MoveWindow(generate, 12, 162, 232, 28, TRUE);
        }
        CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      252, 162, 72, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STOP_CODEX)), nullptr, nullptr);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_codex_status = CreateWindowW(L"STATIC", L"Codex: idle.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      12, 600, 312, 120, hwnd, nullptr, nullptr, nullptr);
        if (g_codex_status) SendMessageW(g_codex_status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetTimer(hwnd, CODEX_REFRESH_TIMER, 200, nullptr);
        return result;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == ID_BEGIN) {
        start_automatic_generation(hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == ID_STOP_CODEX) {
        g_codex_client.cancel();
        post_codex_status(hwnd, L"Codex: stopping; existing canvas will be kept.");
        return 0;
    }
    if (msg == WM_CODEX_STATUS) {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
        if (text && g_codex_status) SetWindowTextW(g_codex_status, text->c_str());
        return 0;
    }
    if (msg == WM_TIMER && wparam == CODEX_REFRESH_TIMER) {
        if (g_codex_client.busy() || g_session_recorder.active()) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_DESTROY) {
        KillTimer(hwnd, CODEX_REFRESH_TIMER);
        g_codex_client.shutdown();
        g_local_tool_session.stop();
        finalize_recording_if_needed(hwnd);
        return pixelforge_legacy_wndproc(hwnd, msg, wparam, lparam);
    }
    return pixelforge_legacy_wndproc(hwnd, msg, wparam, lparam);
}

std::vector<std::wstring> command_line_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> out;
    if (argv) {
        out.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return out;
}

std::wstring option_argument(const std::vector<std::wstring>& args, std::wstring_view option) {
    for (std::size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == option) return args[i + 1];
    return {};
}

bool has_arg(const std::vector<std::wstring>& args, std::wstring_view wanted) {
    const std::wstring value(wanted);
    return std::find(args.begin(), args.end(), value) != args.end();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    const auto args = command_line_args();

    if (const auto pipe = option_argument(args, L"--bridge"); !pipe.empty())
        return pixelforge::win32::run_mcp_bridge_stdio(pipe);
    if (const auto pipe = option_argument(args, L"--record-bridge"); !pipe.empty())
        return pixelforge::win32::run_record_bridge_stdio(pipe);
    if (has_arg(args, L"--mcp"))
        return pixelforge_legacy_wWinMain(instance, previous, command_line, show);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_executable_path = executable_path();
    g_repo_root = find_repo_root(g_executable_path);

    WNDCLASSW wc{};
    wc.lpfnWndProc = automatic_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = L"PixelForgeAutomaticWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"PixelForge - Automatic Codex Pixel Editor",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 900,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_codex_client.shutdown();
    g_local_tool_session.stop();
    finalize_recording_if_needed(hwnd);
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
