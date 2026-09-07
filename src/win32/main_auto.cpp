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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_CODEX_STATUS = WM_APP + 20;
constexpr UINT WM_SHOW_USER_REVIEW = WM_APP + 21;
constexpr UINT_PTR CODEX_REFRESH_TIMER = 77;
constexpr UINT_PTR RECORDING_FINALIZE_TIMER = 79;
constexpr int ID_STOP_CODEX = 1020;
constexpr int ID_INTELLIGENCE = 1021;
constexpr int ID_REVIEW_FEEDBACK = 1030;
constexpr int ID_REVIEW_ACCEPT = 1031;
constexpr int ID_REVIEW_CHANGES = 1032;

constexpr const char* kFallbackAgentContract =
    "You are a pixel artist using PixelForge's local drawing tools. "
    "Study supplied references visually, create the requested artwork directly at native pixel resolution, "
    "use your visual judgment, render and inspect your work, make targeted corrections, and export the native PNG. "
    "Use pixelforge_program for broad drawing and pixelforge_edit for precise corrections. "
    "Fetch each reference once and reuse it from context. Do not use external image generation or mouse automation.";

pixelforge::win32::CodexAppClient g_codex_client;
pixelforge::win32::SessionRecorder g_session_recorder;
pixelforge::win32::LocalAgentToolSession g_local_tool_session;
std::wstring g_repo_root;
std::wstring g_executable_path;
HWND g_codex_status = nullptr;
HWND g_intelligence = nullptr;
HWND g_review_window = nullptr;
HWND g_review_feedback = nullptr;
bool g_review_popup_pending = false;
std::wstring g_review_summary_text;

void start_automatic_generation(HWND hwnd, std::string review_feedback = {});
void show_user_review_window(HWND owner);

std::string selected_reasoning_effort() {
    if (!g_intelligence) return "medium";
    const LRESULT selection = SendMessageW(g_intelligence, CB_GETCURSEL, 0, 0);
    switch (selection) {
        case 0: return "low";
        case 1: return "medium";
        case 2: return "high";
        case 3: return "xhigh";
        case 4: return "max";
        default: return "medium";
    }
}

std::wstring executable_path() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::wstring(buffer.data(), count);
}

std::wstring environment_value(std::wstring_view name) {
    const DWORD needed = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
    if (!needed) return {};
    std::wstring out(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(std::wstring(name).c_str(), out.data(), needed);
    if (!written || written >= out.size()) return {};
    out.resize(written);
    return out;
}

std::wstring find_repo_root_from(std::filesystem::path current) {
    for (int depth = 0; depth < 12 && !current.empty(); ++depth) {
        std::error_code ec;
        const bool has_cmake = std::filesystem::exists(current / L"CMakeLists.txt", ec);
        ec.clear();
        const bool has_source = std::filesystem::exists(current / L"src" / L"win32" / L"main_auto.cpp", ec);
        if (has_cmake && has_source) return current.wstring();
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

std::wstring find_repo_root(const std::wstring& exe) {
    if (auto configured = environment_value(L"PIXELFORGE_REPO_ROOT"); !configured.empty()) {
        if (auto root = find_repo_root_from(configured); !root.empty()) return root;
    }
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

bool finalize_recording_if_needed(HWND hwnd) {
    if (!g_session_recorder.active()) return true;
    std::wstring recording_error;
    if (!g_session_recorder.stop(recording_error)) {
        post_codex_status(hwnd, L"Recording finalize error: " + recording_error);
        return false;
    }
    return true;
}

void finalize_terminal_recording(HWND hwnd, std::wstring status) {
    if (!g_session_recorder.active()) {
        post_codex_status(hwnd, std::move(status));
        return;
    }
    const auto path = g_session_recorder.output_path();
    g_session_recorder.permit_finalization();
    if (finalize_recording_if_needed(hwnd))
        post_codex_status(hwnd, std::move(status) + L" Demo recording saved: " + path);
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

void center_owned_window(HWND window, HWND owner) {
    RECT wr{}, orc{};
    if (!GetWindowRect(window, &wr) || !owner || !GetWindowRect(owner, &orc)) return;
    const int width = wr.right - wr.left;
    const int height = wr.bottom - wr.top;
    const int x = orc.left + ((orc.right - orc.left) - width) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - height) / 2;
    SetWindowPos(window, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK review_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND title = CreateWindowW(L"STATIC", L"Astra has finished this pass. Review the artwork, then accept it or request another pass.",
                                       WS_CHILD | WS_VISIBLE, 18, 16, 454, 38, hwnd, nullptr, nullptr, nullptr);
            if (title) SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            HWND summary_label = CreateWindowW(L"STATIC", L"AGENT SUMMARY", WS_CHILD | WS_VISIBLE,
                                               18, 62, 120, 20, hwnd, nullptr, nullptr, nullptr);
            if (summary_label) SendMessageW(summary_label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            HWND summary = CreateWindowW(L"STATIC", g_review_summary_text.empty() ? L"Artwork submitted for review." : g_review_summary_text.c_str(),
                                         WS_CHILD | WS_VISIBLE, 18, 84, 454, 54, hwnd, nullptr, nullptr, nullptr);
            if (summary) SendMessageW(summary, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            HWND feedback_label = CreateWindowW(L"STATIC", L"CHANGES", WS_CHILD | WS_VISIBLE,
                                                18, 146, 100, 20, hwnd, nullptr, nullptr, nullptr);
            if (feedback_label) SendMessageW(feedback_label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            g_review_feedback = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                                18, 168, 454, 92, hwnd,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REVIEW_FEEDBACK)), nullptr, nullptr);
            if (g_review_feedback) SendMessageW(g_review_feedback, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            HWND accept = CreateWindowW(L"BUTTON", L"Accept", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                        18, 276, 140, 32, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REVIEW_ACCEPT)), nullptr, nullptr);
            HWND changes = CreateWindowW(L"BUTTON", L"Request changes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         170, 276, 302, 32, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REVIEW_CHANGES)), nullptr, nullptr);
            if (accept) SendMessageW(accept, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (changes) SendMessageW(changes, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (g_review_feedback) SetFocus(g_review_feedback);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            HWND owner = GetWindow(hwnd, GW_OWNER);
            if (id == ID_REVIEW_ACCEPT) {
                std::string error;
                {
                    std::lock_guard lock(g_state_mutex);
                    g_app.task.user_accept_review(&error);
                }
                if (!error.empty()) {
                    show_error(hwnd, utf8_to_wide(error));
                    return 0;
                }

                // Keep recording for a short tail after the click so the demo
                // captures the review popup disappearing and the accepted final state.
                if (g_session_recorder.active()) {
                    g_session_recorder.permit_finalization();
                    post_codex_status(owner, L"Artwork accepted. Finalizing demo recording...");
                } else {
                    post_codex_status(owner, L"Artwork accepted.");
                }
                DestroyWindow(hwnd);
                if (owner) {
                    PostMessageW(owner, WM_AGENT_UPDATED, 0, 0);
                    if (g_session_recorder.active()) SetTimer(owner, RECORDING_FINALIZE_TIMER, 750, nullptr);
                }
                return 0;
            }
            if (id == ID_REVIEW_CHANGES) {
                const std::wstring feedback_w = get_window_text(g_review_feedback);
                if (feedback_w.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                    show_error(hwnd, L"Enter the changes you want Astra to make.");
                    return 0;
                }
                const std::string feedback = wide_to_utf8(feedback_w);
                std::string error;
                {
                    std::lock_guard lock(g_state_mutex);
                    g_app.task.user_request_changes(feedback, &error);
                }
                if (!error.empty()) {
                    show_error(hwnd, utf8_to_wide(error));
                    return 0;
                }
                DestroyWindow(hwnd);
                if (owner) {
                    post_codex_status(owner, g_session_recorder.active()
                        ? L"Revision requested; demo recording continues through the next pass..."
                        : L"Revision requested; continuing from the current canvas...");
                    start_automatic_generation(owner, feedback);
                }
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            HWND owner = GetWindow(hwnd, GW_OWNER);
            g_review_feedback = nullptr;
            g_review_window = nullptr;
            if (owner) {
                EnableWindow(owner, TRUE);
                SetForegroundWindow(owner);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void show_user_review_window(HWND owner) {
    if (!owner) return;
    if (g_review_window) {
        ShowWindow(g_review_window, SW_SHOW);
        SetForegroundWindow(g_review_window);
        return;
    }

    AgentTaskSnapshot snapshot;
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_app.task.snapshot();
    }
    if (!snapshot.awaiting_user_review) return;
    g_review_summary_text = utf8_to_wide(snapshot.review_summary);

    static ATOM review_class = 0;
    if (!review_class) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = review_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PixelForgeUserReviewWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        review_class = RegisterClassW(&wc);
        if (!review_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            show_error(owner, L"Could not create the user review window.");
            return;
        }
    }

    g_review_window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                      L"PixelForgeUserReviewWindow", L"Review artwork",
                                      WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 510, 360,
                                      owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_review_window) {
        show_error(owner, L"Could not open the user review window.");
        return;
    }

    EnableWindow(owner, FALSE);
    center_owned_window(g_review_window, owner);
    ShowWindow(g_review_window, SW_SHOW);
    UpdateWindow(g_review_window);
    SetForegroundWindow(g_review_window);
}

void maybe_show_pending_review(HWND hwnd) {
    if (!g_review_popup_pending || g_review_window || g_codex_client.busy()) return;

    bool animating = false;
    AgentTaskSnapshot snapshot;
    {
        std::lock_guard lock(g_state_mutex);
        animating = g_app.playback.animating();
        snapshot = g_app.task.snapshot();
    }
    if (!snapshot.awaiting_user_review) {
        g_review_popup_pending = false;
        return;
    }
    if (animating) return;

    g_review_popup_pending = false;
    show_user_review_window(hwnd);
}

void start_automatic_generation(HWND hwnd, std::string review_feedback) {
    if (g_codex_client.busy()) {
        show_error(hwnd, L"Codex is already working on the current PixelForge task.");
        return;
    }

    if (review_feedback.empty()) {
        AgentTaskSnapshot snapshot;
        {
            std::lock_guard lock(g_state_mutex);
            snapshot = g_app.task.snapshot();
        }
        if (snapshot.awaiting_user_review) {
            g_review_popup_pending = false;
            show_user_review_window(hwnd);
            return;
        }
    }

    const std::wstring prompt_w = get_window_text(g_prompt);
    const auto first_non_ws = prompt_w.find_first_not_of(L" \t\r\n");
    if (first_non_ws == std::wstring::npos) {
        show_error(hwnd, L"Enter a PixelForge prompt before generating.");
        return;
    }
    if (g_repo_root.empty()) {
        show_error(hwnd, L"PixelForge could not resolve its repository root. Expected a checkout containing CMakeLists.txt and src\\win32\\main_auto.cpp. You can override it with PIXELFORGE_REPO_ROOT.");
        return;
    }

    g_local_tool_session.stop();
    std::wstring local_error;
    if (!start_local_tools(hwnd, local_error)) {
        show_error(hwnd, local_error);
        return;
    }

    std::string prompt = wide_to_utf8(prompt_w);
    if (!review_feedback.empty()) {
        prompt += "\n\nUser review feedback:\n" + review_feedback +
                  "\n\nContinue editing the existing canvas. Preserve what already works and make the requested changes.";
    }
    const std::string reasoning_effort = selected_reasoning_effort();
    {
        std::lock_guard lock(g_state_mutex);
        g_app.task.begin(prompt);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    post_codex_status(hwnd, review_feedback.empty()
        ? L"Codex: queued (" + utf8_to_wide(reasoning_effort) + L")..."
        : L"Codex: revision pass queued (" + utf8_to_wide(reasoning_effort) + L")...");

    pixelforge::win32::CodexGenerateRequest request;
    request.repo_root = g_repo_root;
    request.executable_path = g_executable_path.empty() ? g_repo_root : g_executable_path;
    request.prompt = prompt;
    request.agent_contract = read_utf8_file(std::filesystem::path(g_repo_root) / L"config" / L"agent_system_prompt.md");
    if (request.agent_contract.empty()) request.agent_contract = kFallbackAgentContract;
    request.reasoning_effort = reasoning_effort;
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

            AgentTaskSnapshot snapshot;
            {
                std::lock_guard lock(g_state_mutex);
                snapshot = g_app.task.snapshot();
            }
            if (!ok) {
                post_codex_status(hwnd, g_session_recorder.active()
                    ? L"Codex error: " + message + L" Recording remains active so you can continue the artwork."
                    : L"Codex error: " + message);
            } else if (snapshot.awaiting_user_review) {
                post_codex_status(hwnd, g_session_recorder.active()
                    ? L"Astra submitted this pass for your review. Demo recording is still running."
                    : L"Astra submitted this pass for your review.");
                PostMessageW(hwnd, WM_SHOW_USER_REVIEW, 0, 0);
            } else if (snapshot.state == TaskState::Finished) {
                post_codex_status(hwnd, L"Artwork complete.");
            } else if (snapshot.state == TaskState::Rejected) {
                finalize_terminal_recording(hwnd, L"Codex rejected task: " + utf8_to_wide(snapshot.status_message));
            } else if (snapshot.state == TaskState::Aborted) {
                finalize_terminal_recording(hwnd, L"Codex aborted task: " + utf8_to_wide(snapshot.status_message));
            } else {
                post_codex_status(hwnd, L"Codex turn ended without submitting the artwork for review.");
            }
            PostMessageW(hwnd, WM_AGENT_UPDATED, 0, 0);
        },
        error);

    if (!started) {
        g_local_tool_session.stop();
        post_codex_status(hwnd, g_session_recorder.active()
            ? L"Codex could not start; demo recording remains active."
            : L"Codex: idle.");
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

        HWND intelligence_label = CreateWindowW(L"STATIC", L"INTELLIGENCE", WS_CHILD | WS_VISIBLE,
                                                12, 272, 96, 20, hwnd, nullptr, nullptr, nullptr);
        if (intelligence_label) SendMessageW(intelligence_label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        g_intelligence = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      112, 268, 212, 160, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INTELLIGENCE)), nullptr, nullptr);
        if (g_intelligence) {
            SendMessageW(g_intelligence, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            for (const wchar_t* label : {L"Low", L"Medium", L"High", L"Extra High", L"Max"})
                SendMessageW(g_intelligence, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(g_intelligence, CB_SETCURSEL, 1, 0);
        }

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
        post_codex_status(hwnd, g_session_recorder.active()
            ? L"Codex: stopping; existing canvas and demo recording will be kept."
            : L"Codex: stopping; existing canvas will be kept.");
        return 0;
    }
    if (msg == WM_CODEX_STATUS) {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
        if (text && g_codex_status) SetWindowTextW(g_codex_status, text->c_str());
        return 0;
    }
    if (msg == WM_SHOW_USER_REVIEW) {
        g_review_popup_pending = true;
        maybe_show_pending_review(hwnd);
        return 0;
    }
    if (msg == WM_TIMER && wparam == RECORDING_FINALIZE_TIMER) {
        KillTimer(hwnd, RECORDING_FINALIZE_TIMER);
        const auto path = g_session_recorder.output_path();
        if (finalize_recording_if_needed(hwnd))
            post_codex_status(hwnd, path.empty()
                ? L"Artwork accepted."
                : L"Artwork accepted. Demo recording saved: " + path);
        return 0;
    }
    if (msg == WM_TIMER && wparam == CODEX_REFRESH_TIMER) {
        maybe_show_pending_review(hwnd);
        if (g_codex_client.busy() || g_session_recorder.active() || g_review_popup_pending)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_DESTROY) {
        KillTimer(hwnd, CODEX_REFRESH_TIMER);
        KillTimer(hwnd, RECORDING_FINALIZE_TIMER);
        if (g_review_window) DestroyWindow(g_review_window);
        g_codex_client.shutdown();
        g_local_tool_session.stop();
        g_session_recorder.permit_finalization();
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
    g_session_recorder.set_user_review_gate(true);

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
    g_session_recorder.permit_finalization();
    finalize_recording_if_needed(hwnd);
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
