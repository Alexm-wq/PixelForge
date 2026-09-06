# PixelForge architecture

PixelForge is a portable C++20 document core with a native Win32/GDI/WIC host.

## Automatic Codex path

The normal executable is now an embedded Codex client as well as the editor:

```text
PixelForge.exe (GUI + PixelDocument)
        |
        | Codex App Server JSONL
        v
local Codex App Server
        |
        | launches only the PixelForge MCP bridge
        v
PixelForge.exe --bridge <private named pipe>
        |
        | raw MCP bytes over named pipe
        v
GUI-owned MCP server -> same PixelDocument
```

The bridge process is deliberately stateless. It never creates a canvas, task, reference image, or second GUI. It only forwards the Codex-owned MCP stdin/stdout stream to the private named pipe opened by the already-running PixelForge editor.

`PipeMcpHost` reuses the existing MCP implementation against the GUI-owned `PixelDocument`, `AgentTaskController`, reference slots and mutex. Consequently mouse edits and Codex edits share the same revision counter and stale-revision protection.

A normal **Generate with Codex** operation:

1. snapshots the current prompt into a new PixelForge task;
2. starts a dedicated Codex App Server process using the user's existing Codex authentication;
3. injects only the private PixelForge bridge as an MCP server for that art turn;
4. creates a fresh ephemeral Codex thread with the PixelForge system contract as developer instructions;
5. runs the agent read-only/no-approval so artwork must flow through MCP;
6. streams status while MCP edits update the live GUI document;
7. terminates the task-scoped App Server after completion so old image/tool context and bridge processes do not accumulate.

Manual `PixelForge.exe --mcp` remains available for development or for a Codex session that should launch the editor itself.

## Core document model

The six tools route lifecycle, patches, observations, palettes, history and export. Semantic scope decisions belong to the agent. The application validates dimensions, coordinates, task state and revision numbers.

Transactions retain the first old value and final new value per changed pixel. Tiny transactions scan at most 64 entries; larger transactions switch to a dense pixel-to-change index for constant-time writes. Net no-ops leave revision/history unchanged. Transactions reject commits after another mutation or resize. Undo/redo use deltas and monotonically increasing revisions. History is held in memory.

The GUI composites straight alpha over a checkerboard into one bitmap and uses nearest-neighbor drawing. Large canvases fit the viewport. Export preserves exact ARGB without the GUI checkerboard or grid. References are decoded through WIC, limited to 64 megapixels, and observations use the loaded snapshot.

Render cache keys include a pixel-content hash, source width, task, revision, crop and scale, preventing stale images when process-local counters restart. PNG cache files live in the Windows temporary PixelForge/observations directory. They are not a project save format and are not automatically pruned. Observation IDs avoid retransmitting unchanged images. Patch input is buffered in 16 KiB chunks with an 8 MiB line limit. Inspection uses row-major run-length encoding.

## Runtime boundaries

Automatic Codex generation is intentionally isolated from repository mutation. The art thread uses a read-only filesystem sandbox and `approvalPolicy=never`; it is instructed not to use shell/source editing for artwork. PixelForge itself remains responsible for its document state and MCP operations.

App Server stderr is written to `build/pixelforge-codex-app-server.log`. `PIXELFORGE_CODEX_EXE` can override Codex CLI discovery when `codex.exe`/`codex.cmd` is not on PATH.

Current editor boundaries: one in-memory canvas, no project persistence/import, layers, animation, transforms or flood fill. Agent patches are the primary drawing path; the GUI provides preview, reference loading, simple pixel touchups and PNG saving.
