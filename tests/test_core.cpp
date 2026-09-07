#include "AgentTask.hpp"
#include "PixelDocument.hpp"

#include "AgentCommands.hpp"
#include "MiniJson.hpp"
#include <cstdlib>
#include <chrono>
#include <climits>
#include <utility>
#include <iostream>

#define CHECK(expr) do { if (!(expr)) { std::cerr << "Failed: " #expr << " at line " << __LINE__ << '\n'; std::exit(1); } } while (false)

using namespace pixelforge;

int main() {
    PixelDocument doc;
    CHECK(doc.width() == 0);

    std::string error;
    CHECK(!doc.can_resize(0, 64, &error));
    CHECK(!doc.can_resize(4096, 4096, &error));
    CHECK(doc.resize(64, 64, &error));
    const auto after_resize = doc.revision();

    {
        auto tx = doc.begin_transaction();
        CHECK(tx.set_pixel(1, 1, 0xff112233u));
        CHECK(tx.set_pixel(2, 1, 0xff445566u));
        CHECK(tx.set_pixel(1, 1, 0xff778899u));
        CHECK(tx.commit());
    }
    CHECK(doc.revision() == after_resize + 1);
    CHECK(doc.pixel(1, 1) == 0xff778899u);
    CHECK(doc.pixel(2, 1) == 0xff445566u);

    CHECK(doc.undo());
    CHECK(doc.pixel(1, 1) == 0x00000000u);
    CHECK(doc.redo());
    CHECK(doc.pixel(1, 1) == 0xff778899u);

    PixelDocument task_doc;
    AgentTaskController task(task_doc);
    task.begin("Draw a 32x40 infected creature in pixel art.");
    CHECK(task.state() == TaskState::AwaitingAgentDecision);
    CHECK(task.accept(32, 40, &error));
    CHECK(task.state() == TaskState::Accepted);
    CHECK(task_doc.width() == 32 && task_doc.height() == 40);
    CHECK(task.finish("done", &error));

    PixelDocument reject_doc;
    AgentTaskController rejected(reject_doc);
    rejected.begin("Make a non-pixel oil painting.");
    // The core deliberately does not infer semantic scope. The AGENT chooses this call.
    CHECK(rejected.reject("PixelForge only handles pixel-art output.", &error));
    CHECK(rejected.state() == TaskState::Rejected);
    CHECK(reject_doc.width() == 0);

    PixelDocument routed;
    AgentTaskController lifecycle(routed);
    AgentCommandRouter router(routed, lifecycle);
    const auto id = lifecycle.begin("Regression sprite");
    CHECK(router.task_accept(id, 16, 16).ok);
    AgentPixelOp pixel;
    pixel.x = 1; pixel.y = 1; pixel.argb = 0xff123456;
    auto invalid = pixel; invalid.x = INT_MAX;
    auto rev = routed.revision();
    CHECK(!router.edit(id, rev, {pixel, invalid}).ok);
    CHECK(router.edit(id, rev, {pixel, invalid}).message.find("Operation 2") != std::string::npos);
    CHECK(routed.pixel(1, 1) == 0 && routed.revision() == rev);
    auto reset = pixel; reset.argb = 0;
    auto result = router.edit(id, rev, {pixel, reset});
    CHECK(result.ok && result.changed_pixels == 0 && result.revision == rev);
    result = router.edit(id, rev, {pixel, reset, pixel});
    CHECK(result.ok && result.changed_pixels == 1 && result.revision == rev + 1);
    CHECK(router.edit(id, rev, {pixel}).error == AgentErrorCode::StaleRevision);
    CHECK(router.edit(id + 1, routed.revision(), {pixel}).error == AgentErrorCode::StaleTask);
    auto line = pixel; line.kind = AgentPixelOpKind::Line; line.x2 = INT_MIN;
    CHECK(!router.edit(id, routed.revision(), {line}).ok);
    CHECK(router.history_undo(id, routed.revision()).ok);
    CHECK(routed.pixel(1, 1) == 0);
    CHECK(router.history_redo(id, routed.revision()).ok);
    CHECK(routed.pixel(1, 1) == pixel.argb);
    CHECK(router.task_finish(id, routed.revision(), "done").ok);
    CHECK(router.inspect_region(id, routed.revision(), 0, 0, 16, 16).ok);
    CHECK(!router.edit(id, routed.revision(), {pixel}).ok);

    auto stale = routed.begin_transaction();
    CHECK(stale.set_pixel(0, 0, 1));
    CHECK(routed.resize(2, 2));
    CHECK(!stale.commit());
    CHECK(routed.pixel(0, 0) == 0);
    auto moved = routed.begin_transaction();
    CHECK(moved.set_pixel(0, 0, 2));
    auto destination = std::move(moved);
    CHECK(!moved.commit());
    CHECK(destination.commit());

    FlatJsonObject json; std::string json_error;
    CHECK(parse_flat_json_object("{\"prompt\":\"\\uD83C\\uDFA8\"}", json, json_error));
    CHECK(json.get("prompt").size() == 4);
    CHECK(!parse_flat_json_object("{} garbage", json, json_error));
    CHECK(!parse_flat_json_object("{\"x\":\"raw\nline\"}", json, json_error));
    CHECK(!parse_flat_json_object("{\"x\":\"\\uD800\"}", json, json_error));

    PixelDocument large;
    CHECK(large.resize(2048, 2048));
    const auto started = std::chrono::steady_clock::now();
    auto fill = large.begin_transaction();
    CHECK(fill.fill_rect(0, 0, 2048, 2048, 0xffabcdef));
    CHECK(fill.fill_rect(0, 0, 1024, 1024, 0xff112233));
    CHECK(fill.pending_changes() == 2048u * 2048u);
    CHECK(fill.commit());
    CHECK(large.pixel(0, 0) == 0xff112233 && large.pixel(2047, 2047) == 0xffabcdef);
    CHECK(large.undo() && large.pixel(0, 0) == 0);
    CHECK(large.redo() && large.pixel(0, 0) == 0xff112233);
    std::cout << "2048x2048 overlapping fill + undo/redo: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
              << " ms\nPixelForge core tests passed\n";
    return 0;
}
