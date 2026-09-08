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
    add_executable(PixelForge WIN32
        src/win32/main_auto.cpp
        src/win32/ImageIO.cpp
        src/win32/AgentMcpServerFramed.cpp
        src/win32/ReferenceResponseLimiter.cpp
        src/win32/PipeBridge.cpp
        src/win32/CodexAppClientDynamic.cpp
        src/win32/CodexSessionTrace.cpp
        src/win32/AgentInteractionUi.cpp
        src/win32/LocalAgentToolSessionWorkspace.cpp
        src/win32/MultiCanvasTool.cpp
        src/win32/PackWorkspaceUiSafe.cpp
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
            src/win32/LocalAgentToolSessionWorkspace.cpp
            src/win32/MultiCanvasTool.cpp
            src/win32/PackWorkspaceUiSafe.cpp
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