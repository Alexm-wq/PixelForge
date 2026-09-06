# PixelForge

Agent-native pixel-art editor for Codex.

PixelForge is a native C++ pixel editor designed around a compact agent protocol. The GUI is for the user; the agent edits the same document through structured commands rather than GUI automation.

## Current prototype

- Dependency-free Win32 C++20 UI (Win32/GDI/WIC only)
- Editable nearest-neighbor pixel canvas
- User prompt panel and explicit task lifecycle
- Agent-selected arbitrary canvas size within hard technical limits
- **Content Reference** image slot
- **Style Reference** image slot
- PNG reference loading and transparent PNG export
- Revisioned batch edits
- Delta undo/redo
- Portable core tests
- Windows GitHub Actions build validation

## Important scope boundary

PixelForge does **not** decide whether a user prompt is semantically appropriate for a pixel editor.

The agent receives the original prompt and the system contract in `config/agent_system_prompt.md`. It then chooses:

- `task.accept(width, height)` and starts editing, or
- `task.reject(reason)` and terminates without creating a canvas.

The application itself only blocks hard technical violations such as invalid dimensions, out-of-bounds pixel operations, unsupported file decoding, or configured resource limits.

## Build on Windows

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable will be under `build/Release/PixelForge.exe` for the standard Visual Studio generator layout.

## Test portable core

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Agent contract

See:

- `config/agent_system_prompt.md`
- `docs/AGENT_PROTOCOL.md`
- `docs/ARCHITECTURE.md`

## Next milestone

Add the local stdio/MCP bridge with a deliberately small tool surface: task lifecycle, inspect, cached render, batched edit, compact patch, history, and IO.
