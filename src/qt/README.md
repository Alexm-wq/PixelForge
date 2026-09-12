# PixelForge Qt

Portable C++/Qt editor and automation host using PixelForge's existing document,
agent-task, command and PixelProgram core. The original Windows sources are
unchanged. This is a local Linux review candidate; Windows/macOS builds and
complete platform parity have not been verified.

## Build and launch

Requires C++20, CMake, Qt 6.2+ Widgets/Network (Qt Test for checks). Session
recording uses an existing `ffmpeg` with libx264. Automatic art uses an existing
signed-in Codex CLI; no provider process starts until Generate.

```sh
cmake -S . -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DPIXELFORGE_BUILD_QT=ON -DPIXELFORGE_QT_TESTS=ON
cmake --build build/qt -j1
./build/qt/src/qt/PixelForgeQt [image.png]
```

`BUILD_TESTING=OFF` excludes the legacy Windows suite; `PIXELFORGE_QT_TESTS`
enables the separate portable cases. On macOS the normal target is an app
bundle. Linux was built using Qt 6.4.2 and GCC 13. The port uses Qt processes and
local sockets in place of Windows process/pipe APIs.

## Drawing, projects and animation

Start with a transparent 16×16 canvas or use New. Pencil (P), Eraser (E) and
Pick colour (I), swatches, full palette and custom RGBA selection edit actual
pixels. A drag is one undo transaction; Escape cancels it. Grid (G), zoom, Fit
and scrolling affect the view. Save PNG / Save PNG as write atomically; Export
PNG copy preserves the working filename. New/Open/Quit protect unsaved work.

Choose a group and named canvas/frame above the canvas. Add Canvas creates an
additional frame without replacing others. Play frames shows an actual animated
preview of the selected group. File > Export project supports PNG canvases,
sheets/strips/timelines and animated GIF. Generated tools can also create groups,
copy/clone, inspect, analyze and run atomic multi-canvas programs/passes.

File > Save project writes a version-1 `project.json` and immutable PNG assets.
File > Open project restores names, groups, frame order, pixels and accepted
project orientation. This reads the existing Windows PNG-manifest format and
retains optional Qt metadata. Unchanged assets are reused; failed publication
retains the old manifest/assets. Undo history is session-local. PNG saving is
not whole-project acceptance. Positional launch arguments open raster images;
use File > Open project for manifests.

Source/content/style references are independent of the open document. Choose
image loads one frame of an installed supported raster format; metadata reports
the decoder and first-frame limitation. PNG/JPEG/BMP/GIF and any installed
TIFF/WebP/ICO reader are supported; SVG and executable content are excluded.

## Generate and review

Enter the art request, select an available model/effort, then Generate. The
installed login is verified and requested/effective model, effort and read-only
sandbox are checked before one original turn. The tool controller edits the
same document, returns actual rendered images, and retains task/revision guards.
Tool progress and failures are shown without fabricated completion. The agent
can ask a question through the inline answer panel. Stop interrupts the original
turn and retires owned processes; it preserves committed pixels and does not
retry. No default total-art timeout is imposed; negotiation is bounded.

The agent's Finish produces **awaiting human review**. Accept artwork saves the
project; Request changes starts a user-requested revision that preserves the
existing artwork. It is distinct from automatic success or PNG export.
`PIXELFORGE_CODEX_EXE`, then `CODEX_CLI_PATH`, then the installed/PATH CLI resolve
the executable. No credentials are copied into the project.

## Optional local MCP and recording

Automation > Enable local MCP exposes a user-only local socket for this editor.
The dialog supplies its exact endpoint and command:

```sh
./build/qt/src/qt/PixelForgeQt --mcp --socket '<shown instance name>'
```

This is a real stdio MCP client to the enabled editor, not a second canvas.
Initialize/tools/list/tools/call reach the current guarded controller. Manual
MCP adds task.begin; it refuses concurrent in-app generation. No TCP listener
or persistent external client configuration is installed. Disable closes it.

Recording requires the user's explicit Automation > Allow recording choice.
Start/status/stop are available manually and to the current task. A real MP4 is
published only after ffmpeg successfully finishes. Stop/finalize can be pending;
no premature saved result is returned. Only the owned editor's supplied pixels
are captured, never the desktop or other applications. This differs from the
Windows desktop-rectangle capture of overlaid dialogs. Resize is letterboxed;
H264 is lossy, alpha is composited on black, and queue drops are reported.

## Agent-selectable navigation and limits

Agent animation/timeline observations are explicitly labelled ordered PNG frame
strips, matching the Windows tool contract. Native Play frames and GIF exports
remain animated; no model moving-image perception is claimed.

The agent chooses broad or filtered index/list/analyze, compact or richer legacy
metadata, full supported images or explicit crops/scales, and fresh bytes or
known-observation reuse. There is no mandatory search/preview sequence. Offset
continuations require the returned pack revision; stale pages are refused.
`changed_since` respects the stated tracking floor. A matching `known_observation`
explicitly requests suppression; `resend_image:true` forces fresh bytes.

Project storage supports at most 4,096 canvases and 64 Mi pixels total. The editor
supports 4 Mi pixels per selected image and 4,096 per edge; loading is eager,
not unbounded or lazy. Metadata pages are bounded to 8 KiB with continuation;
image/inspection/program/recording limits are reported explicitly. PNG preserves
alpha; GIF supports one transparent index and may quantize colors/partial alpha.
Content-addressed old assets are retained, not garbage-collected automatically.
Agent exports require a new destination and reject links in every path component,
preserving saved manifest assets. Explicit trusted UI export can replace a file
chosen by the user; no model field grants that replacement capability.
The file store coordinates cooperating processes, not hostile same-user directory
replacement. Recording's atomic publication needs same-filesystem hard links.

Remaining platform differences include untested Windows/macOS Qt deployment,
Windows-specific integration/capture behavior. Both targets share the same
Windows compiler transformation. Qt adds explicit host limits:262,144 emitted
operations/16MiB expanded patch,16,384 named masks/256MiB live plus staged mask
storage,1,024-character mask names,65,536 palette entries,64Mi touched-pixel
transaction work and4Mi pixels per compiler/COPY temporary canvas. The total
process can use additional image/history/raster memory;256MiB is not an RSS
guarantee. Oversized work is refused before document mutation. No new general layer or
animation-authoring model is invented beyond the existing named frame/group
workflow. Consult the accompanying implementation and independent QA reports
for exact tested behavior, first failures and remaining limitations.
