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

    // LIST must never create a hidden fallback pack.
    auto before_create = tools.call("pixelforge_pack",
        "{\"action\":\"list\",\"task_id\":" + id_text + "}");
    CHECK(!before_create.success);
    CHECK(before_create.text.find("\"error\":\"pack_not_created\"") != std::string::npos);
    CHECK(before_create.text.find("LIST is read-only") != std::string::npos);

    auto created = tools.call("pixelforge_pack",
        "{\"action\":\"create\",\"task_id\":" + id_text +
        ",\"canvases\":\"walk_00,walk,8,8,0|walk_01,walk,8,8,1|walk_02,walk,8,8,2\"}");
    CHECK(created.success);
    CHECK(created.text.find("\"canvas_count\":3") != std::string::npos);
    CHECK(task.state() == TaskState::Accepted);
    CHECK(document.width() == 8 && document.height() == 8);

    // Astra has full control: CREATE may intentionally replace an existing pack,
    // including changing the primary dimensions, without a host-side opt-in flag.
    auto replaced = tools.call("pixelforge_pack",
        "{\"action\":\"create\",\"task_id\":" + id_text +
        ",\"canvases\":\"replacement_00,rebuild,6,5,0|replacement_01,rebuild,6,5,1\"}");
    CHECK(replaced.success);
    CHECK(document.width() == 6 && document.height() == 5);
    auto list_replaced = tools.call("pixelforge_pack",
        "{\"action\":\"list\",\"task_id\":" + id_text + "}");
    CHECK(list_replaced.success);
    CHECK(list_replaced.text.find("\"canvas_count\":2") != std::string::npos);
    CHECK(list_replaced.text.find("replacement_00") != std::string::npos);

    // One model/tool interaction can now rebuild the whole animation, inspect it,
    // author every frame, inspect again, and return the final temporal review.
    auto compound = tools.call("pixelforge_pass",
        "{\"task_id\":" + id_text +
        ",\"create_canvases\":\"walk_00,walk,8,8,0|walk_01,walk,8,8,1|walk_02,walk,8,8,2\""
        ",\"inspect_before\":\"walk_00,0,0,8,8|walk_01,0,0,8,8\""
        ",\"program\":\"CANVAS walk_00\\nP 0 0 #FF101010;CANVAS walk_01\\nP 0 0 #FF202020;CANVAS walk_02\\nP 0 0 #FF303030\""
        ",\"inspect_after\":\"walk_00,0,0,8,8|walk_01,0,0,8,8\""
        ",\"render_mode\":\"animation\",\"render_group\":\"walk\",\"scale\":2,\"fps\":8}");
    CHECK(compound.success);
    CHECK(compound.text.find("\"compound_pass\":true") != std::string::npos);
    CHECK(compound.text.find("\"pack_created\":true") != std::string::npos);
    CHECK(compound.text.find("\"program_applied\":true") != std::string::npos);
    CHECK(compound.text.find("\"touched_canvases\":3") != std::string::npos);
    CHECK(compound.text.find("\"inspect_before_count\":2") != std::string::npos);
    CHECK(compound.text.find("\"inspect_after_count\":2") != std::string::npos);
    CHECK(compound.text.find("\"render_mode\":\"animation\"") != std::string::npos);
    CHECK(!compound.image_base64.empty() && compound.image_mime == "image/png");
    CHECK(document.width() == 8 && document.height() == 8);
    CHECK(document.pixel(0, 0) == 0xFF101010u);

    auto task_info = tools.call("pixelforge_task", "{\"action\":\"get\"}");
    CHECK(task_info.success);
    CHECK(task_info.text.find("\"pack_exists\":true") != std::string::npos);
    CHECK(task_info.text.find("\"pack_canvas_count\":3") != std::string::npos);
    CHECK(task_info.text.find("\"animation_groups\":\"walk:3\"") != std::string::npos);
    CHECK(task_info.text.find("\"agent_has_full_workspace_control\":true") != std::string::npos);
    CHECK(task_info.text.find("\"pack_create_requires_replace_existing\":false") != std::string::npos);

    auto unknown = tools.call("pixelforge_pack",
        "{\"action\":\"view\",\"task_id\":" + id_text + ",\"mode\":\"canvas\",\"canvas\":\"does_not_exist\"}");
    CHECK(!unknown.success);
    CHECK(unknown.text.find("pixelforge_pack list") != std::string::npos);

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
    CHECK(inspected.text.find("\"clipped\":false") != std::string::npos);

    // Oversized/out-of-bounds inspection is best-effort: clip and succeed instead
    // of consuming the model's consecutive-failure budget.
    auto clipped_inspect = tools.call("pixelforge_pack",
        "{\"action\":\"inspect\",\"task_id\":" + id_text + ",\"canvas\":\"walk_01\",\"x\":7,\"y\":7,\"width\":800,\"height\":800}");
    CHECK(clipped_inspect.success);
    CHECK(clipped_inspect.text.find("\"clipped\":true") != std::string::npos);
    CHECK(clipped_inspect.text.find("\"x\":7") != std::string::npos);
    CHECK(clipped_inspect.text.find("\"y\":7") != std::string::npos);
    CHECK(clipped_inspect.text.find("\"width\":1") != std::string::npos);
    CHECK(clipped_inspect.text.find("\"height\":1") != std::string::npos);
    CHECK(clipped_inspect.text.find("max_area") == std::string::npos);

    // COPY uses the same best-effort clipping semantics.
    auto clipped_copy = tools.call("pixelforge_pack",
        "{\"action\":\"copy\",\"task_id\":" + id_text +
        ",\"source\":\"walk_00\",\"dest\":\"walk_01\",\"sx\":0,\"sy\":0,\"width\":80,\"height\":80,\"dx\":0,\"dy\":0}");
    CHECK(clipped_copy.success);
    CHECK(clipped_copy.text.find("\"clipped\":true") != std::string::npos);
    CHECK(clipped_copy.text.find("\"width\":8") != std::string::npos);
    CHECK(clipped_copy.text.find("\"height\":8") != std::string::npos);

    auto undone = tools.call("pixelforge_pack",
        "{\"action\":\"history\",\"task_id\":" + id_text + ",\"direction\":\"undo\"}");
    CHECK(undone.success);
    // Undo the clipped copy first, then the later broad program pass. The compound
    // initial pass remains underneath it as the earlier animation construction.
    auto undone_program = tools.call("pixelforge_pack",
        "{\"action\":\"history\",\"task_id\":" + id_text + ",\"direction\":\"undo\"}");
    CHECK(undone_program.success);
    CHECK(document.pixel(1, 1) == 0x00000000u);
    CHECK(document.pixel(0, 0) == 0xFF101010u);
    auto redone_program = tools.call("pixelforge_pack",
        "{\"action\":\"history\",\"task_id\":" + id_text + ",\"direction\":\"redo\"}");
    CHECK(redone_program.success);
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
            CHECK(gif_bytes[i + 3] == 0x09);
        }
    }
    CHECK(gce_count == 3);

    const auto catalog = pixelforge_dynamic_tools_json();
    CHECK(catalog.find("timeline") != std::string::npos);
    CHECK(catalog.find("dests") != std::string::npos);
    CHECK(catalog.find("pixelforge_pass") != std::string::npos);
    CHECK(catalog.find("create_canvases") != std::string::npos);
    CHECK(catalog.find("inspect_before") != std::string::npos);
    CHECK(catalog.find("Astra may use it on an existing project") != std::string::npos);
    CHECK(catalog.find("permitted to replace an existing pack") != std::string::npos);

    tools.stop();
    DestroyWindow(hwnd);
    std::cout << "Multi-canvas pack tests passed.\n";
    return 0;
}
