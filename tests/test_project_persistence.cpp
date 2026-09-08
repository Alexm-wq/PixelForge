// Exercise the exact acceptance persistence boundary without opening the editor.
#include "AutomaticUiRepaintGuard.cpp"

#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace {
void write_fixture(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    CHECK(file.good());
}

std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int main() {
    const auto root = std::filesystem::current_path() / "test-output" /
        ("persistence-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    write_fixture(root / "CMakeLists.txt", "# isolated persistence fixture\n");
    write_fixture(root / "src" / "win32" / "main_auto.cpp", "// fixture\n");
    CHECK(SetEnvironmentVariableW(L"PIXELFORGE_REPO_ROOT", root.c_str()));

    pixelforge::PixelDocument document;
    pixelforge::AgentTaskController task(document);
    const auto task_id = task.begin("Persistence regression");
    CHECK(task.accept(2, 2));
    CHECK(task.finish("PROJECT_NAME: Accepted animation pack\nReviewed."));
    const auto project = root / "projects" / ("task_" + std::to_string(task_id));
    const auto manifest_path = project / "project.json";
    std::string original = "{\"version\":1,\"project_name\":\"Draft\",\"canvases\":[";
    for (int i = 0; i < 120; ++i) {
        const std::string relative = "canvases/walk/frame_" + std::to_string(i) + ".png";
        write_fixture(project / relative, "preserved frame " + std::to_string(i));
        if (i) original += ',';
        original += "{\"name\":\"frame_" + std::to_string(i) + "\",\"file\":\"" + relative + "\"}";
    }
    original += "]}";
    write_fixture(manifest_path, original);
    std::wstring saved, error;

    // Before the fix this fails with Windows error 5: the read stream for
    // project.json remains alive while the replacement is attempted.
    CHECK(persist_reviewed_project(saved, error));
    CHECK(saved == project.wstring());
    CHECK(pixelforge::win32::read_project_brief(project) == task.snapshot().review_summary);
    CHECK(pixelforge::win32::compact_project_brief(std::string(9000, 'x')).size() == 4096);
    const auto accepted_manifest = read_fixture(manifest_path);
    CHECK(accepted_manifest.find("Accepted animation pack") != std::string::npos);
    CHECK(manifest_canvas_count(accepted_manifest) == 120);
    for (int i = 0; i < 120; ++i)
        CHECK(read_fixture(project / "canvases" / "walk" / ("frame_" + std::to_string(i) + ".png")) ==
              "preserved frame " + std::to_string(i));

    // A real external file lock must fail without truncating the saved project
    // or accepting the review. Closing that lock must make retry succeed.
    HANDLE locked = CreateFileW(manifest_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    CHECK(!persist_reviewed_project(saved, error));
    CHECK(error.find(L"Windows error") != std::wstring::npos);
    CHECK(saved.empty());
    CHECK(task.awaiting_user_review());
    CHECK(read_fixture(manifest_path) == accepted_manifest);
    CHECK(CloseHandle(locked));
    CHECK(persist_reviewed_project(saved, error));
    CHECK(read_fixture(manifest_path) == accepted_manifest);
    CHECK(task.user_accept_review());

    std::cout << "Project acceptance persistence tests passed.\n";
}
