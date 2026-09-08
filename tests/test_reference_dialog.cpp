#include "AgentInteractionUi.hpp"
#include "LocalAgentToolSession.hpp"
#include "MiniJson.hpp"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

using namespace pixelforge;
using namespace pixelforge::win32;

#define CHECK(expr) do { if (!(expr)) { std::cerr << "Failed: " #expr " at line " << __LINE__ << '\n'; return 1; } } while (false)

namespace {

ImageData two_color_image(std::uint32_t left_argb, std::uint32_t right_argb) {
    ImageData out;
    out.width = 2;
    out.height = 2;
    out.bgra.resize(16);
    auto write = [&](int i, std::uint32_t argb) {
        out.bgra[static_cast<std::size_t>(i) * 4u + 0] = static_cast<std::uint8_t>(argb & 0xffu);
        out.bgra[static_cast<std::size_t>(i) * 4u + 1] = static_cast<std::uint8_t>((argb >> 8) & 0xffu);
        out.bgra[static_cast<std::size_t>(i) * 4u + 2] = static_cast<std::uint8_t>((argb >> 16) & 0xffu);
        out.bgra[static_cast<std::size_t>(i) * 4u + 3] = static_cast<std::uint8_t>((argb >> 24) & 0xffu);
    };
    write(0, left_argb); write(1, left_argb); write(2, left_argb); write(3, right_argb);
    return out;
}

struct Harness {
    PixelDocument document;
    AgentTaskController task{document};
    ImageData content;
    ImageData style;
    std::wstring content_path;
    std::wstring style_path;
    std::mutex mutex;
    SessionRecorder recorder;
    LocalAgentToolSession tools;
    HWND hwnd = nullptr;

    bool start() {
        hwnd = CreateWindowW(L"STATIC", L"interaction-test", 0, 0, 0, 1, 1,
                             HWND_MESSAGE, nullptr, nullptr, nullptr);
        if (!hwnd) return false;
        AgentMcpBindings bindings;
        bindings.document = &document;
        bindings.task = &task;
        bindings.content_reference = &content;
        bindings.style_reference = &style;
        bindings.content_path = &content_path;
        bindings.style_path = &style_path;
        bindings.state_mutex = &mutex;
        bindings.hwnd = hwnd;
        LocalRecordBindings recording;
        recording.recorder = &recorder;
        recording.task = &task;
        recording.state_mutex = &mutex;
        recording.hwnd = hwnd;
        recording.recording_directory = L"build/test-output";
        std::wstring error;
        return tools.start(bindings, recording, error);
    }

    ~Harness() {
        tools.stop();
        if (hwnd) DestroyWindow(hwnd);
    }
};

} // namespace

int main() {
    {
        Harness h;
        CHECK(h.start());
        const auto source = two_color_image(0xff112233u, 0xffaabbccu);
        agent_interaction_set_source_for_testing(source, L"editable-source.png");
        const auto id = h.task.begin("Edit the Source image directly.");

        auto get = h.tools.call("pixelforge_task", "{\"action\":\"get\"}");
        CHECK(get.success && get.text.find("\"source_present\":true") != std::string::npos);

        auto accept = h.tools.call("pixelforge_task",
            "{\"action\":\"accept\",\"task_id\":" + std::to_string(id) + "}");
        CHECK(accept.success);
        CHECK(accept.text.find("\"seeded_from\":\"source\"") != std::string::npos);
        CHECK(accept.text.find("\"canvas_blank\":false") != std::string::npos);
        CHECK(h.document.width() == 2 && h.document.height() == 2);
        CHECK(h.document.pixel(0, 0) == 0xff112233u);
        CHECK(h.document.pixel(1, 1) == 0xffaabbccu);

        auto source_view = h.tools.call("pixelforge_reference",
            "{\"action\":\"view\",\"task_id\":" + std::to_string(id) +
            ",\"reference\":\"source\"}");
        CHECK(source_view.success);
        CHECK(source_view.text.find("\"reference\":\"source\"") != std::string::npos);

        auto render = h.tools.call("pixelforge_view",
            "{\"action\":\"render\",\"task_id\":" + std::to_string(id) + ",\"scale\":4}");
        CHECK(render.success && !render.image_base64.empty());

        auto palette = h.tools.call("pixelforge_reference",
            "{\"action\":\"palette\",\"task_id\":" + std::to_string(id) +
            ",\"reference\":\"source\",\"max_colors\":2}");
        CHECK(palette.success);
        CHECK(palette.text.find("FF112233") != std::string::npos);
        CHECK(palette.text.find("FFAABBCC") != std::string::npos);

        h.content = two_color_image(0xff010203u, 0xffd0e0f0u);
        h.content_path = L"content.png";
        ReferenceSlot content_slot{"content.png", 2, 2, true};
        h.task.set_content_reference(content_slot);
        auto seed = h.tools.call("pixelforge_reference",
            "{\"action\":\"seed\",\"task_id\":" + std::to_string(id) +
            ",\"reference\":\"content\"}");
        CHECK(seed.success);
        CHECK(h.document.pixel(0, 0) == 0xff010203u);
        CHECK(h.document.pixel(1, 1) == 0xffd0e0f0u);

        SetEnvironmentVariableW(L"PIXELFORGE_DIALOG_TEST_ANSWER", L"Preserve the source dimensions");
        auto dialog = h.tools.call("pixelforge_dialog",
            "{\"task_id\":" + std::to_string(id) +
            ",\"reason\":\"The requested resize would change the editable Source dimensions.\""
            ",\"question\":\"How should I proceed?\""
            ",\"suggestions\":\"Preserve the source dimensions|Resize and crop\"}");
        SetEnvironmentVariableW(L"PIXELFORGE_DIALOG_TEST_ANSWER", nullptr);
        CHECK(dialog.success);
        CHECK(dialog.text.find("Preserve the source dimensions") != std::string::npos);
        CHECK(!h.task.awaiting_user_input());
    }

    // Clearing Source after a completed session must not touch destroyed task state.
    agent_interaction_set_source_for_testing({}, {});
    {
        Harness h;
        CHECK(h.start());
        const auto id = h.task.begin("Create a fresh blank sprite.");
        auto accept = h.tools.call("pixelforge_task",
            "{\"action\":\"accept\",\"task_id\":" + std::to_string(id) +
            ",\"width\":4,\"height\":4}");
        CHECK(accept.success);
        CHECK(accept.text.find("\"canvas_blank\":true") != std::string::npos);
    }

    const auto catalog = pixelforge_dynamic_tools_json();
    CHECK(catalog.find("pixelforge_reference") != std::string::npos);
    CHECK(catalog.find("pixelforge_dialog") != std::string::npos);
    CHECK(catalog.find("YOU decide whether a pause is warranted") != std::string::npos);

    std::cout << "Reference seeding, palette extraction, blank signaling and agent dialog passed.\n";
    return 0;
}
