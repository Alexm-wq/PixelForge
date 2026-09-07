# PixelForge agent protocol

PixelForge supports automatic and manual MCP hosting paths.

## Automatic Generate path

Normal GUI launch:

```text
PixelForge.exe
```

The user optionally loads references, enters a prompt, and clicks **Generate with Codex**. PixelForge starts a local Codex App Server turn automatically and injects two private MCP connections:

```text
PixelForge.exe --bridge <private pixel pipe>
PixelForge.exe --record-bridge <private recording pipe>
```

Both bridge modes are headless and stateless. The pixel bridge forwards to the live `PixelDocument` in the already-open GUI. The recording bridge forwards only local recorder-control commands. Neither bridge creates a second editor document.

No global PixelForge MCP configuration is required for this automatic path.

Each Generate operation uses a fresh ephemeral Codex thread with the PixelForge agent contract injected as developer instructions. The automatic art turn uses a read-only filesystem sandbox and no approvals; artwork therefore goes through PixelForge tools rather than repository/source writes. The task-scoped App Server is terminated after the turn to release bridge and observation context.

PixelForge does not currently pin a Codex model in `thread/start`; the installed Codex CLI/config chooses its default. The compact per-run trace records `model=...` whenever App Server reports the selected model.

## Automatic-run diagnostics

Each Generate run truncates and rewrites:

```text
build/pixelforge-codex-session.log
```

This compact trace records App Server RPC/notification method names, request IDs, item/tool metadata, turn status, reported model, and compact failure messages. It deliberately does not record raw prompts, pixel patches, image/base64 data, reference bytes, agent prose, or recording video.

Codex App Server stderr remains separate in:

```text
build/pixelforge-codex-app-server.log
```

Use the session trace for a turn that consumed usage but ended without `task.finish`; use stderr for process/config/auth/crash diagnostics.

## Manual MCP path

For development/debugging, PixelForge can still be launched by any stdio MCP client as:

```text
PixelForge.exe --mcp
```

The native GUI remains visible. MCP and mouse editing operate on the same revisioned `PixelDocument` in that process. This manual path exposes the six artwork tools; automatic Generate additionally injects the isolated recording-control server.

## Design rule

Agent efficiency is optimized without reducing artwork fidelity:

- the canvas remains full 32-bit ARGB;
- edits are exact deterministic pixel operations;
- palette indices compress the wire format only;
- any color can always be supplied as exact `#AARRGGBB`;
- canvas renders are lossless PNG with nearest-neighbor integer scaling;
- content/style references are optional and remain full resolution inside PixelForge;
- the reference copy sent to Codex is proportionally reduced only when total pixel area exceeds 65,536 pixels (256x256 equivalent);
- unchanged observations are addressed by IDs and do not need retransmission;
- recording video is never part of an agent observation.

## Artwork MCP tool surface

Six broad artwork tools are exposed so Codex does not carry a large catalog of micro-tools.

### `pixelforge_task`

Actions: `begin`, `get`, `accept`, `reject`, `abort`, `finish`.

The **agent owns semantic scope**. PixelForge never guesses whether the user's request is appropriate for pixel art. `accept` selects canvas dimensions. `reject` is used before editing for an incompatible final medium. `abort` is used after acceptance only for a concrete technical blocker. `finish` requires the current document revision.

`get` supports `known_task`, `known_revision`, and `known_state`; when all still match, the response is a compact `unchanged` result instead of repeating the prompt/references.

### `pixelforge_edit`

Applies one atomic patch at an expected revision.

Patch grammar:

```text
P,x,y,c                 single pixel
H,x,y,len,c             horizontal run
V,x,y,len,c             vertical run
R,x,y,width,height,c    filled rectangle
L,x0,y0,x1,y1,c         integer line
```

`c` is either a palette index or exact `#AARRGGBB`.

One call can carry up to 20,000 operations. If any operation is malformed or invalid/out-of-bounds, the complete transaction is cancelled. A successful batch with effective changes increments the document revision once. A net no-op succeeds with `changed_pixels:0` and leaves revision unchanged. Requests are limited to 8 MiB per JSONL line.

### `pixelforge_view`

Actions: `render`, `content_reference`, `style_reference`, `inspect`.

`render` accepts a canvas crop and integer scale. Render cache keys include task ID, revision, crop, scale and pixel content. The returned observation ID can be passed back as `known_observation`; unchanged images then return metadata only.

Reference actions observe the original loaded snapshot but the transport-delivered PNG is capped by total area, not side length: images with `width * height <= 65536` are sent unchanged; larger images are proportionally reduced until the delivered area is at most 65,536 pixels. Thus `256x256` is unchanged, while `512x512` is reduced. Canvas renders are not subject to this reference cap. References are optional. `inspect` returns row-major run-length encoded exact pixels and is capped at 4096 pixels.

### `pixelforge_palette`

Actions: `get`, `set`.

Palette values are comma-separated `AARRGGBB` colors, up to 256 entries. The palette is an agent-side compression dictionary; it never quantizes existing artwork.

### `pixelforge_history`

Actions: `undo`, `redo`. Both require task ID + expected revision.

### `pixelforge_io`

Exposes lossless native-resolution PNG `export` with task/revision validation.

## Recording-control MCP

Automatic Generate exposes one additional tool, `pixelforge_record`, on a separate MCP server. Recording intent remains semantic: Codex uses it only when the user's prompt asks to record the work/session/process.

Actions:

- `start` — requires `task_id`; optional `fps` 1–60, default 30.
- `status` — returns recording boolean and frame count.
- `stop` — finalizes the local MP4.

Recording should start after the agent decides the task is in scope but before the first canvas mutation, and stop after final visual inspection. PixelForge automatically finalizes an active recording when the Codex turn ends as a safety net.

The recorder captures the visible PixelForge client area and writes H.264 MP4 under the local `recordings` directory. The recording MCP intentionally exposes **no** path parameter for agent-chosen output, no video-read operation, no frame/preview operation, and no video bytes. `pixelforge_view` cannot access recordings either.

## Revision discipline

Every edit/history command carries `task_id` and `expected_revision`. If the user manually changes the sprite while Codex is working, a stale command fails instead of overwriting newer work. Codex should inspect/re-render the relevant area and continue from the returned current revision.

## Recommended observation cadence

Do not render after every batch. Prefer: establish silhouette with a large batch; render whole sprite; refine anatomy/shading by region; render changed crops; use compact inspect for cleanup; final whole-sprite render; stop any requested recording; finish.

## Transport

PixelForge MCP uses newline-delimited JSON-RPC. The artwork server compacts internal formatting at the wire boundary so every response occupies one transport line. Automatic Generate adds a separate Codex App Server JSONL control channel above MCP. PixelForge owns that control channel; Codex-launched bridge processes own only their MCP stdio channels and contain no artwork/video state.

### Automatic session efficiency

Automatic tools retain the last observed task/revision outside the model. An
omitted expected_revision uses that observed revision; it never substitutes the
live document revision. Concurrent mouse edits still reject stale commands.
Manual MCP retains explicit expected_revision requirements.

Automatic pixelforge_edit accepts optional render_scale (1–32). A successful edit
can return its render in the same call; a rejected edit returns no image and leaves
revision unchanged. If rendering fails after a commit, render_ok is false while
ok stays true; do not replay the committed edit. Plain successful edits return
only ok, revision and changed_pixels.

Reference previews now have a 65,536-pixel area budget, retaining the full loaded
source locally. The automatic session supplies known observation IDs and cannot
resend identical reference images, even with resend_image=true. That option only
redelivers canvas renders. A changed loaded reference produces a new observation.
This avoids repeated delivery; it does not remove earlier images or tool history
from Codex's existing context. No fixed 10–20k context or token saving is promised.
