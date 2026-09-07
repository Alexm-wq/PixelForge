// Include the client implementation so the fake peer can use the same JSON
// framing helpers. This executable never launches a real model/server.
#include "CodexAppClientDynamic.cpp"
#include <iostream>
#include <future>
#include <cstdlib>

using namespace pixelforge;
using namespace pixelforge::win32;
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace {
void emit(const std::string& json) { std::cout << json << '\n' << std::flush; }
std::string invoke(int id, const std::string& tool, const std::string& args, bool expected_success = true) {
    emit("{\"id\":" + std::to_string(id) + ",\"method\":\"item/tool/call\",\"params\":{\"tool\":\"pixelforge_" + tool + "\",\"arguments\":" + args + "}}");
    std::string line; CHECK(static_cast<bool>(std::getline(std::cin, line)));
    CHECK(line.find(expected_success ? "\"success\":true" : "\"success\":false") != std::string::npos);
    // A tool result carries a text item followed by optional image items.
    const auto marker = line.find("\"text\":"); CHECK(marker != std::string::npos);
    std::size_t p = marker + 7; std::string text; CHECK(parse_string(line, p, text));
    if (tool == "view") CHECK(line.find("data:image/png;base64,") != std::string::npos);
    return text;
}
int mock_server() {
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto method = nested_string(line, {"method"});
        std::string id; extract_member_raw(line, "id", id);
        if (method == "initialize") emit("{\"id\":" + id + ",\"result\":{}}");
        else if (method == "thread/start") {
            CHECK(line.find("pixelforge_edit") != std::string::npos);
            CHECK(line.find("pixelforge_program") != std::string::npos);
            CHECK(!nested_string(line, {"params", "baseInstructions"}).empty());
            emit("{\"id\":" + id + ",\"result\":{\"thread\":{\"id\":\"test-thread\"}}}");
        } else if (method == "turn/start") {
            CHECK(nested_string(line, {"params", "effort"}) == "medium");
            emit("{\"id\":" + id + ",\"result\":{\"turn\":{\"id\":\"test-turn\"}}}");
            const auto mode = get_env(L"PIXELFORGE_TEST_MODE");
            if (mode == L"stall" || mode == L"cancel") {
                for (;;) { emit("{\"method\":\"thread/tokenUsage/updated\",\"params\":{\"totalTokens\":100}}"); Sleep(20); }
            }
            auto text = invoke(0, "task", "{\"action\":\"get\"}");
            FlatJsonObject task; std::string error; CHECK(parse_flat_json_object(text, task, error));
            const auto task_id = std::to_string(*task.get_i64("task_id"));
            if (mode == L"errors") {
                for (int i = 1; i <= 3; ++i) invoke(i, "palette", "{\"action\":\"set\",\"colors\":\"bad\"}", false);
                Sleep(10000); return 1;
            }
            emit("{\"method\":\"item/completed\",\"params\":{\"item\":{\"type\":\"agentMessage\",\"text\":\"Drawing the silhouette now.\"}}}");
            emit("{\"method\":\"thread/tokenUsage/updated\",\"params\":{\"tokenUsage\":{\"total\":{\"inputTokens\":1000,\"cachedInputTokens\":900,\"outputTokens\":10},\"last\":{\"inputTokens\":500,\"cachedInputTokens\":450,\"outputTokens\":5}}}}");
            if (mode != L"incomplete") {
                text = invoke(1, "task", "{\"action\":\"accept\",\"task_id\":" + task_id + ",\"width\":8,\"height\":8}");
                CHECK(parse_flat_json_object(text, task, error));
                auto rev = std::to_string(*task.get_i64("revision"));
                text = invoke(2, "edit", "{\"task_id\":" + task_id + ",\"expected_revision\":" + rev + ",\"patch\":\"R,1,1,6,6,#FF123456\"}");
                CHECK(parse_flat_json_object(text, task, error));
                rev = std::to_string(*task.get_i64("revision"));
                const auto view_args = "{\"action\":\"render\",\"task_id\":" + task_id + ",\"expected_revision\":" + rev + ",\"scale\":1}";
                invoke(3, "view", view_args);
                emit("{\"id\":30,\"method\":\"item/tool/call\",\"params\":{\"tool\":\"pixelforge_view\",\"arguments\":" + view_args + "}}");
                CHECK(static_cast<bool>(std::getline(std::cin, line)));
                CHECK(line.find("inputImage") == std::string::npos && line.find("unchanged") != std::string::npos);
                auto resend_args = view_args; resend_args.pop_back(); resend_args += ",\"resend_image\":true}";
                invoke(31, "view", resend_args);
                invoke(4, "task", "{\"action\":\"finish\",\"task_id\":" + task_id + ",\"expected_revision\":" + rev + "}");
            }
            emit("{\"method\":\"turn/completed\",\"params\":{\"turn\":{\"status\":\"completed\"}}}");
        }
    }
    return 0;
}
}

int main(int argc, char**) {
    if (argc > 1) return mock_server();
    const auto exe = [] { wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768); return std::wstring(path); }();
    SetEnvironmentVariableW(L"PIXELFORGE_CODEX_EXE", exe.c_str());
    const auto test_log = std::filesystem::path(exe).parent_path() / L"offline-session.log";
    SetEnvironmentVariableW(L"PIXELFORGE_TRACE_PATH", test_log.c_str());
    HWND hwnd = CreateWindowW(L"STATIC", L"test", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, nullptr, nullptr);
    CHECK(hwnd);
    PixelDocument document; AgentTaskController task(document);
    ImageData content, style; std::wstring content_path, style_path;
    std::mutex mutex; SessionRecorder recorder;
    AgentMcpBindings bindings{&document, &task, &content, &style, &content_path, &style_path, &mutex, hwnd};
    LocalRecordBindings recording{&recorder, &task, &mutex, hwnd, L"build/test-output"};
    LocalAgentToolSession tools;
    const HANDLE stdin_before = GetStdHandle(STD_INPUT_HANDLE), stdout_before = GetStdHandle(STD_OUTPUT_HANDLE);
    for (int i = 0; i < 3; ++i) {
        std::wstring error; CHECK(tools.start(bindings, recording, error));
        task.begin("Repeat session");
        const auto result = tools.call("pixelforge_task", "{\"action\":\"get\"}");
        CHECK(result.success);
        FlatJsonObject fields; std::string parse_error;
        CHECK(parse_flat_json_object(result.text, fields, parse_error));
        CHECK(fields.get_i64("task_id").value_or(0) == i + 1);
        tools.stop();
        CHECK(GetStdHandle(STD_INPUT_HANDLE) == stdin_before && GetStdHandle(STD_OUTPUT_HANDLE) == stdout_before);
    }
    {
        std::wstring error;
        const auto reference_path = std::filesystem::path(exe).parent_path() / L"automatic-reference.png";
        CHECK(save_png_wic(reference_path.wstring(), 512, 512, std::vector<std::uint32_t>(512 * 512, 0xff123456), error));
        CHECK(load_image_wic(reference_path.wstring(), content, error));
        content_path = reference_path.wstring();
        CHECK(tools.start(bindings, recording, error));
        const auto id = std::to_string(task.begin("Automatic state and observations"));
        auto reference_args = "{\"action\":\"content_reference\",\"task_id\":" + id + "}";
        const auto first = tools.call("pixelforge_view", reference_args);
        CHECK(first.success && !first.image_base64.empty());
        FlatJsonObject metadata; std::string parse_error;
        CHECK(parse_flat_json_object(first.text, metadata, parse_error));
        CHECK(*metadata.get_i64("width") * *metadata.get_i64("height") <= 65536);
        CHECK(content.width == 512 && content.height == 512);
        reference_args.pop_back(); reference_args += ",\"resend_image\":true}";
        const auto repeat = tools.call("pixelforge_view", reference_args);
        CHECK(repeat.success && repeat.image_base64.empty());
        CHECK(tools.call("pixelforge_task", "{\"action\":\"accept\",\"task_id\":" + id + ",\"width\":96,\"height\":64}").success);

        // High-level program: broad scene construction is expanded locally and
        // committed as one exact diff. Deliberate overshoot must clip, not fail.
        auto programmed = tools.call("pixelforge_program", "{\"task_id\":" + id +
            ",\"program\":\"CLEAR #FF112233;FELLIPSE 48 32 60 20 #FF335577;BOX -2 -2 12 12 #FFFFFFFF;Q 0 63 48 -8 95 63 #FF00FF00\",\"render_scale\":2}");
        CHECK(programmed.success && !programmed.image_base64.empty());
        CHECK(parse_flat_json_object(programmed.text, metadata, parse_error));
        CHECK(metadata.get_i64("commands").value_or(0) == 4);
        CHECK(metadata.get_i64("patch_operations").value_or(0) > 0);
        CHECK(metadata.get_i64("clipped_writes").value_or(0) > 0);
        CHECK(document.pixel(48, 32) == 0xff335577);

        const auto revision = document.revision();
        auto rejected = tools.call("pixelforge_edit", "{\"task_id\":" + id + ",\"patch\":\"H,0,0,97,#FFFFFFFF\",\"render_scale\":2}");
        CHECK(!rejected.success && rejected.image_base64.empty() && document.revision() == revision);
        CHECK(tools.call("pixelforge_edit", "{\"task_id\":" + id + ",\"patch\":\"P,0,0,#FFFFFFFF\"}").success);
        {
            auto mouse = document.begin_transaction();
            CHECK(mouse.set_pixel(1, 1, 0xff445566)); CHECK(mouse.commit());
        }
        auto stale = tools.call("pixelforge_edit", "{\"task_id\":" + id + ",\"patch\":\"P,1,1,#FFFFFFFF\"}");
        CHECK(!stale.success && document.pixel(1,1) == 0xff445566);
        // The stale response carries the authoritative revision; the next render
        // may omit it and should observe latest state instead of failing stale again.
        auto latest = tools.call("pixelforge_view", "{\"action\":\"render\",\"task_id\":" + id + ",\"scale\":1}");
        CHECK(latest.success && !latest.image_base64.empty());
        CHECK(tools.call("pixelforge_edit", "{\"task_id\":" + id + ",\"patch\":\"P,1,1,#FFFFFFFF\"}").success);
        tools.stop();
        std::cout << "Passed raster program, clipping, cached observations, edit/render and concurrent user edits.\n";
    }
    for (const auto mode : {L"success", L"incomplete", L"errors", L"stall", L"cancel"}) {
        SetEnvironmentVariableW(L"PIXELFORGE_TEST_MODE", mode);
        std::wstring error; CHECK(tools.start(bindings, recording, error));
        task.begin("Offline integration");
        CodexAppClient client;
        CodexGenerateRequest request;
        request.repo_root = std::filesystem::current_path().wstring();
        request.executable_path = exe; request.prompt = "Offline integration"; request.tool_session = &tools;
        request.progress_timeout_ms = std::wstring(mode) == L"stall" ? 300 : 10000;
        request.total_timeout_ms = 15000;
        std::promise<std::pair<bool, std::wstring>> done; auto future = done.get_future();
        CHECK(client.generate_async(request, {}, [&](bool ok, std::wstring message) { done.set_value({ok, message}); }, error));
        if (std::wstring(mode) == L"cancel") { Sleep(100); client.cancel(); }
        CHECK(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        const auto result = future.get();
        client.shutdown(); tools.stop();
        std::ifstream trace(test_log);
        const std::string log((std::istreambuf_iterator<char>(trace)), {});
        CHECK(log.find("TX event") == std::string::npos);
        if (std::wstring(mode) == L"success") {
            CHECK(log.find("TOOL result id=2 tool=pixelforge_edit success=true") != std::string::npos);
            CHECK(log.find("AGENT Drawing the silhouette now.") != std::string::npos);
            CHECK(log.find("uncachedInputTokens=100") != std::string::npos);
            CHECK(log.find("uncachedInputTokens=50") != std::string::npos);
        }
        if (std::wstring(mode) == L"errors") CHECK(log.find("invalid_palette") != std::string::npos);
        CHECK(result.first == (std::wstring(mode) == L"success"));
        if (std::wstring(mode) == L"success") CHECK(task.state() == TaskState::Finished && document.pixel(1,1) == 0xff123456);
        if (std::wstring(mode) == L"incomplete") CHECK(result.second.find(L"without finishing") != std::wstring::npos);
        if (std::wstring(mode) == L"errors") CHECK(result.second.find(L"three consecutive") != std::wstring::npos);
        if (std::wstring(mode) == L"stall") CHECK(result.second.find(L"no pixel edits") != std::wstring::npos);
        if (std::wstring(mode) == L"cancel") CHECK(result.second.find(L"Stopped by user") != std::wstring::npos);
        std::wcout << L"Passed automatic client scenario: " << mode << L'\n';
    }
    DestroyWindow(hwnd);
    std::cout << "Offline automatic generation tests passed; no model calls.\n";
}
