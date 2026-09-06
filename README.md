# PixelForge

Agent-native pixel-art editor for Codex.

PixelForge is a native C++ pixel editor designed around a compact agent protocol. The GUI is for the user; Codex edits the exact same live document through MCP rather than GUI automation.

## Automatic workflow

The default PixelForge executable now drives Codex directly through Codex App Server.

Normal use is:

1. Launch `build/Release/PixelForge.exe` normally — no command-line arguments.
2. Load an optional **Content Reference**.
3. Load an optional **Style Reference**.
4. Enter the artwork prompt.
5. Click **Generate with Codex**.
6. PixelForge creates the task, starts a dedicated Codex App Server art turn, exposes the live document through a private named-pipe MCP bridge, and updates the canvas while Codex works.
7. Codex chooses the canvas size, draws/refines the sprite, then calls `task.finish`, `task.reject`, or `task.abort`.

You do **not** need to open a Codex chat or send a second prompt. PixelForge uses the existing local Codex CLI login. If `codex.exe`/`codex.cmd` is not on PATH, set `PIXELFORGE_CODEX_EXE` to its full path.

Automatic art turns are intentionally isolated: Codex runs with `approvalPolicy=never`, a read-only filesystem sandbox, and only the PixelForge MCP server injected for that turn. Artwork changes therefore go through PixelForge's revisioned pixel tools rather than shell/file edits. A fresh ephemeral Codex context is used for each Generate operation.

Codex App Server diagnostics are written to:

```text
build/pixelforge-codex-app-server.log
```

## Implemented editor/agent features

- Dependency-free Win32 C++20 UI (Win32/GDI/WIC only)
- Editable nearest-neighbor pixel canvas
- User prompt panel and explicit task lifecycle
- Agent-selected arbitrary canvas size within hard technical limits
- **Content Reference** image slot
- **Style Reference** image slot
- Lossless transparent PNG export
- Revisioned atomic batch edits
- Delta undo/redo
- Automatic Generate → Codex App Server integration
- Private named-pipe MCP bridge into the already-open GUI document
- Manual stdio MCP mode via `PixelForge.exe --mcp`
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

For a repeatable configure/build/test workflow, run `./build.ps1` from PowerShell. Use `./build.ps1 -Configuration Debug` for Debug or add `-Run` to open the tested GUI. Requires CMake, Visual Studio 2022 C++ Build Tools and a Windows SDK.

The user's incremental `rebuild_pixelforge.bat` workflow remains compatible with the same target/output path.

## Manual Codex-driven MCP mode

Automatic Generate does **not** require a global PixelForge MCP entry in `~/.codex/config.toml`; PixelForge injects a temporary private bridge into the Codex App Server process itself.

For development/debugging where Codex should launch PixelForge as an ordinary MCP server, the old mode is still supported. Copy the example from `config/codex_mcp.toml.example`:

```toml
[mcp_servers.pixelforge]
command = 'C:\dev\PixelForge\build\Release\PixelForge.exe'
args = ["--mcp"]
startup_timeout_sec = 20
tool_timeout_sec = 120
```

In this manual mode, Codex owns the stdio pipes and launches PixelForge. Do not pre-launch `PixelForge.exe --mcp` yourself.

`--bridge <pipe>` is an internal headless mode used by automatic Generate. It forwards Codex MCP stdio to the named-pipe server owned by the already-open PixelForge GUI; it does not create a second document or second editor window.

## Efficient agent workflow

A typical task is:

1. `pixelforge_task(get)` to read the GUI-created task.
2. Read content/style references once with `pixelforge_view`.
3. `accept` with the requested/inferred pixel canvas size.
4. Optionally set a compact working palette once.
5. Send large atomic patches through `pixelforge_edit`.
6. Request a render only when visual judgment is useful.
7. Reuse `known_observation` to avoid retransmitting unchanged images.
8. Use cropped renders/inspection during local cleanup rather than repeatedly observing the whole canvas.
9. Final whole-sprite visual inspection, then `finish`.

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

Content and style references are observation-addressed and sent at their original loaded resolution rather than silently downscaled.

## Agent contract

The system contract is `config/agent_system_prompt.md`. Automatic Generate injects it into the Codex thread as developer instructions, so the agent does not need an extra filesystem read to learn PixelForge's scope and workflow. The build also copies the file beside `PixelForge.exe`.

PixelForge currently edits one in-memory canvas. Export before closing: there is no project save/reopen, canvas import, layer or animation support. Reference slots load images for observation; they do not import pixels into the canvas.

See:

- `config/agent_system_prompt.md`
- `config/codex_mcp.toml.example`
- `docs/AGENT_PROTOCOL.md`
- `docs/ARCHITECTURE.md`
