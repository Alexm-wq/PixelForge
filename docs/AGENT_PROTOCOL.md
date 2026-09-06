# Agent protocol (planned MCP surface)

The C++ core already models the lifecycle that the MCP bridge will expose.

## Task lifecycle

### `task.get`
Returns the active task without retransmitting image data unnecessarily.

Compact response shape:

```json
{
  "task_id": 7,
  "state": "awaiting_agent",
  "prompt": "64x64 ant-queen creature...",
  "canvas": null,
  "revision": 0,
  "content_reference": {"present": true, "observation": "ref:content:7"},
  "style_reference": {"present": true, "observation": "ref:style:3"},
  "limits": {"max_width": 2048, "max_height": 2048, "max_pixels": 4194304}
}
```

### `task.accept`
Agent semantic decision that the prompt is in scope.

```json
{"width": 64, "height": 64}
```

The editor validates only the requested dimensions/resource limits, creates the canvas, and returns the new revision.

### `task.reject`
Agent semantic decision to terminate without editing.

```json
{"reason": "PixelForge produces pixel-art assets; this task requests a photorealistic painting."}
```

This is a terminal task state. No canvas is created.

### `task.finish`
Marks an accepted task complete.

```json
{"summary": "Finished 64x64 queen sprite."}
```

## Editing surface

The intended compact MCP tool set is:

- `task` — get / accept / reject / finish
- `inspect` — metadata, palette, region, lint, revision deltas
- `render` — cached visual observation of whole canvas or crop
- `edit` — semantic batched primitives
- `patch` — compact indexed pixel-run patches
- `history` — checkpoint / undo / redo / compare
- `io` — open / save / export

The bridge should not expose hundreds of micro-tools. Batchability and observation caching are first-class requirements.

## Validation boundary

**Agent:** semantic scope, prompt interpretation, canvas-size choice, art decisions, completion/refusal.

**Editor:** bounds, file decoding, dimensions, coordinate validity, resource caps, transactional consistency, revision conflicts.
