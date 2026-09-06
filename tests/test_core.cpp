#include "AgentTask.hpp"
#include "PixelDocument.hpp"

#include <cassert>
#include <iostream>

using namespace pixelforge;

int main() {
    PixelDocument doc;
    assert(doc.width() == 0);

    std::string error;
    assert(!doc.can_resize(0, 64, &error));
    assert(!doc.can_resize(4096, 4096, &error));
    assert(doc.resize(64, 64, &error));
    const auto after_resize = doc.revision();

    {
        auto tx = doc.begin_transaction();
        assert(tx.set_pixel(1, 1, 0xff112233u));
        assert(tx.set_pixel(2, 1, 0xff445566u));
        assert(tx.set_pixel(1, 1, 0xff778899u));
        assert(tx.commit());
    }
    assert(doc.revision() == after_resize + 1);
    assert(doc.pixel(1, 1) == 0xff778899u);
    assert(doc.pixel(2, 1) == 0xff445566u);

    assert(doc.undo());
    assert(doc.pixel(1, 1) == 0x00000000u);
    assert(doc.redo());
    assert(doc.pixel(1, 1) == 0xff778899u);

    PixelDocument task_doc;
    AgentTaskController task(task_doc);
    task.begin("Draw a 32x40 infected creature in pixel art.");
    assert(task.state() == TaskState::AwaitingAgentDecision);
    assert(task.accept(32, 40, &error));
    assert(task.state() == TaskState::Accepted);
    assert(task_doc.width() == 32 && task_doc.height() == 40);
    assert(task.finish("done", &error));

    PixelDocument reject_doc;
    AgentTaskController rejected(reject_doc);
    rejected.begin("Make a non-pixel oil painting.");
    // The core deliberately does not infer semantic scope. The AGENT chooses this call.
    assert(rejected.reject("PixelForge only handles pixel-art output.", &error));
    assert(rejected.state() == TaskState::Rejected);
    assert(reject_doc.width() == 0);

    std::cout << "PixelForge core tests passed\n";
    return 0;
}
