#include "AgentInteractionUi.hpp"

#include <commdlg.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelforge::win32 {
namespace {

constexpr UINT WM_AGENT_QUESTION = WM_APP + 0x441;
constexpr int ID_SOURCE_LOAD = 4101;
constexpr int ID_SOURCE_CLEAR = 4102;
constexpr int ID_DIALOG_EDIT = 4201;
constexpr int ID_DIALOG_SUBMIT = 4202;
constexpr int ID_DIALOG_CANCEL = 4203;
constexpr int ID_DIALOG_SUGGESTION_BASE = 4210;

std::mutex g_source_mutex;
ImageData g_source_image;
std::wstring g_source_path;
std::atomic<HWND> g_bound_owner{nullptr};
std::atomic<DWORD> g_ui_thread_id{0};
std::atomic<HWND> g_source_window{nullptr};
std::atomic<HWND> g_source_status{nullptr};

std::wstring utf8_to_wide_local(std::string_view text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return L"(invalid UTF-8)";
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8_local(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring file_name_only(const std::wstring& path) {
    const auto p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

ReferenceSlot source_slot_locked() {
    ReferenceSlot slot;
    if (g_source_image.valid()) {
        slot.path = wide_to_utf8_local(g_source_path);
        slot.width = g_source_image.width;
        slot.height = g_source_image.height;
        slot.present = true;
    }
    return slot;
}

void update_source_status() {
    const HWND status = g_source_status.load();
    if (!status) return;
    std::wstring text = L"No editable Source loaded.";
    {
        std::lock_guard lock(g_source_mutex);
        if (g_source_image.valid()) {
            text = L"Source: " + file_name_only(g_source_path) + L"  (" +
                   std::to_wstring(g_source_image.width) + L"x" +
                   std::to_wstring(g_source_image.height) + L")";
        }
    }
    SetWindowTextW(status, text.c_str());
}

void choose_source(HWND owner) {
    wchar_t path[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(std::size(path));
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp\0All files\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    ImageData image;
    std::wstring error;
    if (!load_image_wic(path, image, error)) {
        MessageBoxW(owner, error.c_str(), L"Could not load Source", MB_OK | MB_ICONERROR);
        return;
    }
    {
        std::lock_guard lock(g_source_mutex);
        g_source_image = std::move(image);
        g_source_path = path;
    }
    update_source_status();
}

void clear_source() {
    {
        std::lock_guard lock(g_source_mutex);
        g_source_image = {};
        g_source_path.clear();
    }
    update_source_status();
}

std::vector<std::string> split_suggestions(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size() && out.size() < 3) {
        const auto end = text.find('|', start);
        std::string item(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        const auto first = item.find_first_not_of(" \t\r\n");
        const auto last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) out.push_back(item.substr(first, last - first + 1));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

struct QuestionRequest {
    std::string reason;
    std::string question;
    std::string suggestions;
    std::string answer;
    bool cancelled = false;
    HANDLE done = nullptr;
};

struct QuestionWindowState {
    QuestionRequest* request = nullptr;
    HWND edit = nullptr;
    std::vector<std::string> suggestions;
};

void finish_question(HWND hwnd, QuestionWindowState* state, std::string answer, bool cancelled) {
    if (!state || !state->request) return;
    state->request->answer = std::move(answer);
    state->request->cancelled = cancelled;
    SetEvent(state->request->done);
    DestroyWindow(hwnd);
}

LRESULT CALLBACK question_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<QuestionWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            state = reinterpret_cast<QuestionWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!state || !state->request) return -1;
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            auto make_static = [&](const std::wstring& text, int x, int y, int w, int h) {
                HWND c = CreateWindowW(L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE,
                                       x, y, w, h, hwnd, nullptr, nullptr, nullptr);
                if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            };
            make_static(L"Astra paused because:", 18, 14, 500, 18);
            make_static(utf8_to_wide_local(state->request->reason), 18, 34, 500, 54);
            make_static(L"Question:", 18, 94, 500, 18);
            make_static(utf8_to_wide_local(state->request->question), 18, 114, 500, 54);
            make_static(L"Your answer:", 18, 174, 500, 18);
            state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                18, 194, 500, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIALOG_EDIT)), nullptr, nullptr);
            if (state->edit) SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            state->suggestions = split_suggestions(state->request->suggestions);
            int y = 232;
            for (std::size_t i = 0; i < state->suggestions.size(); ++i) {
                HWND b = CreateWindowW(L"BUTTON", utf8_to_wide_local(state->suggestions[i]).c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    18, y, 500, 28, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIALOG_SUGGESTION_BASE + i)), nullptr, nullptr);
                if (b) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                y += 34;
            }
            HWND submit = CreateWindowW(L"BUTTON", L"Send answer",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                18, 344, 328, 30, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIALOG_SUBMIT)), nullptr, nullptr);
            HWND cancel = CreateWindowW(L"BUTTON", L"Dismiss",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                356, 344, 162, 30, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIALOG_CANCEL)), nullptr, nullptr);
            if (submit) SendMessageW(submit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (cancel) SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (state->edit) SetFocus(state->edit);
            return 0;
        }
        case WM_COMMAND: {
            if (!state || !state->request) break;
            const int id = LOWORD(wparam);
            if (id >= ID_DIALOG_SUGGESTION_BASE && id < ID_DIALOG_SUGGESTION_BASE + 3) {
                const auto index = static_cast<std::size_t>(id - ID_DIALOG_SUGGESTION_BASE);
                if (index < state->suggestions.size()) finish_question(hwnd, state, state->suggestions[index], false);
                return 0;
            }
            if (id == ID_DIALOG_SUBMIT) {
                const int len = state->edit ? GetWindowTextLengthW(state->edit) : 0;
                std::wstring text(static_cast<std::size_t>(len + 1), L'\0');
                if (state->edit && len > 0) GetWindowTextW(state->edit, text.data(), len + 1);
                text.resize(static_cast<std::size_t>(len));
                if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                    MessageBoxW(hwnd, L"Enter an answer or choose one of Astra's suggestions.",
                                L"Answer required", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                finish_question(hwnd, state, wide_to_utf8_local(text), false);
                return 0;
            }
            if (id == ID_DIALOG_CANCEL) { finish_question(hwnd, state, {}, true); return 0; }
            break;
        }
        case WM_CLOSE:
            if (state && state->request) finish_question(hwnd, state, {}, true);
            else DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK source_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND label = CreateWindowW(L"STATIC", L"Editable Source reference",
                WS_CHILD | WS_VISIBLE, 12, 10, 260, 18, hwnd, nullptr, nullptr, nullptr);
            HWND status = CreateWindowW(L"STATIC", L"No editable Source loaded.",
                WS_CHILD | WS_VISIBLE, 12, 32, 360, 36, hwnd, nullptr, nullptr, nullptr);
            HWND load = CreateWindowW(L"BUTTON", L"Load Source...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 74, 180, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SOURCE_LOAD)), nullptr, nullptr);
            HWND clear = CreateWindowW(L"BUTTON", L"Clear",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 202, 74, 90, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SOURCE_CLEAR)), nullptr, nullptr);
            for (HWND c : {label, status, load, clear}) if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            g_source_status = status;
            update_source_status();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == ID_SOURCE_LOAD) { choose_source(hwnd); return 0; }
            if (LOWORD(wparam) == ID_SOURCE_CLEAR) { clear_source(); return 0; }
            break;
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        case WM_DESTROY: g_source_window = nullptr; g_source_status = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, 0);
}

void center_near_owner(HWND window) {
    const HWND owner = g_bound_owner.load();
    if (!window || !owner) return;
    RECT wr{}, orc{};
    if (!GetWindowRect(window, &wr) || !GetWindowRect(owner, &orc)) return;
    SetWindowPos(window, HWND_TOP,
                 orc.right - (wr.right - wr.left) - 24, orc.top + 64,
                 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
}

void ui_thread_main() {
    g_ui_thread_id = GetCurrentThreadId();
    MSG prime{};
    PeekMessageW(&prime, nullptr, 0, 0, PM_NOREMOVE);
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW source_class{};
    source_class.lpfnWndProc = source_wndproc;
    source_class.hInstance = instance;
    source_class.lpszClassName = L"PixelForgeSourceReferenceWindow";
    source_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    source_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&source_class);

    WNDCLASSW question_class{};
    question_class.lpfnWndProc = question_wndproc;
    question_class.hInstance = instance;
    question_class.lpszClassName = L"PixelForgeAgentQuestionWindow";
    question_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    question_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&question_class);

    HWND source = CreateWindowExW(WS_EX_TOOLWINDOW,
        source_class.lpszClassName, L"PixelForge Source",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 150,
        nullptr, nullptr, instance, nullptr);
    g_source_window = source;
    if (source) { ShowWindow(source, SW_SHOWNA); UpdateWindow(source); }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_AGENT_QUESTION) {
            auto* request = reinterpret_cast<QuestionRequest*>(msg.lParam);
            if (!request) continue;
            auto* state = new QuestionWindowState;
            state->request = request;
            HWND question = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                question_class.lpszClassName, L"Astra needs your input",
                WS_CAPTION | WS_SYSMENU | WS_POPUP,
                CW_USEDEFAULT, CW_USEDEFAULT, 554, 430,
                g_bound_owner.load(), nullptr, instance, state);
            if (!question) {
                delete state;
                request->cancelled = true;
                SetEvent(request->done);
                continue;
            }
            ShowWindow(question, SW_SHOW);
            UpdateWindow(question);
            SetForegroundWindow(question);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

struct UiBootstrap {
    UiBootstrap() { std::thread(ui_thread_main).detach(); }
};
UiBootstrap g_bootstrap;

} // namespace

void agent_interaction_bind_task(AgentTaskController* task, std::mutex* state_mutex, HWND owner) {
    g_bound_owner = owner;
    if (task && state_mutex) {
        ReferenceSlot slot;
        {
            std::lock_guard lock(g_source_mutex);
            slot = source_slot_locked();
        }
        std::lock_guard state_lock(*state_mutex);
        task->set_source_reference(std::move(slot));
    }
    const HWND source = g_source_window.load();
    if (source) {
        center_near_owner(source);
        ShowWindow(source, SW_SHOWNA);
    }
}

bool agent_interaction_source_snapshot(ImageData& image, std::wstring& path) {
    std::lock_guard lock(g_source_mutex);
    if (!g_source_image.valid()) return false;
    image = g_source_image;
    path = g_source_path;
    return true;
}

void agent_interaction_set_source_for_testing(ImageData image, std::wstring path) {
    {
        std::lock_guard lock(g_source_mutex);
        g_source_image = std::move(image);
        g_source_path = std::move(path);
    }
    update_source_status();
}

bool agent_interaction_ask_user(std::string reason,
                                std::string question,
                                std::string suggestions,
                                std::string& answer,
                                bool& cancelled,
                                std::wstring& error) {
    wchar_t test_answer[4096]{};
    const DWORD test_len = GetEnvironmentVariableW(
        L"PIXELFORGE_DIALOG_TEST_ANSWER", test_answer,
        static_cast<DWORD>(std::size(test_answer)));
    if (test_len > 0 && test_len < std::size(test_answer)) {
        answer = wide_to_utf8_local(std::wstring_view(test_answer, test_len));
        cancelled = false;
        return true;
    }

    const DWORD thread_id = g_ui_thread_id.load();
    if (!thread_id) {
        error = L"PixelForge interaction UI is not ready.";
        return false;
    }

    QuestionRequest request;
    request.reason = std::move(reason);
    request.question = std::move(question);
    request.suggestions = std::move(suggestions);
    request.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request.done) {
        error = L"Could not create the agent-question synchronization event.";
        return false;
    }
    if (!PostThreadMessageW(thread_id, WM_AGENT_QUESTION, 0,
                            reinterpret_cast<LPARAM>(&request))) {
        CloseHandle(request.done);
        error = L"Could not open the Astra question popup.";
        return false;
    }
    WaitForSingleObject(request.done, INFINITE);
    CloseHandle(request.done);
    answer = std::move(request.answer);
    cancelled = request.cancelled;
    return true;
}

} // namespace pixelforge::win32
