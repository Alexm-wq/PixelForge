# PixelForge architecture

PixelForge is a portable C++20 document core with a native Win32/GDI/WIC host.
`main_mcp.cpp` owns the GUI and document. `AgentMcpServerFramed.cpp` builds the
MCP adapter with JSONL response framing. MCP runs on a worker; the GUI and adapter
lock the same mutex while reading or mutating the document.

The six tools route lifecycle, patches, observations, palettes, history and export.
Semantic scope decisions belong to the agent. The application validates dimensions,
coordinates, task state and revision numbers. No external runtime is needed to run
the application; PowerShell is used only for build/test automation.

Transactions retain the first old value and final new value per changed pixel.
Tiny transactions scan at most 64 entries; larger transactions switch to a dense
pixel-to-change index for constant-time writes. Net no-ops leave revision/history
unchanged. Transactions reject commits after another mutation or resize. Undo/redo
use deltas and monotonically increasing revisions. History is held in memory.

The GUI composites straight alpha over a checkerboard into one bitmap and uses
nearest-neighbor drawing. Large canvases fit the viewport. Export preserves exact
ARGB without the GUI checkerboard or grid. References are decoded through WIC,
limited to 64 megapixels, and observations use the loaded snapshot.

Render cache keys include a pixel-content hash, source width, task, revision, crop
and scale, preventing stale images when process-local counters restart. PNG cache
files live in the Windows temporary PixelForge/observations directory. They are
not a project save format and are not automatically pruned. Observation IDs avoid
retransmitting unchanged images. Patch input is buffered in 16 KiB chunks with an
8 MiB line limit. Inspection uses row-major run-length encoding.

Tests exercise portable state/transaction behavior (including a maximum-size
fill), plus the actual Windows executable over redirected MCP pipes, WIC PNG
export, reference images and process restart caching. Checks remain active in
Release builds. CTest runs both suites and Windows CI uploads the tested executable
with its agent prompt.

Current boundaries: one in-memory canvas, no project persistence/import, layers,
animation, transforms or flood fill. Agent patches are the primary drawing path;
the GUI provides preview, reference loading, simple pixel touchups and PNG saving.
