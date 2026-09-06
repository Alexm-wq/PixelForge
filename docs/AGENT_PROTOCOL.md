# Agent protocol

The C++ core exposes a stable typed command router in `src/core/AgentCommands.*`. The future MCP/stdio adapter should remain thin and map wire commands directly onto this surface.

## Task lifecycle

### `task.get`
Returns the active task, original user prompt, reference metadata, current revision, state, canvas metadata, and hard editor limits.

### `task.accept`
Agent semantic decision that the prompt is in scope.

```json
{"task_id":7,"width":64,"height":64}
```

The editor does not decide whether the prompt is pixel art. It validates only the concrete canvas request and resource limits, creates the canvas, and returns the new revision.

### `task.reject`
Agent semantic decision to terminate before editing.

```json
{"task_id":7,"reason":"PixelForge produces pixel-art assets; this task requests a photorealistic painting."}
```

This is a terminal task state. The reason is agent-authored and user-facing.

### `task.abort`
Technical failure discovered after the agent already accepted the task.

```json
{"task_id":7,"reason":"The requested explicit canvas exceeds the active editor resource limit."}
```

`abort` is not a second semantic classifier. It exists so an accepted task can terminate cleanly if a concrete blocker is discovered later.

### `task.finish`
Marks an accepted task complete. It is revision-guarded so the agent cannot finish a sprite after the user or another client changed it behind its back.

```json
{"task_id":7,"expected_revision":18,"summary":"Finished 64x64 queen sprite."}
```

## Editing commands

All agent edits are atomic and revision-guarded:

```json
{
  "task_id":7,
  "expected_revision":18,
  "ops":[
    {"kind":"horizontal_run","x":12,"y":20,"width":14,"argb":"ff36505a"},
    {"kind":"line","x":19,"y":10,"x2":25,"y2":18,"argb":"ff78aab0"}
  ]
}
```

Implemented primitive kinds:

- `set_pixel`
- `fill_rect`
- `horizontal_run`
- `vertical_run`
- `line`

A batch is all-or-nothing. One malformed/out-of-bounds operation cancels the entire transaction. A successful batch increments the document revision once regardless of how many pixels changed.

## Inspection and history

The router also exposes:

- region inspection at an expected revision
- undo at an expected revision
- redo at an expected revision

These use the same task-id/revision guards as editing.

## Concurrency rules

Every mutating agent command targets both a `task_id` and, after acceptance, an `expected_revision`.

- `stale_task`: the user started/replaced the task; the agent must stop operating on the old task.
- `stale_revision`: the document changed since the agent last observed it; the agent must refresh before editing.

This is intentional. PixelForge may be open while the user manually edits the same canvas.

## Intended compact MCP surface

The final adapter should expose only a few coarse tools:

- `task` — get / accept / reject / abort / finish
- `inspect` — metadata / region / palette / lint / revision deltas
- `render` — cached visual observation of full canvas or crop
- `edit` — semantic batched primitives
- `patch` — compact indexed pixel-run patches
- `history` — checkpoint / undo / redo / compare
- `io` — open / save / export

Do not expose one tool per pixel operation. Batchability and observation caching are first-class requirements.

## Validation boundary

**Agent:** semantic scope, prompt interpretation, canvas-size choice, art decisions, completion/refusal.

**Editor:** bounds, file decoding, dimensions, coordinate validity, resource caps, transactional consistency, revision conflicts.
