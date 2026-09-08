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

    # INSPECT errors must be self-correcting. Return the exact request, canvas
    # bounds, hard area limit, and the specific failed condition so Astra does
    # not burn three retries guessing why the region was rejected.
    string(REPLACE
[=[        if (!rect_fits(*canvas->document, x, y, width, height) ||
            static_cast<std::uint64_t>(width) * height > 4096)
            return {false, "{\"ok\":false,\"error\":\"inspect_region_invalid\"}"};]=]
[=[        const bool region_fits = rect_fits(*canvas->document, x, y, width, height);
        const bool positive_region = width > 0 && height > 0;
        const std::uint64_t area = positive_region
            ? static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)
            : 0;
        if (!region_fits || area > 4096) {
            const std::string reason = !positive_region ? "non_positive_size" :
                                       (!region_fits ? "out_of_bounds" : "area_exceeds_limit");
            return {false, "{\"ok\":false,\"error\":\"inspect_region_invalid\",\"reason\":" + quote_json(reason) +
                          ",\"canvas\":" + quote_json(canvas->name) +
                          ",\"canvas_width\":" + std::to_string(canvas->document->width()) +
                          ",\"canvas_height\":" + std::to_string(canvas->document->height()) +
                          ",\"requested_x\":" + std::to_string(x) +
                          ",\"requested_y\":" + std::to_string(y) +
                          ",\"requested_width\":" + std::to_string(width) +
                          ",\"requested_height\":" + std::to_string(height) +
                          ",\"requested_area\":" + std::to_string(area) +
                          ",\"max_area\":4096,\"message\":\"INSPECT requires x>=0, y>=0, width>0, height>0, x+width<=canvas_width, y+height<=canvas_height, and width*height<=4096. Correct the rectangle directly; LIST is not required unless the canvas name itself is stale.\"}"};
        }]=]
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
    string(REPLACE
[=[    } else if (has("\"error\":\"primary_canvas_size_mismatch\"")) {]=]
[=[    } else if (has("\"error\":\"inspect_region_invalid\"")) {
        add_member(result.text,
            ",\"recovery\":\"Use the returned reason, canvas_width/canvas_height, requested rectangle, and max_area to correct INSPECT directly. Do not repeat the same rectangle and do not call LIST unless the canvas name is stale.\"");
    } else if (has("\"error\":\"primary_canvas_size_mismatch\"")) {]=]
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
