cmake_minimum_required(VERSION 3.20)
project(PixelForge VERSION 0.3.0 LANGUAGES CXX)

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
        src/win32/PipeBridge.cpp
        src/win32/CodexAppClientTraced.cpp
        src/win32/CodexSessionTrace.cpp
        src/win32/SessionRecorder.cpp
        src/win32/RecordMcpServer.cpp
    )
    target_include_directories(PixelForge PRIVATE src/core src/win32)
    target_link_libraries(PixelForge PRIVATE
        pixelforge_core
        user32 gdi32 comdlg32 ole32 windowscodecs shell32
        mfplat mfreadwrite mfuuid
    )
    target_compile_definitions(PixelForge PRIVATE UNICODE _UNICODE NOMINMAX)
    add_custom_command(TARGET PixelForge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/agent_system_prompt.md"
            "$<TARGET_FILE_DIR:PixelForge>/agent_system_prompt.md"
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
    if(WIN32)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME pixelforge_mcp_tests COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp.ps1"
            -Executable "$<TARGET_FILE:PixelForge>" -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/test-output")
        set_tests_properties(pixelforge_mcp_tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
