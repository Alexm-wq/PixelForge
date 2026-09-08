#include "LocalAgentToolSession.hpp"
#include "ProjectLoader.hpp"
#include "MiniJson.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
using namespace pixelforge;
using namespace pixelforge::win32;
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
int main() {
    auto root = std::filesystem::current_path() / "test-output" / ("large-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root);
    HWND hwnd = CreateWindowW(L"STATIC", L"large-test", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, nullptr, nullptr);
    PixelDocument document; AgentTaskController task(document);
    ImageData content, style; std::wstring content_path, style_path, error;
    std::mutex mutex; SessionRecorder recorder; LocalAgentToolSession tools;
    AgentMcpBindings bindings{&document, &task, &content, &style, &content_path, &style_path, &mutex, hwnd};
    CHECK(tools.start(bindings, {&recorder, &task, &mutex, hwnd, (root / "recordings").wstring()}, error));
    auto id = task.begin("Large workspace regressions");
    auto call = [&](std::string body) { return tools.call("pixelforge_pack", "{\"task_id\":" + std::to_string(id) + "," + body + "}"); };
    auto fields = [](const LocalToolResult& r) { FlatJsonObject o; std::string e; CHECK(parse_flat_json_object(r.text, o, e)); return o; };
    CHECK(call("\"action\":\"create\",\"canvases\":\"a,g,8,8,0|b,g,8,8,1\"").success);
    CHECK(call("\"action\":\"edit\",\"canvas\":\"a\",\"patch\":\"P,1,1,#FFFF0000\"").success);
    CHECK(call("\"action\":\"program\",\"program\":\"CANVAS a;P 1 1 #FFFF0000;CANVAS b;P 2 2 #FF00FF00\"").success);
    CHECK(call("\"action\":\"history\",\"direction\":\"undo\"").success);
    CHECK(document.pixel(1,1) == 0xffff0000); // No-op frame must retain older edit.
    const auto project = root / "projects" / ("task_" + std::to_string(id));
    const auto stable_frame = project / "canvases" / "g" / "b.png";
    const auto stable_time = std::filesystem::last_write_time(stable_frame);
    auto edit = call("\"action\":\"edit\",\"canvas\":\"a\",\"patch\":\"P,0,0,#00123456\"");
    CHECK(edit.success && fields(edit).get_i64("saved_canvases") == 1);
    CHECK(std::filesystem::last_write_time(stable_frame) == stable_time);
    auto noop = call("\"action\":\"edit\",\"canvas\":\"a\",\"patch\":\"P,0,0,#00123456\"");
    CHECK(noop.success && fields(noop).get_i64("saved_canvases") == 0);
    const auto manifest = project / "project.json";
    HANDLE lock = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    edit = call("\"action\":\"edit\",\"canvas\":\"a\",\"patch\":\"P,3,3,#FFFFFFFF\"");
    CHECK(edit.success && fields(edit).get_bool("autosave_ok") == false);
    CHECK(document.pixel(3,3) == 0xffffffff && std::filesystem::exists(project / ".autosave-pending"));
    CloseHandle(lock);
    CHECK(call("\"action\":\"save\"").success);
    CHECK(!std::filesystem::exists(project / ".autosave-pending"));

    // Bulk reopen preserves hidden RGB and creates no artificial undo history.
    ProjectLoadSummary summary;
    CHECK(load_project_into_workspace(manifest.wstring(), root.wstring(), tools, task, document, mutex, summary, error));
    id = summary.task_id;
    CHECK(document.pixel(0,0) == 0x00123456 && document.pixel(1,1) == 0xffff0000);
    CHECK(!call("\"action\":\"history\",\"direction\":\"undo\"").success);
    edit = call("\"action\":\"edit\",\"canvas\":\"a\",\"patch\":\"P,4,4,#FFFFFFFF\"");
    CHECK(edit.success && fields(edit).get_i64("saved_canvases") == 1);

    // 200 animations, 8 frames each. Measure native tool work, not model usage.
    std::string specs;
    for (int group = 0; group < 200; ++group) for (int frame = 0; frame < 8; ++frame) {
        if (!specs.empty()) specs += '|';
        const auto g = "anim" + std::to_string(group);
        specs += g + "_" + std::to_string(frame) + "," + g + ",64,64," + std::to_string(frame);
    }
    const auto start = std::chrono::steady_clock::now();
    auto created = call("\"action\":\"create\",\"seed_from_source\":false,\"canvases\":" + json_quote(specs));
    CHECK(created.success && fields(created).get_bool("autosave_ok") == true);
    const auto creation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    auto listed = call("\"action\":\"list\"");
    CHECK(listed.success && fields(listed).get_i64("canvas_count") == 1600);
    CHECK(fields(listed).get_i64("returned_count") == 64 && fields(listed).get_i64("next_offset") == 64);
    CHECK(listed.text.size() < 16000);
    auto second = call("\"action\":\"list\",\"offset\":64");
    CHECK(second.success && fields(second).get("canvases") != fields(listed).get("canvases"));
    auto analyzed = call("\"action\":\"analyze\",\"group\":\"anim0\"");
    CHECK(analyzed.success && fields(analyzed).get_i64("scanned_frames") == 8 && analyzed.image_base64.empty());
    const auto analyze_start = std::chrono::steady_clock::now();
    analyzed = call("\"action\":\"analyze\",\"group\":\"anim0\"");
    const auto cached_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - analyze_start).count();
    CHECK(fields(analyzed).get_i64("scanned_frames") == 0 && fields(analyzed).get_i64("compared_pairs") == 0);
    auto view = call("\"action\":\"view\",\"scale\":32");
    CHECK(view.success && !view.image_base64.empty() && fields(view).get_i64("returned_count") == 32);
    CHECK(fields(view).get_i64("width").value() * fields(view).get_i64("height").value() <= 1048576);
    const auto image_chars = view.image_base64.size();
    auto repeat = call("\"action\":\"view\",\"scale\":32");
    CHECK(repeat.success && repeat.image_base64.empty() && fields(repeat).get_bool("unchanged") == true);
    const auto edit_start = std::chrono::steady_clock::now();
    edit = call("\"action\":\"edit\",\"canvas\":\"anim0_0\",\"patch\":\"P,1,1,#FFFFFFFF\"");
    const auto edit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - edit_start).count();
    CHECK(edit.success && fields(edit).get_i64("saved_canvases") == 1);
    analyzed = call("\"action\":\"analyze\",\"group\":\"anim0\"");
    CHECK(fields(analyzed).get_i64("scanned_frames") == 1 && fields(analyzed).get_i64("compared_pairs") == 2);
    const auto summary_start = std::chrono::steady_clock::now();
    auto overview = call("\"action\":\"analyze\",\"summary\":true,\"limit\":256");
    const auto summary_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - summary_start).count();
    CHECK(overview.success && fields(overview).get_i64("group_count") == 200 && fields(overview).get_i64("returned_count") == 200);
    CHECK(overview.image_base64.empty() && overview.text.size() < 16000);
    auto cached_overview = call("\"action\":\"analyze\",\"summary\":true,\"limit\":256");
    CHECK(fields(cached_overview).get_i64("scanned_frames") == 0);
    CHECK(!call("\"action\":\"view\",\"canvases\":\"anim0_0|missing\"").success);
    auto exact = call("\"action\":\"inspect\",\"canvas\":\"anim0_0\",\"width\":64,\"height\":64");
    CHECK(exact.success && fields(exact).get_i64("returned_pixels") == 1024 && fields(exact).get_i64("next_pixel_offset") == 1024);
    exact = call("\"action\":\"inspect\",\"canvas\":\"anim0_0\",\"width\":64,\"height\":64,\"pixel_offset\":1024,\"pixel_limit\":4096");
    CHECK(exact.success && fields(exact).get_i64("returned_pixels") == 3072 && fields(exact).get_i64("next_pixel_offset") == -1);
    CHECK(!call("\"action\":\"copy\",\"source\":\"anim0_0\",\"dest\":\"anim0_1\",\"sx\":4294967296").success);
    std::cout << "200 animations / 1600 64x64 frames: create+save=" << creation_ms << "ms, single-frame edit+save=" << edit_ms
              << "ms, cached analysis=" << cached_us << "us, list=" << listed.text.size() << " bytes, analysis=" << analyzed.text.size()
              << " bytes, preview=" << image_chars << " base64 chars; repeated preview=0; all-group analysis=" << summary_ms << "ms / " << overview.text.size() << " bytes.\n";
    tools.stop(); DestroyWindow(hwnd);
}
