# Large workspace audit

## Agent navigation

Use `pixelforge_task get` to identify the task, then:

```json
{"action":"list","task_id":123,"summary":true,"limit":256}
{"action":"analyze","task_id":123,"summary":true,"limit":256}
{"action":"analyze","task_id":123,"group":"walk"}
{"action":"view","task_id":123,"group":"walk","mode":"animation","limit":16}
```

Replace the task and group with returned values. Summary pages contain one row per
group; detailed pages contain one row per frame. `next_offset=-1` means complete.
Default list/analysis pages contain 64 rows, with at most 256 rows per call.
Detailed analysis provides bounds, visible pixels, centroid, color count, pixel
hash and adjacent-frame differences. Group summaries report empty/duplicate
frames, dimensional differences, occupancy range and the largest transition.
Loop comparisons include last-to-first within the selected group. These are
measurements for triage, not proof of artistic quality or unwanted motion.

Views default to 32 frames, allow up to 128, and allocate a delivered preview of
at most 1,048,576 pixels and 2048 pixels per edge. They may downsample large frames.
Pagination metadata and exact frame names identify what was shown. An unchanged
view returns no image; `resend_image:true` requests it explicitly. The Codex
transport forwards this observation once, without silently adding every frame
as another image. Animation observations are ordered temporal samples plus FPS;
actual animated GIF encoding is performed for export.

Pack inspect returns row-major RLE pages inside the clipped requested rectangle:
`pixel_offset`, `pixel_limit` (default 1024, maximum 4096), `returned_pixels` and
`next_pixel_offset`. This bounds text output even on very large canvases.
Full-resolution exports are not paginated or downsampled by observation budgets.

## Correctness and persistence fixes

- Accept closes its manifest read stream before atomic replacement on Windows.
- Autosave writes changed frames via adjacent temporary files; manifest writes
  are flushed and closed before replacement. Failed persistence is reported
  separately from the already-committed edit, with `autosave_ok:false`.
- `.autosave-pending` prevents acceptance of a partially saved pack. Retry with
  `pixelforge_pack save`; do not repeat the drawing. Existing manifest bytes are
  preserved when a destination is locked. This is a detectable partial-save
  boundary, not a filesystem-wide transaction over every canvas file.
- Unchanged canvases are not re-exported/redecoded after each edit. Only affected
  animation strips and UI frame snapshots are regenerated.
- Multi-frame undo includes only frames that changed in that pass, preserving
  older edits on no-op frames.
- Project loading restores raw pixels without large textual patches or fake
  per-pixel undo history. Transparent pixels retain their RGB bits.
- Opening an external project with a colliding task directory chooses a new
  working ID rather than deleting the existing project. Copying into a source
  subdirectory is rejected. Invalid dimensions and duplicate/path-like names
  are rejected before replacing the live project.
- Pack/pass edits count as real drawing progress for the automatic client.

## Verification and limits

`pixelforge_large_workspace_tests` uses 200 animation groups with 8 frames each
(1,600 canvases at 64×64). It checks bounded listings/reviews, cached analysis,
one-frame autosave, no-op autosave, failed-save recovery, exact raw restore,
undo correctness and inspection paging. It prints timings and response sizes.
`pixelforge_project_persistence_tests` exercises the actual acceptance boundary
and Windows file locking. The fake app-server test checks that animation view
produces one model-visible image, without real model calls.

These tests establish native tool behavior, not an end-to-end model token budget
or guaranteed art quality. Initial save still writes every frame. Pixel buffers,
undo history and UI snapshots remain in RAM; there is no disk-backed lazy frame
store. Very high-resolution projects need separate memory profiling. Numeric
consistency checks should be followed by targeted visual review.

Measured on this Windows workspace, Release build, 2026-09-09 (mostly empty
synthetic frames with sparse edits):

| Operation | Result |
| --- | --- |
| Create and initially save 1,600 64×64 frames | 23,128 ms |
| Edit and save one frame in that workspace | 65 ms; one PNG rewritten |
| Cached analysis of one 8-frame group | 38 µs; no pixel rescans |
| Overview analysis of all 200 groups | 17 ms; 4,756 bytes of text |
| Default frame listing page | 2,585 bytes of text |
| Repeated unchanged preview | Zero image bytes |

Detailed/noisy art can increase PNG sizes and palette-analysis work. These are
local measurements for the synthetic fixture, not guaranteed timings for every
project or a measured model-token reduction. All nine CTest suites passed; no
paid drawing session was run.
