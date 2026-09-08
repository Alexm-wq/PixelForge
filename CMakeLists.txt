cmake_minimum_required(VERSION 3.20)
project(PixelForge VERSION 0.4.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pixelforge_core
    src/core/PixelDocument.cpp
    src/core/AgentTask.cpp
    src/core/AgentCommands.cpp
    src/core/MiniJson.cpp
)
target_include_directories(pixelforge_core PUBLIC src/core)

if(WIN32)
    set(PIXELFORGE_MULTI_CANVAS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/MultiCanvasTool.cpp")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

    # PixelProgram is the agent's expansion engine. Do not impose arbitrary limits
    # on the number of masks, masks' aggregate bytes, emitted patch operations,
    # palette indices, mask-name length, or cluster size. The process/allocator is
    # the resource boundary; large valid artistic passes should compile as one pass.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/PixelProgram.cpp" PIXELFORGE_PIXEL_PROGRAM_TEXT)
    string(REPLACE
        "constexpr std::size_t kMaxPatchOperations = 20000;\nconstexpr std::size_t kMaxNamedMasks = 64;\nconstexpr std::size_t kMaxMaskBytes = 64u * 1024u * 1024u;\n"
        ""
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "return parse_int(token, index) && index >= 0 && index <= 255;"
                   "return parse_int(token, index) && index >= 0;"
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "if (name.empty() || name.size() > 64) return false;" "if (name.empty()) return false;"
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
[=[                ++operations;
                if (operations > kMaxPatchOperations) return {};]=]
[=[                ++operations;]=]
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
[=[    bool store_mask(std::string_view name, Mask mask, std::string& error) {
        const auto key = mask_key(name);
        const auto found = masks_.find(key);
        if (found == masks_.end()) {
            if (masks_.size() >= kMaxNamedMasks) {
                error = "Program exceeds the 64 named-mask limit.";
                return false;
            }
            if ((masks_.size() + 1) * mask.size() > kMaxMaskBytes) {
                error = "Named masks would exceed the 64 MiB mask-memory limit; clear/reuse masks or split the pass.";
                return false;
            }
        }
        masks_[key] = std::move(mask);
        return true;
    }]=]
[=[    bool store_mask(std::string_view name, Mask mask, std::string& error) {
        (void)error;
        masks_[mask_key(name)] = std::move(mask);
        return true;
    }]=]
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "color must be palette index 0..255 or #AARRGGBB."
                   "color must be a non-negative palette index or #AARRGGBB."
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE "mask name must be 1..64 letters, digits, '_' or '-'."
                   "mask name must contain letters, digits, '_' or '-'."
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
        "if(size_min <= 0 || size_max < size_min || size_max > 256) { result.error = \"Line \" + std::to_string(line_no) + \": cluster sizes must satisfy 1 <= size_min <= size_max <= 256.\"; return result; }"
        "if(size_min <= 0 || size_max < size_min) { result.error = \"Line \" + std::to_string(line_no) + \": cluster sizes must satisfy 1 <= size_min <= size_max.\"; return result; }"
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    string(REPLACE
[=[    if (result.patch.empty()) {
        result.error = "Program expands beyond the 20,000-operation atomic patch limit; split it into two artistic passes.";
        return result;
    }]=]
[=[]=]
        PIXELFORGE_PIXEL_PROGRAM_TEXT "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")
    set(PIXELFORGE_PIXEL_PROGRAM_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/PixelProgram.cpp")
    file(WRITE "${PIXELFORGE_PIXEL_PROGRAM_SOURCE}" "${PIXELFORGE_PIXEL_PROGRAM_TEXT}")

    # Remove equivalent arbitrary ceilings from the single-canvas MCP path too.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/AgentMcpServer.cpp" PIXELFORGE_AGENT_MCP_TEXT)
    string(REPLACE
        "constexpr std::uint64_t kMaxObservationPixels = 16ull * 1024ull * 1024ull;\nconstexpr std::size_t kMaxPatchOperations = 20000;\nconstexpr std::size_t kMaxInspectPixels = 4096;\n"
        ""
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE "        if (line.size() > 8u * 1024u * 1024u) return false;\n" ""
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE
[=[        operations.push_back(op);
        if (operations.size() > kMaxPatchOperations) {
            error = "Patch exceeds the 20,000-operation transport limit; split it into multiple revisioned batches.";
            return false;
        }]=]
[=[        operations.push_back(op);]=]
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE
[=[        parsed.push_back(c);
        if (parsed.size() > 256) {
            error = "Palette is limited to 256 entries.";
            return false;
        }]=]
[=[        parsed.push_back(c);]=]
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE "Batch aggressively: one call may contain up to 20,000 operations."
                   "Batch aggressively: one call may contain as many valid operations as the artistic pass requires."
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE
[=[            if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaxInspectPixels)
                return {true, "{\"ok\":false,\"error\":\"inspect_too_large\",\"message\":\"Use render for regions over 4096 pixels.\"}"};]=]
[=[]=]
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE "if (scale < 1 || scale > 32 || x < 0 || y < 0 || w <= 0 || h <= 0 ||"
                   "if (scale < 1 || x < 0 || y < 0 || w <= 0 || h <= 0 ||"
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    string(REPLACE
[=[                const std::uint64_t out_pixels = static_cast<std::uint64_t>(w) * h * scale * scale;
                if (out_pixels > kMaxObservationPixels)
                    return {true, "{\"ok\":false,\"error\":\"render_too_large\",\"message\":\"Crop the render or use a smaller integer scale.\"}"};]=]
[=[]=]
        PIXELFORGE_AGENT_MCP_TEXT "${PIXELFORGE_AGENT_MCP_TEXT}")
    set(PIXELFORGE_AGENT_MCP_IMPL "${CMAKE_CURRENT_BINARY_DIR}/generated/AgentMcpServer.cpp")
    file(WRITE "${PIXELFORGE_AGENT_MCP_IMPL}" "${PIXELFORGE_AGENT_MCP_TEXT}")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/AgentMcpServerFramed.cpp" PIXELFORGE_AGENT_MCP_FRAMED_TEXT)
    string(REPLACE "#include \"AgentMcpServer.cpp\"" "#include \"AgentMcpServer.cpp\""
        PIXELFORGE_AGENT_MCP_FRAMED_TEXT "${PIXELFORGE_AGENT_MCP_FRAMED_TEXT}")
    set(PIXELFORGE_AGENT_MCP_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/AgentMcpServerFramed.cpp")
    file(WRITE "${PIXELFORGE_AGENT_MCP_SOURCE}" "${PIXELFORGE_AGENT_MCP_FRAMED_TEXT}")

    set(PIXELFORGE_WORKSPACE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/LocalAgentToolSessionWorkspace.cpp")

    # Treat secondary pack canvases as real Canvas-tab pages. Single click in the
    # grid selects; double click switches to Canvas and opens that exact canvas.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/PackWorkspaceUiTabs.cpp" PIXELFORGE_PACK_UI_TEXT)
    string(REPLACE
[=[void apply_page_visibility_locked() {
    if (g_content_window)
        ShowWindow(g_content_window, g_ui.tab == TAB_CANVAS ? SW_HIDE : SW_SHOW);
    if (g_tabs_window) BringWindowToTop(g_tabs_window);
}]=]
[=[bool selected_secondary_canvas_locked() {
    return g_ui.tab == TAB_CANVAS && !g_ui.selected_canvas.empty() &&
           !g_ui.canvases.empty() && g_ui.selected_canvas != g_ui.canvases.front().name;
}

bool content_page_visible_locked() {
    return g_ui.tab != TAB_CANVAS || selected_secondary_canvas_locked();
}

void apply_page_visibility_locked() {
    if (g_content_window)
        ShowWindow(g_content_window, content_page_visible_locked() ? SW_SHOW : SW_HIDE);
    if (g_tabs_window) BringWindowToTop(g_tabs_window);
}]=]
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    string(REPLACE
[=[void draw_canvases(HDC dc, const RECT& client, UiState& state) {]=]
[=[void draw_selected_canvas(HDC dc, const RECT& client, UiState& state) {
    if (state.selected_canvas.empty()) return;
    const auto canvas_it = std::find_if(state.canvases.begin(), state.canvases.end(), [&](const auto& canvas) {
        return canvas.name == state.selected_canvas;
    });
    if (canvas_it == state.canvases.end()) return;
    const auto presentation_it = state.presentations.find(state.selected_canvas);
    if (presentation_it == state.presentations.end() || !presentation_it->second.authoritative.valid()) return;
    const auto& presentation = presentation_it->second;
    const auto& pixels = presentation.playback.pixels_or(
        presentation.authoritative.pixels,
        presentation.authoritative.width,
        presentation.authoritative.height);
    text_ui(dc, 18, 18, L"Canvas: " + utf8_to_wide_ui(state.selected_canvas), RGB(111, 220, 215));
    RECT area{18, 48, client.right - 18, client.bottom - 34};
    draw_pixels_fit(dc, area, presentation.authoritative.width, presentation.authoritative.height, pixels);
    if (presentation.playback.animating())
        text_ui(dc, 18, static_cast<int>(client.bottom) - 26, L"drawing...", RGB(111, 220, 215));
}

void draw_canvases(HDC dc, const RECT& client, UiState& state) {]=]
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    string(REPLACE
[=[        if (g_ui.tab == TAB_CANVASES) draw_canvases(target, client, g_ui);
        else if (g_ui.tab == TAB_ANIMATION) draw_animation(target, client, g_ui);]=]
[=[        if (g_ui.tab == TAB_CANVAS) draw_selected_canvas(target, client, g_ui);
        else if (g_ui.tab == TAB_CANVASES) draw_canvases(target, client, g_ui);
        else if (g_ui.tab == TAB_ANIMATION) draw_animation(target, client, g_ui);]=]
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    string(REPLACE "if (g_content_window && g_ui.tab != TAB_CANVAS) InvalidateRect(g_content_window, nullptr, FALSE);"
                   "if (g_content_window && content_page_visible_locked()) InvalidateRect(g_content_window, nullptr, FALSE);"
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    string(REPLACE "if (g_ui.tab == TAB_CANVASES && reveal_changed) repaint_content = true;"
                   "if ((g_ui.tab == TAB_CANVASES || g_ui.tab == TAB_CANVAS) && reveal_changed) repaint_content = true;"
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    string(REPLACE
[=[        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};]=]
[=[        case WM_LBUTTONDBLCLK: {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            HWND owner = nullptr;
            bool opened = false;
            bool primary = false;
            {
                std::lock_guard lock(g_ui_mutex);
                if (g_ui.tab == TAB_CANVASES) {
                    for (const auto& hit : g_canvas_hits) {
                        if (!PtInRect(&hit.first, p)) continue;
                        g_ui.selected_canvas = hit.second;
                        g_ui.tab = TAB_CANVAS;
                        primary = !g_ui.canvases.empty() && g_ui.selected_canvas == g_ui.canvases.front().name;
                        owner = g_owner;
                        apply_page_visibility_locked();
                        opened = true;
                        break;
                    }
                }
            }
            if (opened) {
                if (g_tabs_window) InvalidateRect(g_tabs_window, nullptr, FALSE);
                if (g_content_window && IsWindowVisible(g_content_window)) InvalidateRect(g_content_window, nullptr, FALSE);
                if (primary && owner) {
                    const RECT region = parent_canvas_region(owner);
                    InvalidateRect(owner, &region, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};]=]
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    string(REPLACE "    content.lpfnWndProc = content_wndproc;\n"
                   "    content.style = CS_DBLCLKS;\n    content.lpfnWndProc = content_wndproc;\n"
        PIXELFORGE_PACK_UI_TEXT "${PIXELFORGE_PACK_UI_TEXT}")
    set(PIXELFORGE_PACK_UI_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/PackWorkspaceUiTabs.cpp")
    file(WRITE "${PIXELFORGE_PACK_UI_SOURCE}" "${PIXELFORGE_PACK_UI_TEXT}")

    add_executable(PixelForge WIN32
        src/win32/main_auto.cpp
        src/win32/ImageIO.cpp
        ${PIXELFORGE_AGENT_MCP_SOURCE}
        src/win32/ReferenceResponseLimiter.cpp
        src/win32/PipeBridge.cpp
        src/win32/CodexAppClientDynamic.cpp
        src/win32/CodexSessionTrace.cpp
        src/win32/AgentInteractionUi.cpp
        src/win32/AutomaticUiRepaintGuard.cpp
        ${PIXELFORGE_WORKSPACE_SOURCE}
        src/win32/ProjectRestoreSession.cpp
        ${PIXELFORGE_MULTI_CANVAS_SOURCE}
        ${PIXELFORGE_PACK_UI_SOURCE}
        src/win32/ProjectLoader.cpp
        ${PIXELFORGE_PIXEL_PROGRAM_SOURCE}
        src/win32/SessionRecorder.cpp
        src/win32/RecordMcpServerRestricted.cpp
    )
    target_include_directories(PixelForge PRIVATE src/core src/win32 "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(PixelForge PRIVATE
        pixelforge_core
        user32 gdi32 comdlg32 ole32 windowscodecs shell32 advapi32
        mfplat mfreadwrite mfuuid
    )
    target_compile_definitions(PixelForge PRIVATE UNICODE _UNICODE NOMINMAX)
    add_custom_command(TARGET PixelForge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/codex.cmd"
            "$<TARGET_FILE_DIR:PixelForge>/codex.cmd")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(pixelforge_core_tests tests/test_core.cpp)
    target_link_libraries(pixelforge_core_tests PRIVATE pixelforge_core)
    add_test(NAME pixelforge_core_tests COMMAND pixelforge_core_tests)
    set_tests_properties(pixelforge_core_tests PROPERTIES TIMEOUT 60)

    add_executable(pixelforge_interaction_core_tests tests/test_interaction_core.cpp)
    target_link_libraries(pixelforge_interaction_core_tests PRIVATE pixelforge_core)
    add_test(NAME pixelforge_interaction_core_tests COMMAND pixelforge_interaction_core_tests)
    set_tests_properties(pixelforge_interaction_core_tests PROPERTIES TIMEOUT 60)

    add_executable(pixelforge_pixel_program_tests
        tests/test_pixel_program.cpp
        ${PIXELFORGE_PIXEL_PROGRAM_SOURCE})
    target_include_directories(pixelforge_pixel_program_tests PRIVATE src/win32 "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_test(NAME pixelforge_pixel_program_tests COMMAND pixelforge_pixel_program_tests)
    set_tests_properties(pixelforge_pixel_program_tests PROPERTIES TIMEOUT 60)

    if(WIN32)
        add_executable(pixelforge_project_persistence_tests
            tests/test_project_persistence.cpp src/win32/ImageIO.cpp)
        target_include_directories(pixelforge_project_persistence_tests PRIVATE src/win32)
        target_compile_definitions(pixelforge_project_persistence_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_project_persistence_tests PRIVATE pixelforge_core
            user32 gdi32 ole32 windowscodecs)
        add_test(NAME pixelforge_project_persistence_tests COMMAND pixelforge_project_persistence_tests)
        set_tests_properties(pixelforge_project_persistence_tests PROPERTIES TIMEOUT 60)

        set(PIXELFORGE_AUTOMATIC_TOOL_SOURCES
            src/win32/AgentInteractionUi.cpp
            ${PIXELFORGE_WORKSPACE_SOURCE}
            ${PIXELFORGE_MULTI_CANVAS_SOURCE}
            ${PIXELFORGE_PACK_UI_SOURCE}
            ${PIXELFORGE_PIXEL_PROGRAM_SOURCE}
            ${PIXELFORGE_AGENT_MCP_SOURCE}
            src/win32/ReferenceResponseLimiter.cpp
            src/win32/ImageIO.cpp
            src/win32/SessionRecorder.cpp
            src/win32/CodexSessionTrace.cpp)

        add_executable(pixelforge_automatic_tests tests/test_automatic.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_automatic_tests PRIVATE src/win32 "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(pixelforge_automatic_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_automatic_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_automatic_tests COMMAND pixelforge_automatic_tests)
        set_tests_properties(pixelforge_automatic_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(pixelforge_multicanvas_tests tests/test_multicanvas.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_multicanvas_tests PRIVATE src/win32 "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(pixelforge_multicanvas_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_multicanvas_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_multicanvas_tests COMMAND pixelforge_multicanvas_tests)
        set_tests_properties(pixelforge_multicanvas_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(pixelforge_reference_dialog_tests tests/test_reference_dialog.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_reference_dialog_tests PRIVATE src/win32 "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(pixelforge_reference_dialog_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_reference_dialog_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_reference_dialog_tests COMMAND pixelforge_reference_dialog_tests)
        set_tests_properties(pixelforge_reference_dialog_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_executable(pixelforge_large_workspace_tests tests/test_large_workspace.cpp
            src/win32/ProjectLoader.cpp src/win32/ProjectRestoreSession.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_large_workspace_tests PRIVATE src/win32 "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(pixelforge_large_workspace_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_large_workspace_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_large_workspace_tests COMMAND pixelforge_large_workspace_tests)
        set_tests_properties(pixelforge_large_workspace_tests PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
        add_test(NAME pixelforge_mcp_tests COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp.ps1"
            -Executable "$<TARGET_FILE:PixelForge>" -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/test-output")
        set_tests_properties(pixelforge_mcp_tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
