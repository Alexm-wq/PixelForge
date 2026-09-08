# PixelForge

Agent-native pixel-art editor for Codex.

PixelForge is a native C++ pixel editor designed around a compact agent protocol. The GUI is for the user; Codex edits the exact same live document through MCP rather than GUI automation.

## Automatic workflow

The default PixelForge executable drives Codex directly through Codex App Server.

Normal use is:

1. Launch `build/Release/PixelForge.exe` normally — no command-line arguments.
2. Load an optional **Content Reference**.
3. Load an optional **Style Reference**.
4. Enter the artwork prompt.
5. Click **Generate with Codex**.
6. PixelForge creates the task, starts a dedicated Codex App Server art turn, exposes the live document through a private named-pipe MCP bridge, and updates the canvas while Codex works.
7. Codex chooses the canvas size, draws/refines the sprite, then calls `task.finish`, `task.reject`, or `task.abort`.

References are optional. Prompt-only, content-only, style-only, and content+style workflows are all supported.

You do **not** need to open a Codex chat or send a second prompt. PixelForge uses the existing local Codex CLI login. The build copies `tools/codex.cmd` beside `PixelForge.exe`; this resolver searches ordinary PATH installs plus common npm, pnpm, FNM, Bun, Scoop and Codex Desktop locations. For unusual installations, set `CODEX_CLI_PATH` or `PIXELFORGE_CODEX_EXE` to the full `codex.exe`/`codex.cmd` path.

Automatic art turns are intentionally isolated: Codex runs with `approvalPolicy=never`, a read-only filesystem sandbox, and only PixelForge's private art/recording MCP bridges injected for that turn. Artwork changes therefore go through PixelForge's revisioned pixel tools rather than shell/file edits. A fresh ephemeral Codex context is used for each Generate operation.

Codex App Server diagnostics are written to:

```text
build/pixelforge-codex-app-server.log
```

## Optional session recording

Recording is opt-in through the prompt. PixelForge does not keyword-classify recording requests; Codex decides whether the user actually requested a recording.

For example:

```text
Create a 64x64 hive queen and record the drawing process.
```

When recording is requested, Codex uses the separate `pixelforge_record` control tool to start recording before the first canvas mutation and stop it after the final visual inspection. PixelForge records the visible editor client area to H.264 MP4 using Windows Media Foundation.

Recordings are stored locally under:

```text
recordings\pixelforge-task-<id>-YYYYMMDD-HHMMSS.mp4
```

The recording MCP server exposes only `start`, `status`, and `stop`. It has no action for reading frames, returning the path, previewing the video, or sending video bytes to Codex. The art observation tools likewise cannot access recordings. If a Codex turn ends while recording is still active, PixelForge automatically finalizes the MP4.

## Implemented editor/agent features

- Native Win32 C++20 UI (Win32/GDI/WIC; Media Foundation for optional MP4 recording)
- Editable nearest-neighbor pixel canvas
- User prompt panel and explicit task lifecycle
- Agent-selected arbitrary canvas size within hard technical limits
- Optional **Content Reference** image slot
- Optional **Style Reference** image slot
- Lossless transparent PNG export
- Revisioned atomic batch edits
- Delta undo/redo
- Automatic Generate → Codex App Server integration
- Private named-pipe MCP bridge into the already-open GUI document
- Local-only optional H.264 MP4 session recording
- Manual stdio MCP mode via `PixelForge.exe --mcp`
- Six broad artwork MCP tools plus one isolated recording-control tool
- Palette-indexed compact patches with exact `#AARRGGBB` fallback
- Cached render observations with observation IDs
- Full-fidelity reference-image delivery through MCP image content
- Stale task/revision protection for simultaneous user + agent editing

## Agent scope boundary

PixelForge does **not** decide whether a user prompt is semantically appropriate for a pixel editor, nor whether wording semantically constitutes a recording request.

The agent receives the original prompt, references, tool contract, and the scope contract in `config/agent_system_prompt.md`. It chooses:

- `task.accept(width, height)` and starts editing,
- `task.reject(reason)` before editing when the requested final medium is outside PixelForge's pixel-art scope,
- `task.abort(reason)` after acceptance only when a concrete technical blocker is discovered,
- `pixelforge_record start/stop` only when the user requested session recording, or
- `task.finish(summary)` when the sprite is complete.

The application itself only blocks hard technical violations such as invalid dimensions, out-of-bounds operations, malformed patches, unsupported image decoding, stale revisions, configured resource limits, or recorder/encoder failures.

## Build on Windows

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable will be under `build/Release/PixelForge.exe` for the standard Visual Studio generator layout. The build also copies `agent_system_prompt.md` and the Codex resolver `codex.cmd` beside the executable.

For a repeatable configure/build/test workflow, run `./build.ps1` from PowerShell. Use `./build.ps1 -Configuration Debug` for Debug or add `-Run` to open the tested GUI. Requires CMake, Visual Studio 2022 C++ Build Tools and a Windows SDK.

The user's incremental `rebuild_pixelforge.bat` workflow remains compatible with the same target/output path.

## Manual Codex-driven MCP mode

Automatic Generate does **not** require a global PixelForge MCP entry in `~/.codex/config.toml`; PixelForge injects temporary private bridges into the Codex App Server process itself.

For development/debugging where Codex should launch PixelForge as an ordinary MCP server, the old mode is still supported. Copy the example from `config/codex_mcp.toml.example`:

```toml
[mcp_servers.pixelforge]
command = 'C:\dev\PixelForge\build\Release\PixelForge.exe'
args = ["--mcp"]
startup_timeout_sec = 20
tool_timeout_sec = 120
```

In this manual mode, Codex owns the stdio pipes and launches PixelForge. Do not pre-launch `PixelForge.exe --mcp` yourself.

`--bridge <pipe>` and `--record-bridge <pipe>` are internal headless modes used by automatic Generate. They forward MCP stdio to named-pipe servers owned by the already-open PixelForge GUI; they do not create a second document or second editor window.

## Efficient agent workflow

A typical task is:

1. `pixelforge_task(get)` to read the GUI-created task.
2. Read only references that are actually present, once, with `pixelforge_view`.
3. If the user explicitly requested recording, start `pixelforge_record` before the first canvas mutation.
4. `accept` with the requested/inferred pixel canvas size.
5. Optionally set a compact working palette once.
6. Send large atomic patches through `pixelforge_edit`.
7. Request a render only when visual judgment is useful.
8. Reuse `known_observation` to avoid retransmitting unchanged images.
9. Use cropped renders/inspection during local cleanup rather than repeatedly observing the whole canvas.
10. Final whole-sprite visual inspection; stop requested recording; then `finish`.

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

Content and style references are observation-addressed and sent at their original loaded resolution rather than silently downscaled. Recordings are deliberately outside the observation system.

## Agent contract

The system contract is `config/agent_system_prompt.md`. Automatic Generate injects it into the Codex thread as developer instructions, so the agent does not need an extra filesystem read to learn PixelForge's scope, recording rules and workflow. The build also copies the file beside `PixelForge.exe`.

PixelForge supports saved multi-canvas projects and animation groups, with Source
image seeding and bulk project reopening. Canvases and undo history remain resident
in memory; large project capacity depends on total pixels and available RAM.
Accept finalizes the project manifest after review. Export creates requested PNG
or GIF deliverables; it is separate from project autosave.

For large workspaces, use `pixelforge_pack list` / `analyze` with `summary:true`
to survey groups, then filter a group for frame details. Results are paginated.
Analysis caches frame measurements and adjacent-frame comparisons. Views return
bounded preview pages and suppress unchanged images; exports preserve full
resolution. Edits save only changed canvases and refresh affected UI snapshots.
See `docs/LARGE_WORKSPACES.md` for the current behavior and measured checks.

See:

- `config/agent_system_prompt.md`
- `config/codex_mcp.toml.example`
- `docs/AGENT_PROTOCOL.md`
- `docs/ARCHITECTURE.md`

## Automatic-generation diagnostics and stopping

Generate uses the configured model with medium reasoning effort. It asks for a
small first silhouette batch, followed by incremental refinement. It stops after
120 seconds without an effective pixel edit, three consecutive tool failures,
or 10 minutes overall. Background usage/status events do not reset these limits.
Use **Stop** to cancel the running session and keep the current canvas. There is
no automatic retry that silently spends more usage.

The status panel distinguishes reasoning from tool execution and displays agent
messages and tool failures. Detailed local diagnostics are in:

- `build/pixelforge-codex-session.log`: tool/action, success, elapsed time,
  revision/change counts, error text, agent messages and token counts.
- `build/pixelforge-codex-session.log.previous`: preceding run.
- `build/pixelforge-codex-app-server.log`: child-process stderr.

Image/video bytes and raw patch strings are omitted. Agent messages may quote
parts of the drawing request. Logs describe the JSONL actually sent on the wire.

`ctest --test-dir build -C Release --output-on-failure` includes offline automatic
client tests with a fake app server: repeated sessions, actual pixel tools and
image results, incomplete turns, failed tools, telemetry-only stalls and Stop.
These tests make no model requests. Offline test traces go to
`build/Release/offline-session.log`, keeping real session diagnostics separate.

Automatic turns now supply a concise pixel-art base instruction instead of the
standard coding-agent base prompt. Repeated observation images are suppressed
within a session even if the agent forgets its observation ID; `resend_image:true`
explicitly requests another copy. Rejected geometry reports the failing operation
number and coordinates so the agent can repair it without inspecting a blank canvas.
The session log includes `USAGE scope=total` and `scope=last`, separating cached
and uncached input from generated output; these counters are not an account-limit
percentage or a dollar charge. Drawing instructions require a silhouette/value/
proportion check before texture. This improves the workflow, but artistic quality
still depends on the model and must be judged visually.
