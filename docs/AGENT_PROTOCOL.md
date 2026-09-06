# PixelForge agent protocol

PixelForge supports two MCP hosting paths.

## Automatic Generate path

Normal GUI launch:

```text
PixelForge.exe
```

The user loads references, enters a prompt, and clicks **Generate with Codex**. PixelForge then starts a local Codex App Server turn automatically. The Codex process is given only a temporary PixelForge MCP configuration that launches:

```text
PixelForge.exe --bridge <private named pipe>
```

`--bridge` is headless and stateless. It forwards MCP stdio to the already-running editor over the named pipe. The MCP server on the GUI side operates on the exact same live `PixelDocument` shown to the user.

No global PixelForge MCP configuration is required for this automatic path.

Each Generate operation uses a fresh ephemeral Codex thread with the PixelForge agent contract injected as developer instructions. The automatic art turn uses a read-only filesystem sandbox and no approvals; artwork therefore goes through PixelForge tools rather than repository/source writes. The task-scoped App Server is terminated after the turn to release the bridge and old observation/tool context.

## Manual MCP path

For development/debugging, PixelForge can still be launched by any stdio MCP client as:

```text
PixelForge.exe --mcp
```

The native GUI remains visible. MCP and mouse editing operate on the same revisioned `PixelDocument` in that process.

## Design rule

Agent efficiency is optimized without reducing artwork fidelity:

- the canvas remains full 32-bit ARGB;
- edits are exact deterministic pixel operations;
- palette indices compress the wire format only;
- any color can always be supplied as exact `#AARRGGBB`;
- renders are lossless PNG with nearest-neighbor integer scaling;
- content/style references are returned at original loaded resolution;
- unchanged observations are addressed by IDs and do not need retransmission.

## MCP tool surface

Only six broad tools are exposed so Codex does not carry a large catalog of micro-tools.

### `pixelforge_task`

Actions:

- `begin`
- `get`
- `accept`
- `reject`
- `abort`
- `finish`

The **agent owns semantic scope**. PixelForge never guesses whether the user's request is appropriate for pixel art.

`accept` selects the canvas dimensions. PixelForge validates only hard limits.

`reject` is used before editing for a request outside the pixel-art editor's scope.

`abort` is used after acceptance only when a concrete technical blocker is discovered.

`finish` requires the current document revision.

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

Example:

```text
H,12,14,8,4;H,11,15,10,4;P,15,13,#FFFFFFFF;L,8,20,17,27,2
```

One call can carry up to 20,000 operations. If any operation is malformed or invalid/out-of-bounds, the complete transaction is cancelled.

A successful batch with effective changes increments the document revision once. A net no-op succeeds with `changed_pixels:0` and leaves the revision unchanged. `changed_pixels` counts unique pixels whose final value differs from their initial value, including when operations overlap. Requests are limited to 8 MiB per JSONL line.

### `pixelforge_view`

Actions:

- `render`
- `content_reference`
- `style_reference`
- `inspect`

`render` accepts a canvas crop and integer scale. Render cache keys include task ID, revision, crop, and scale. The returned observation ID can be passed back as `known_observation`; unchanged images then return metadata only.

`content_reference` and `style_reference` use the same observation-ID rule and return the loaded reference snapshot as lossless PNG at original resolution. Changing the file on disk does not change the loaded reference. Reference decoding is limited to 16384 pixels per dimension and 64 megapixels total.

`render` and `inspect` work for accepted and finished tasks; edits and history require an accepted task. Render scales are 1–32, limited to 16,777,216 output pixels. Render cache keys include pixel content as well as task/revision/crop/scale, so restarting the app cannot reuse a different document's cached image.

`inspect` returns row-major run-length encoded exact pixel values and is capped at 4096 pixels. Use visual renders for larger areas.

### `pixelforge_palette`

Actions:

- `get`
- `set`

Palette values are comma-separated `AARRGGBB` colors, up to 256 entries. The palette is an agent-side compression dictionary; it never quantizes existing artwork.

### `pixelforge_history`

Actions:

- `undo`
- `redo`

Both require task ID + expected revision.

### `pixelforge_io`

Currently exposes lossless native-resolution PNG `export` with task/revision validation.

## Revision discipline

Every edit/history command carries:

```text
task_id
expected_revision
```

If the user manually changes the sprite while Codex is working, a stale command fails instead of overwriting newer work. Codex should inspect/re-render the relevant area and continue from the returned current revision.

## Recommended observation cadence

Do not render after every batch.

Prefer:

1. establish silhouette with a large batch;
2. render whole sprite;
3. refine anatomy/shading by region with large patches;
4. render only changed crops;
5. use compact `inspect` for exact cluster cleanup;
6. final whole-sprite render;
7. finish.

This keeps visual quality high while avoiding unnecessary image/tool round-trips.

## Transport

The PixelForge MCP transport uses newline-delimited JSON-RPC. PixelForge compacts internal formatting at the wire boundary so every response occupies exactly one transport line.

The server supports the initialize/tools flow used by stdio MCP clients and also recognizes `server/discover`.

Automatic Generate adds a separate Codex App Server JSONL control channel above MCP. PixelForge owns that control channel; the Codex-launched `--bridge` process owns only the MCP stdio channel and contains no artwork state.
