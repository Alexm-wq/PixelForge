cmake_minimum_required(VERSION 3.20)
project(PixelForge VERSION 0.2.0 LANGUAGES CXX)

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
        src/win32/main_mcp.cpp
        src/win32/ImageIO.cpp
        src/win32/AgentMcpServer.cpp
    )
    target_include_directories(PixelForge PRIVATE src/core src/win32)
    target_link_libraries(PixelForge PRIVATE
        pixelforge_core
        user32 gdi32 comdlg32 ole32 windowscodecs
    )
    target_compile_definitions(PixelForge PRIVATE UNICODE _UNICODE NOMINMAX)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(pixelforge_core_tests tests/test_core.cpp)
    target_link_libraries(pixelforge_core_tests PRIVATE pixelforge_core)
    add_test(NAME pixelforge_core_tests COMMAND pixelforge_core_tests)
endif()
