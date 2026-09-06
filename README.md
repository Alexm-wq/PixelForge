# PixelForge

Agent-native pixel-art editor for Codex.

PixelForge is a native C++ pixel editor designed around a compact agent protocol. The GUI is for the user; the agent edits the same document through structured commands rather than GUI automation.

## Design goals

- Pixel-precise editing with deterministic operations
- Minimal agent round-trips and compact responses
- Content reference + style reference slots
- Revisioned document state and delta history
- Explicit agent task lifecycle (`accept`, `reject`, `finish`)
- Hard editor validation without semantic prompt policing
- Native Windows build with minimal dependencies

## Agent scope model

PixelForge does **not** decide whether a user prompt belongs in a pixel-art editor. The agent does that from its system prompt.

The editor only enforces technical invariants such as:

- valid canvas dimensions
- supported image formats
- coordinates within bounds
- valid palette/index data
- maximum configured resource limits

The agent may terminate without editing by issuing a task rejection with a user-facing reason.

## Status

Initial native C++ UI and document core are being built on `main`.
