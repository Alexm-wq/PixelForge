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
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/MultiCanvasTool.cpp" PIXELFORGE_MULTI_CANVAS_TEXT)

    # Correct GIF frame disposal so transparent sprite frames do not accumulate.
    string(FIND "${PIXELFORGE_MULTI_CANVAS_TEXT}" "bytes.push_back(0x05);" PIXELFORGE_GIF_DISPOSAL_OFFSET)
    if(PIXELFORGE_GIF_DISPOSAL_OFFSET EQUAL -1)
        message(FATAL_ERROR "PixelForge GIF disposal marker was not found in MultiCanvasTool.cpp")
    endif()
    string(REPLACE
        "bytes.push_back(0x05);"
        "bytes.push_back(0x09); // GIF disposal=2 (restore background) + transparency"
        PIXELFORGE_MULTI_CANVAS_TEXT
        "${PIXELFORGE_MULTI_CANVAS_TEXT}")

    # LIST is observational and must never create a hidden fallback pack.
    string(REPLACE
[=[    auto workspace_it = g_pack_workspaces.find(bindings_.task);]=]
[=[    auto workspace_it = g_pack_workspaces.find(bindings_.task);
    if (workspace_it == g_pack_workspaces.end() && action == "LIST") {
        return {false, "{\"ok\":false,\"error\":\"pack_not_created\",\"message\":\"No multi-canvas pack exists yet. LIST is read-only and does not create one. For a genuinely new multi-canvas task, call CREATE once; for a loaded project, refresh task state because its restored pack should already exist.\"}"};
    }]=]
        PIXELFORGE_MULTI_CANVAS_TEXT
        "${PIXELFORGE_MULTI_CANVAS_TEXT}")

    # CREATE means create/replace. Astra is allowed to rebuild an existing pack
    # without a host-side preservation gate. Reinitializing the primary document
    # also makes replacement work when dimensions change and avoids inheriting
    # pixels from the previous first canvas when dimensions stay the same.
    string(FIND "${PIXELFORGE_MULTI_CANVAS_TEXT}" "Primary canvas already contains artwork at a different size." PIXELFORGE_CREATE_GUARD_OFFSET)
    if(PIXELFORGE_CREATE_GUARD_OFFSET EQUAL -1)
        message(FATAL_ERROR "PixelForge pack-create replacement guard marker was not found")
    endif()
    string(REPLACE
[=[        } else {
            std::lock_guard state_lock(*bindings_.state_mutex);
            if (bindings_.document->width() != specs.front().width || bindings_.document->height() != specs.front().height) {
                bool blank = true;
                for (const auto pixel : bindings_.document->pixels()) if (pixel != 0) { blank = false; break; }
                if (!blank)
                    return {false, "{\"ok\":false,\"error\":\"primary_canvas_size_mismatch\",\"message\":\"Primary canvas already contains artwork at a different size.\"}"};
                std::string resize_error;
                if (!bindings_.document->resize(specs.front().width, specs.front().height, &resize_error))
                    return {false, "{\"ok\":false,\"error\":\"resize_failed\",\"message\":" + quote_json(resize_error) + "}"};
            }
        }]=]
[=[        } else {
            std::lock_guard state_lock(*bindings_.state_mutex);
            std::string resize_error;
            if (!bindings_.document->resize(specs.front().width, specs.front().height, &resize_error))
                return {false, "{\"ok\":false,\"error\":\"resize_failed\",\"message\":" + quote_json(resize_error) + "}"};
        }]=]
        PIXELFORGE_MULTI_CANVAS_TEXT
        "${PIXELFORGE_MULTI_CANVAS_TEXT}")

    # INSPECT is observational. Never fail a model turn merely because the model
    # requested more than the canvas contains. Clip to the canvas and return the
    # actual rectangle. There is intentionally no arbitrary pixel-area ceiling.
    string(REPLACE
[=[        if (!rect_fits(*canvas->document, x, y, width, height) ||
            static_cast<std::uint64_t>(width) * height > 4096)
            return {false, "{\"ok\":false,\"error\":\"inspect_region_invalid\"}"};
        std::lock_guard state_lock(*bindings_.state_mutex);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"canvas\":" + quote_json(canvas->name) +
                      ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                      ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
                      ",\"rle\":" + quote_json(rle_region(*canvas->document, x, y, width, height)) + "}"};]=]
[=[        const int canvas_width = canvas->document->width();
        const int canvas_height = canvas->document->height();
        const std::int64_t requested_right = static_cast<std::int64_t>(x) + std::max(0, width);
        const std::int64_t requested_bottom = static_cast<std::int64_t>(y) + std::max(0, height);
        const int actual_x = std::clamp(x, 0, canvas_width);
        const int actual_y = std::clamp(y, 0, canvas_height);
        const int actual_right = static_cast<int>(std::clamp<std::int64_t>(requested_right, actual_x, canvas_width));
        const int actual_bottom = static_cast<int>(std::clamp<std::int64_t>(requested_bottom, actual_y, canvas_height));
        const int actual_width = std::max(0, actual_right - actual_x);
        const int actual_height = std::max(0, actual_bottom - actual_y);
        const bool clipped = actual_x != x || actual_y != y || actual_width != width || actual_height != height;
        std::lock_guard state_lock(*bindings_.state_mutex);
        const std::string rle = actual_width > 0 && actual_height > 0
            ? rle_region(*canvas->document, actual_x, actual_y, actual_width, actual_height)
            : std::string{};
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"canvas\":" + quote_json(canvas->name) +
                      ",\"requested_x\":" + std::to_string(x) + ",\"requested_y\":" + std::to_string(y) +
                      ",\"requested_width\":" + std::to_string(width) + ",\"requested_height\":" + std::to_string(height) +
                      ",\"x\":" + std::to_string(actual_x) + ",\"y\":" + std::to_string(actual_y) +
                      ",\"width\":" + std::to_string(actual_width) + ",\"height\":" + std::to_string(actual_height) +
                      ",\"clipped\":" + std::string(clipped ? "true" : "false") +
                      ",\"empty\":" + std::string(actual_width == 0 || actual_height == 0 ? "true" : "false") +
                      ",\"rle\":" + quote_json(rle) + "}"};]=]
        PIXELFORGE_MULTI_CANVAS_TEXT
        "${PIXELFORGE_MULTI_CANVAS_TEXT}")

    # COPY follows the same best-effort rule as pixel-program writes. Work in a
    # shared offset domain and clip to the portion that exists in both source and
    # destination instead of rejecting the complete operation.
    string(REPLACE
[=[        if (!rect_fits(*source->document, sx, sy, width, height) || !rect_fits(*dest->document, dx, dy, width, height))
            return {false, "{\"ok\":false,\"error\":\"copy_out_of_bounds\"}"};
        std::vector<std::uint32_t> source_pixels(static_cast<std::size_t>(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                source_pixels[static_cast<std::size_t>(y) * width + x] = source->document->pixel(sx + x, sy + y);
        std::size_t changed = 0;
        {
            std::lock_guard state_lock(*bindings_.state_mutex);
            auto tx = dest->document->begin_transaction();
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    tx.set_pixel(dx + x, dy + y, source_pixels[static_cast<std::size_t>(y) * width + x]);
            changed = tx.pending_changes();
            if (changed && !tx.commit()) return {false, "{\"ok\":false,\"error\":\"copy_failed\"}"};
            if (!changed) tx.cancel();
        }
        if (changed) push_history(workspace, {dest->name});
        if (dest->document == bindings_.document) PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"changed_pixels\":" + std::to_string(changed) + "}"};]=]
[=[        const int requested_width = std::max(0, width);
        const int requested_height = std::max(0, height);
        const int start_x = std::max({0, -sx, -dx});
        const int start_y = std::max({0, -sy, -dy});
        const int end_x = std::min({requested_width, source->document->width() - sx, dest->document->width() - dx});
        const int end_y = std::min({requested_height, source->document->height() - sy, dest->document->height() - dy});
        const int actual_width = std::max(0, end_x - start_x);
        const int actual_height = std::max(0, end_y - start_y);
        const int actual_sx = sx + start_x;
        const int actual_sy = sy + start_y;
        const int actual_dx = dx + start_x;
        const int actual_dy = dy + start_y;
        const bool clipped = actual_width != width || actual_height != height || start_x != 0 || start_y != 0;
        std::size_t changed = 0;
        if (actual_width > 0 && actual_height > 0) {
            std::vector<std::uint32_t> source_pixels(static_cast<std::size_t>(actual_width) * actual_height);
            for (int y = 0; y < actual_height; ++y)
                for (int x = 0; x < actual_width; ++x)
                    source_pixels[static_cast<std::size_t>(y) * actual_width + x] = source->document->pixel(actual_sx + x, actual_sy + y);
            {
                std::lock_guard state_lock(*bindings_.state_mutex);
                auto tx = dest->document->begin_transaction();
                for (int y = 0; y < actual_height; ++y)
                    for (int x = 0; x < actual_width; ++x)
                        tx.set_pixel(actual_dx + x, actual_dy + y, source_pixels[static_cast<std::size_t>(y) * actual_width + x]);
                changed = tx.pending_changes();
                if (changed && !tx.commit()) return {false, "{\"ok\":false,\"error\":\"copy_failed\"}"};
                if (!changed) tx.cancel();
            }
        }
        if (changed) push_history(workspace, {dest->name});
        if (dest->document == bindings_.document && changed) PostMessageW(bindings_.hwnd, WM_APP + 1, 0, 0);
        return {true, "{\"ok\":true,\"revision\":" + std::to_string(workspace.revision) +
                      ",\"changed_pixels\":" + std::to_string(changed) +
                      ",\"clipped\":" + std::string(clipped ? "true" : "false") +
                      ",\"empty\":" + std::string(actual_width == 0 || actual_height == 0 ? "true" : "false") +
                      ",\"sx\":" + std::to_string(actual_sx) + ",\"sy\":" + std::to_string(actual_sy) +
                      ",\"dx\":" + std::to_string(actual_dx) + ",\"dy\":" + std::to_string(actual_dy) +
                      ",\"width\":" + std::to_string(actual_width) + ",\"height\":" + std::to_string(actual_height) + "}"};]=]
        PIXELFORGE_MULTI_CANVAS_TEXT
        "${PIXELFORGE_MULTI_CANVAS_TEXT}")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(PIXELFORGE_MULTI_CANVAS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/MultiCanvasTool.cpp")
    file(WRITE "${PIXELFORGE_MULTI_CANVAS_SOURCE}" "${PIXELFORGE_MULTI_CANVAS_TEXT}")

    # Agent-facing workspace policy: loaded projects are informative context, not
    # protected state. Astra may intentionally rebuild them. Keep the richer tool
    # recovery guidance, but remove the mechanical create gate and stale wording.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/LocalAgentToolSessionWorkspace.cpp" PIXELFORGE_WORKSPACE_TEXT)
    string(REPLACE
        "if (pack_known_for(bindings_.task, task_id) && !replace_existing) {"
        "if (false && pack_known_for(bindings_.task, task_id) && !replace_existing) {"
        PIXELFORGE_WORKSPACE_TEXT
        "${PIXELFORGE_WORKSPACE_TEXT}")
    string(REPLACE
        ",\\\"preserve_existing_by_default\\\":true,\\\"pack_create_requires_replace_existing\\\":true"
        ",\\\"agent_has_full_workspace_control\\\":true,\\\"pack_create_requires_replace_existing\\\":false"
        PIXELFORGE_WORKSPACE_TEXT
        "${PIXELFORGE_WORKSPACE_TEXT}")
    string(REPLACE
        "create is for initializing a genuinely new pack; if a pack already exists it is destructive and is blocked unless replace_existing=true."
        "create initializes or replaces the pack; Astra may use it on an existing project whenever a full rebuild is appropriate."
        PIXELFORGE_WORKSPACE_TEXT
        "${PIXELFORGE_WORKSPACE_TEXT}")
    string(REPLACE
        "On an existing project, call list first and do not reuse this field to recreate the pack."
        "For create this defines the new/replacement pack; for view/export use exact existing names."
        PIXELFORGE_WORKSPACE_TEXT
        "${PIXELFORGE_WORKSPACE_TEXT}")
    string(REPLACE
        "DESTRUCTIVE opt-in for create when a pack already exists. Default false. Set true only after inspecting the existing pack and intentionally deciding to discard and replace the complete workspace."
        "Optional compatibility flag. Astra is already permitted to replace an existing pack without this flag."
        PIXELFORGE_WORKSPACE_TEXT
        "${PIXELFORGE_WORKSPACE_TEXT}")
    set(PIXELFORGE_WORKSPACE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/LocalAgentToolSessionWorkspace.cpp")
    file(WRITE "${PIXELFORGE_WORKSPACE_SOURCE}" "${PIXELFORGE_WORKSPACE_TEXT}")

    add_executable(PixelForge WIN32
        src/win32/main_auto.cpp
        src/win32/ImageIO.cpp
        src/win32/AgentMcpServerFramed.cpp
        src/win32/ReferenceResponseLimiter.cpp
        src/win32/PipeBridge.cpp
        src/win32/CodexAppClientDynamic.cpp
        src/win32/CodexSessionTrace.cpp
        src/win32/AgentInteractionUi.cpp
        src/win32/AutomaticUiRepaintGuard.cpp
        ${PIXELFORGE_WORKSPACE_SOURCE}
        src/win32/ProjectRestoreSession.cpp
        ${PIXELFORGE_MULTI_CANVAS_SOURCE}
        src/win32/PackWorkspaceUiTabs.cpp
        src/win32/ProjectLoader.cpp
        src/win32/PixelProgram.cpp
        src/win32/SessionRecorder.cpp
        src/win32/RecordMcpServerRestricted.cpp
    )
    target_include_directories(PixelForge PRIVATE src/core src/win32)
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
        src/win32/PixelProgram.cpp)
    target_include_directories(pixelforge_pixel_program_tests PRIVATE src/win32)
    add_test(NAME pixelforge_pixel_program_tests COMMAND pixelforge_pixel_program_tests)
    set_tests_properties(pixelforge_pixel_program_tests PROPERTIES TIMEOUT 60)

    if(WIN32)
        set(PIXELFORGE_AUTOMATIC_TOOL_SOURCES
            src/win32/AgentInteractionUi.cpp
            ${PIXELFORGE_WORKSPACE_SOURCE}
            ${PIXELFORGE_MULTI_CANVAS_SOURCE}
            src/win32/PackWorkspaceUiTabs.cpp
            src/win32/PixelProgram.cpp
            src/win32/AgentMcpServerFramed.cpp
            src/win32/ReferenceResponseLimiter.cpp
            src/win32/ImageIO.cpp
            src/win32/SessionRecorder.cpp
            src/win32/CodexSessionTrace.cpp)

        add_executable(pixelforge_automatic_tests tests/test_automatic.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_automatic_tests PRIVATE src/win32)
        target_compile_definitions(pixelforge_automatic_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_automatic_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_automatic_tests COMMAND pixelforge_automatic_tests)
        set_tests_properties(pixelforge_automatic_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(pixelforge_multicanvas_tests tests/test_multicanvas.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_multicanvas_tests PRIVATE src/win32)
        target_compile_definitions(pixelforge_multicanvas_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_multicanvas_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_multicanvas_tests COMMAND pixelforge_multicanvas_tests)
        set_tests_properties(pixelforge_multicanvas_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(pixelforge_reference_dialog_tests tests/test_reference_dialog.cpp ${PIXELFORGE_AUTOMATIC_TOOL_SOURCES})
        target_include_directories(pixelforge_reference_dialog_tests PRIVATE src/win32)
        target_compile_definitions(pixelforge_reference_dialog_tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(pixelforge_reference_dialog_tests PRIVATE pixelforge_core
            user32 gdi32 comdlg32 ole32 windowscodecs mfplat mfreadwrite mfuuid)
        add_test(NAME pixelforge_reference_dialog_tests COMMAND pixelforge_reference_dialog_tests)
        set_tests_properties(pixelforge_reference_dialog_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME pixelforge_mcp_tests COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp.ps1"
            -Executable "$<TARGET_FILE:PixelForge>" -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/test-output")
        set_tests_properties(pixelforge_mcp_tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
