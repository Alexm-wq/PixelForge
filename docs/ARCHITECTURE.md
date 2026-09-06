# PixelForge architecture

## Current prototype

- `pixelforge_core`: portable C++20 document and task-state library.
- `PixelForge` (Windows): dependency-free Win32/GDI/WIC UI.
- Two cached reference slots: content reference and style reference.
- Native PNG import for references and PNG export for the canvas via Windows Imaging Component.
- Revisioned pixel document with batch transactions and delta-based undo/redo.
- Task state machine with explicit agent accept/reject/finish actions.

## Target architecture

```text
Codex
  | MCP / stdio
  v
pixelforge-agent-bridge
  | compact commands + revisions
  v
PixelForge document core <--> native GUI
```

The GUI and agent bridge must operate the same in-memory/project document. GUI automation is not part of the agent path.

## Performance rules

1. One revision per batch transaction, not per pixel.
2. Responses return changed bounds/revision counts, not full pixel arrays unless explicitly requested.
3. Reference images are decoded once and retained in memory.
4. Future render observations are keyed by document revision + crop + scale + visible layers.
5. Future pixel patch transport uses palette indices and runs/rectangles rather than verbose per-pixel JSON.
6. Semantic prompt refusal remains an agent decision, never a local keyword classifier.
