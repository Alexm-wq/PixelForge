#include "AgentTask.hpp"
#include "PixelDocument.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace pixelforge;

#define CHECK(expr) do { if (!(expr)) { std::cerr << "Failed: " #expr " at line " << __LINE__ << '\n'; return 1; } } while (false)

int main() {
    PixelDocument doc;
    AgentTaskController task(doc);
    std::string error;

    ReferenceSlot source;
    source.path = "editable.png";
    source.width = 8;
    source.height = 6;
    source.present = true;
    task.set_source_reference(source);

    const auto first_id = task.begin("Edit the supplied source image.");
    auto snap = task.snapshot();
    CHECK(snap.id == first_id);
    CHECK(snap.source_reference.present && snap.source_reference.path == "editable.png");
    CHECK(!snap.continuation_pending);

    CHECK(task.request_user_input("The requested operation has two materially different meanings.",
                                  "Which interpretation should I use?",
                                  "Keep silhouette|Change silhouette", &error));
    CHECK(task.awaiting_user_input());
    CHECK(!task.accept(8, 6, &error));
    CHECK(task.user_answer_input("Keep silhouette", &error));
    CHECK(!task.awaiting_user_input());
    CHECK(task.accept(8, 6, &error));

    CHECK(task.request_user_input("A hard constraint conflicts with the requested edit.",
                                  "Should I preserve the native source size?", "Preserve size|Cancel", &error));
    CHECK(!task.finish("should fail", &error));
    CHECK(task.user_answer_input("Preserve size", &error));
    CHECK(task.finish("done", &error));
    CHECK(task.awaiting_user_review());
    CHECK(task.user_request_changes("Make the eye smaller.", &error));

    const auto second_id = task.begin("Continue with review feedback.");
    snap = task.snapshot();
    CHECK(second_id != first_id);
    CHECK(snap.continuation_pending);
    CHECK(task.accept(8, 6, &error));
    CHECK(task.snapshot().source_reference.present);

    std::cout << "Agent-initiated conversation and Source task state passed.\n";
    return 0;
}
