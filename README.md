# PixelForge

Agent-native pixel-art editor for Codex.

PixelForge is a native C++ pixel editor designed around a compact agent protocol. The GUI is for the user; Codex edits the exact same live document through MCP rather than GUI automation.

## Implemented workflow

- Dependency-free Win32 C++20 UI (Win32/GDI/WIC only)
- Editable nearest-neighbor pixel canvas
- User prompt panel and explicit task lifecycle
- Agent-selected arbitrary canvas size within hard technical limits
- **Content Reference** image slot
- **Style Reference** image slot
- Lossless transparent PNG export
- Revisioned atomic batch edits
- Delta undo/redo
- Live stdio MCP server via `PixelForge.exe --mcp`
- Six-tool compact MCP surface rather than many micro-tools
- Palette-indexed compact patches with exact `#AARRGGBB` fallback
- Cached render observations with observation IDs
- Full-fidelity reference-image delivery through MCP image content
- Stale task/revision protection for simultaneous user + agent editing

## Agent scope boundary

PixelForge does **not** decide whether a user prompt is semantically appropriate for a pixel editor.

The agent receives the original prompt, references, tool contract, and the scope contract in `config/agent_system_prompt.md`. It chooses:

- `task.accept(width, height)` and starts editing,
- `task.reject(reason)` before editing when the requested final medium is outside PixelForge's pixel-art scope,
- `task.abort(reason)` after acceptance only when a concrete technical blocker is discovered, or
- `task.finish(summary)` when the sprite is complete.

The application itself only blocks hard technical violations such as invalid dimensions, out-of-bounds operations, malformed patches, unsupported image decoding, stale revisions, or configured resource limits.

## Build on Windows

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable will be under `build/Release/PixelForge.exe` for the standard Visual Studio generator layout.

For a repeatable configure/build/test workflow, run `./build.ps1` from PowerShell.
Use `./build.ps1 -Configuration Debug` for Debug or add `-Run` to open the tested
GUI. Requires CMake, Visual Studio 2022 C++ Build Tools and a Windows SDK.
The script works relative to its own folder and does not pull Git changes or
terminate an existing editor. Close the application before rebuilding its EXE.

CTest includes always-active core checks and a real MCP integration test that
launches hidden app instances, draws a gem, checks revisions/history/cache behavior
and verifies PNG pixels and transparency. Its example output is
`build/test-output/mcp-gem.png`. Windows CI publishes the executable and prompt.

## Connect Codex

Copy the example block from `config/codex_mcp.toml.example` into your Codex `~/.codex/config.toml`:

```toml
[mcp_servers.pixelforge]
command = 'C:\dev\PixelForge\build\Release\PixelForge.exe'
args = ["--mcp"]
startup_timeout_sec = 20
tool_timeout_sec = 120
```

Start a new Codex session after changing the MCP configuration. Codex owns the stdio pipes and launches PixelForge automatically. The PixelForge GUI opens normally, but the same process also serves MCP.

Do **not** manually launch `PixelForge.exe --mcp` before Codex. A stdio MCP server must be launched by the MCP client that owns its stdin/stdout pipes.

## Efficient agent workflow

A typical task should be roughly:

1. `pixelforge_task(get)` to read a task entered in the GUI, or `begin` when Codex originates it.
2. Read content/style references once with `pixelforge_view`.
3. `accept` with the requested/inferred pixel canvas size.
4. Optionally set a compact working palette once.
5. Send large atomic patches through `pixelforge_edit`.
6. Request a render only when visual judgment is useful.
7. Reuse `known_observation` to avoid retransmitting unchanged images.
8. Use cropped renders/inspection during local cleanup rather than repeatedly observing the whole canvas.
9. `finish`, then export the native-resolution PNG.

Patch grammar:

```text
P,x,y,c                 single pixel
H,x,y,len,c             horizontal run
V,x,y,len,c             vertical run
R,x,y,width,height,c    filled rectangle
L,x0,y0,x1,y1,c         exact integer line
```

`c` can be a palette index or an exact `#AARRGGBB` color. Palette indexing is only transport compression; PixelForge continues storing the document as full 32-bit ARGB, so compact tool usage does not reduce color fidelity.

## Visual observations

Canvas renders are cached by task, revision, crop, and integer scale. They are encoded as lossless PNG with nearest-neighbor scaling. If Codex supplies the returned `known_observation` ID for the same unchanged render, PixelForge returns only an `unchanged` response instead of sending the image again.

Content and style references are also observation-addressed and are sent at their original resolution rather than silently downscaled.

## Agent contract

The ready-to-use system prompt is **`config/agent_system_prompt.md`**. Have the
drawing agent read this file before using the six tools, or put its contents in
the agent's system instructions. The build also copies it beside `PixelForge.exe`.
It contains exact arguments, patch examples, limits, visual refinement guidance,
stale-revision recovery and the finish/export workflow.

PixelForge currently edits one in-memory canvas. Export before closing: there is
no project save/reopen, canvas import, layer or animation support. Reference slots
load images for observation; they do not import pixels into the canvas.

See:

- `config/agent_system_prompt.md`
- `config/codex_mcp.toml.example`
- `docs/AGENT_PROTOCOL.md`
- `docs/ARCHITECTURE.md`
