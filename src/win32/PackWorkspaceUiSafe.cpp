// Compile the existing workspace implementation with its public publisher renamed,
// then provide a thread-safe publisher below. This keeps all Win32 window work on
// the UI thread while pack snapshots may be produced by the Codex worker thread.
#define pack_workspace_ui_publish pack_workspace_ui_publish_unsafe
#include "PackWorkspaceUi.cpp"
#undef pack_workspace_ui_publish

namespace pixelforge::win32 {

void pack_workspace_ui_publish(HWND owner,
                               std::uint64_t task_id,
                               std::uint64_t revision,
                               std::wstring project_directory,
                               std::vector<PackUiCanvasInfo> canvases) {
    HWND tabs = nullptr;
    {
        std::lock_guard lock(g_ui_mutex);
        if (owner && !g_owner && is_main_pixelforge_window(owner)) {
            // The automatic attach hook normally owns this assignment. This
            // fallback only records the owner; it deliberately creates/shows
            // no HWND from the worker thread.
            g_owner = owner;
        }

        const bool new_task = g_ui.task_id != 0 && task_id != g_ui.task_id;
        const std::string previous_canvas = new_task ? std::string{} : g_ui.selected_canvas;
        const std::string previous_group = new_task ? std::string{} : g_ui.selected_group;
        g_ui.task_id = task_id;
        g_ui.revision = revision;
        g_ui.project_directory = std::move(project_directory);
        g_ui.canvases = std::move(canvases);

        if (!previous_canvas.empty() && find_canvas(g_ui, previous_canvas))
            g_ui.selected_canvas = previous_canvas;
        else if (g_ui.tab == TAB_CANVAS)
            g_ui.selected_canvas.clear();
        else
            g_ui.selected_canvas = g_ui.canvases.empty() ? std::string{} : g_ui.canvases.front().name;

        const auto groups = animation_groups(g_ui);
        if (!previous_group.empty() && std::find(groups.begin(), groups.end(), previous_group) != groups.end())
            g_ui.selected_group = previous_group;
        else
            g_ui.selected_group = groups.empty() ? std::string{} : groups.front();

        tabs = g_tabs_window;
    }

    // Never call ShowWindow/BringWindowToTop while holding g_ui_mutex from the
    // Codex worker thread. A synthetic click outside all tab hit rects causes
    // the existing tab wndproc to apply visibility and repaint asynchronously
    // on the owning UI thread without changing the selected tab.
    if (tabs && IsWindow(tabs))
        PostMessageW(tabs, WM_LBUTTONDOWN, 0, MAKELPARAM(-100, -100));
}

} // namespace pixelforge::win32
