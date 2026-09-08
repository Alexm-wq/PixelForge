#include "LocalAgentToolSession.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace pixelforge;
using namespace pixelforge::win32;

#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

int main() {
    HWND hwnd = CreateWindowW(L"STATIC", L"pack-test", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, nullptr, nullptr);
    CHECK(hwnd);

    PixelDocument document;
    AgentTaskController task(document);
    ImageData content, style;
    std::wstring content_path, style_path;
    std::mutex mutex;
    SessionRecorder recorder;
    AgentMcpBindings bindings{&document, &task, &content, &style, &content_path, &style_path, &mutex, hwnd};
    const auto output_dir = std::filesystem::current_path() / "test-output" / "multicanvas";
    LocalRecordBindings recording{&recorder, &task, &mutex, hwnd, output_dir.wstring()};

    LocalAgentToolSession tools;
    std::wstring error;
    CHECK(tools.start(bindings, recording, error));
    const auto id = task.begin("Build a three-frame walk cycle");
    const auto id_text = std::to_string(id);

    auto created = tools.call("pixelforge_pack",
        "{\"action\":\"create\",\"task_id\":" + id_text +
        ",\"canvases\":\"walk_00,walk,8,8,0|walk_01,walk,8,8,1|walk_02,walk,8,8,2\"}");
    CHECK(created.success);
    CHECK(created.text.find("\"canvas_count\":3") != std::string::npos);
    CHECK(task.state() == TaskState::Accepted);
    CHECK(document.width() == 8 && document.height() == 8);

    auto program = tools.call("pixelforge_pack",
        "{\"action\":\"program\",\"task_id\":" + id_text +
        ",\"program\":\"CANVAS walk_00\\nR 1 1 3 3 #FFFF0000;CANVAS walk_01\\nR 2 1 3 3 #FF00FF00;CANVAS walk_02\\nR 3 1 3 3 #FF0000FF\",\"render_scale\":2}");
    CHECK(program.success);
    CHECK(program.text.find("\"touched_canvases\":3") != std::string::npos);
    CHECK(!program.image_base64.empty() && program.image_mime == "image/png");
    CHECK(document.pixel(1, 1) == 0xFFFF0000u);

    auto sheet = tools.call("pixelforge_pack",
        "{\"action\":\"view\",\"task_id\":" + id_text + ",\"mode\":\"sheet\",\"group\":\"walk\",\"scale\":3}");
    CHECK(sheet.success && !sheet.image_base64.empty() && sheet.image_mime == "image/png");
    CHECK(sheet.text.find("\"canvas_count\":3") != std::string::npos);

    auto timeline = tools.call("pixelforge_pack",
        "{\"action\":\"view\",\"task_id\":" + id_text + ",\"mode\":\"timeline\",\"canvases\":\"walk_00|walk_01|walk_02\",\"scale\":4}");
    CHECK(timeline.success && !timeline.image_base64.empty() && timeline.image_mime == "image/png");
    CHECK(timeline.text.find("\"temporal_review\":true") != std::string::npos);

    auto animation = tools.call("pixelforge_pack",
        "{\"action\":\"view\",\"task_id\":" + id_text + ",\"mode\":\"animation\",\"group\":\"walk\",\"scale\":2,\"fps\":8}");
    CHECK(animation.success && !animation.image_base64.empty() && animation.image_mime == "image/png");
    CHECK(animation.text.find("\"agent_observation\":\"ordered_frame_strip\"") != std::string::npos);

    auto inspected = tools.call("pixelforge_pack",
        "{\"action\":\"inspect\",\"task_id\":" + id_text + ",\"canvas\":\"walk_01\",\"x\":0,\"y\":0,\"width\":8,\"height\":8}");
    CHECK(inspected.success && inspected.text.find("\"canvas\":\"walk_01\"") != std::string::npos);

    auto undone = tools.call("pixelforge_pack",
        "{\"action\":\"history\",\"task_id\":" + id_text + ",\"direction\":\"undo\"}");
    CHECK(undone.success);
    CHECK(document.pixel(1, 1) == 0x00000000u);
    auto redone = tools.call("pixelforge_pack",
        "{\"action\":\"history\",\"task_id\":" + id_text + ",\"direction\":\"redo\"}");
    CHECK(redone.success);
    CHECK(document.pixel(1, 1) == 0xFFFF0000u);

    const auto autosave_root = output_dir.parent_path() / "projects" / ("task_" + id_text);
    CHECK(std::filesystem::exists(autosave_root / "project.json"));
    CHECK(std::filesystem::exists(autosave_root / "canvases" / "walk" / "walk_00.png"));
    CHECK(std::filesystem::exists(autosave_root / "previews" / "walk_strip.png"));

    std::error_code ec;
    std::filesystem::remove_all(output_dir / "frames", ec);
    const auto export_path = (output_dir / "frames").generic_string();
    auto exported = tools.call("pixelforge_pack",
        "{\"action\":\"export\",\"task_id\":" + id_text + ",\"scope\":\"all\",\"path\":\"" + export_path + "\"}");
    CHECK(exported.success);
    CHECK(std::filesystem::exists(output_dir / "frames" / "walk_00.png"));
    CHECK(std::filesystem::exists(output_dir / "frames" / "walk_01.png"));
    CHECK(std::filesystem::exists(output_dir / "frames" / "walk_02.png"));

    const auto gif_path = output_dir / "walk.gif";
    std::filesystem::remove(gif_path, ec);
    auto gif_exported = tools.call("pixelforge_pack",
        "{\"action\":\"export\",\"task_id\":" + id_text +
        ",\"scope\":\"animation\",\"group\":\"walk\",\"path\":\"" + gif_path.generic_string() +
        "\",\"scale\":2,\"fps\":8}");
    CHECK(gif_exported.success);
    CHECK(std::filesystem::exists(gif_path));
    std::ifstream gif_file(gif_path, std::ios::binary);
    CHECK(gif_file.good());
    std::vector<unsigned char> gif_bytes((std::istreambuf_iterator<char>(gif_file)), std::istreambuf_iterator<char>());
    CHECK(gif_bytes.size() > 20);
    std::size_t gce_count = 0;
    for (std::size_t i = 0; i + 4 < gif_bytes.size(); ++i) {
        if (gif_bytes[i] == 0x21 && gif_bytes[i + 1] == 0xF9 && gif_bytes[i + 2] == 0x04) {
            ++gce_count;
            CHECK(gif_bytes[i + 3] == 0x09); // disposal=2 + transparent-index flag
        }
    }
    CHECK(gce_count == 3);

    const auto catalog = pixelforge_dynamic_tools_json();
    CHECK(catalog.find("timeline") != std::string::npos);
    CHECK(catalog.find("dests") != std::string::npos);

    tools.stop();
    DestroyWindow(hwnd);
    std::cout << "Multi-canvas pack tests passed.\n";
    return 0;
}