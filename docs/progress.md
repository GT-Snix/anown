# Progress

## M0: skeleton (2026-11-23)

Example: `make run EX=00_hello`

What works:
- Makefile builds `build/libanown.a`, every `examples/*/main.c`, and every
  `tests/test_*.c`.
- `an_window_run()` opens a 1280x720 high-DPI window (sokol_app, Metal) and clears
  it to cream `#F3EDE2` every frame.
- `an_hex()` / `AN_HEX()` / `AN_CREAM` colors, plus a headless test (`make test`).
- sokol-shdc is fetched into `tools/`, and the Makefile has a rule that compiles
  `shaders/*.glsl` to Metal headers. There are no shaders yet.

Stubbed / not started:
- Scenes, frame-index timeline, seeded RNG, arena allocator: not started. These are
  rules in CLAUDE.md, but there is no code yet.
- No drawing of any kind beyond the clear.
- No offscreen rendering or video export.

Dependencies: sokol `09558a1` (2026-11-21), sokol-tools-bin `d4c808b` (2026-11-23).

## Spike: offscreen render -> readback -> ffmpeg (2026-11-23)

Example: `./build/examples/spike_export [out.mp4] [--frames N] [--no-encode]`

What works:
- Headless: `sg_setup` with our own `MTLCreateSystemDefaultDevice()`. No window
  and no sokol_app.
- 1920x1080 RGBA8 offscreen target, a pink quad whose position depends only on the
  frame index, drawn with a sokol-shdc shader (`shaders/spike_quad.glsl`). This is
  the first real use of the shader build rule.
- Readback: sokol_gfx has no GPU->CPU API. After `sg_commit()`, the spike gets the
  `MTLTexture` via `sg_mtl_query_image_info()` and blits it into a shared
  `MTLBuffer` on sokol's own queue (`sg_mtl_command_queue()`), then calls
  `waitUntilCompleted`. Ordering is safe because sokol enqueues its frame command
  buffer when it acquires it. Rows come out top-down, matching ffmpeg's raw input.
- ffmpeg via `popen()`: libx264, CRF 18, preset medium, yuv420p, tagged BT.709
  limited range. Decoded pixels: background f3ede3 (target f3ede2), quad ff48b0
  (exact).
- Deterministic: two runs produce byte-identical mp4 files.

Measured on M4, 300 frames:
- render + readback alone: 1.1 ms/frame readback (7.4 GB/s), ~875 fps.
- with encoding: ~208 fps end to end (3.5x realtime). libx264 is the limit, not
  readback.

Stubbed / spike-only:
- Readback is synchronous: the CPU waits for the GPU every frame. That's fine at
  these numbers.
- No audio, no error handling beyond the basics, quad shader is throwaway.

## Offscreen module promoted into the library (2026-11-24)

Example: `./build/examples/spike_export` (now built on the module). Test: `make test`.

What works:
- `src/offscreen.m` + `src/offscreen_internal.h` (internal, `ani_` prefix): headless
  Metal setup, autorelease pool push/pop, RGBA8 offscreen targets with their own
  shared staging buffer, and blocking readback into tightly packed top-down RGBA8.
- `tests/test_readback.c` renders two known 64x32 frames and checks every byte.
  I confirmed it fails on three broken versions of the readback: a vertical flip,
  an R/B channel swap, and stale data from skipping the GPU copy.
- The spike's quad shader is now `shaders/solid_rect.glsl`, shared by the test and
  the spike.
- Same performance as before: readback ~1.3 ms/frame, ~209 fps end to end with
  encoding.
- Fixed: make was deleting generated shader headers as intermediates, which caused
  rebuilds. They're now `.SECONDARY`.

Stubbed:
- Nothing public uses the module yet. There's no `an_` export API.
- Readback is still synchronous.
- `test_readback` needs a Metal device, so it won't run on a GPU-less CI runner.

## M1: core (2026-11-24)

Example: `./build/examples/01_shapes` (preview window) or
`./build/examples/01_shapes --export out.mp4` (2 s of video).

What works:
- Arena allocator (`src/arena.c`): chunked, zeroed, aligned allocations; reset
  keeps blocks; oversized allocations get their own block. `tests/test_arena.c`.
- Scene (`an_scene_create`): width, height, fps, seed, background, arena, and a
  hidden root group. Objects are allocated from the arena and freed with the scene.
- Objects: rect, circle, line, text, group. Each has pos, scale, rotation, z, parent,
  fill, stroke and stroke width. Options structs share fields via `AN_OBJ_FIELDS`.
  Reparenting rejects cycles and objects from other scenes. `tests/test_scene.c`
  covers defaults, the tree, world transforms, draw order and deterministic
  tessellation.
- Renderer: CPU tessellation into one triangle list, then one draw call. Preview and
  export share `ani_renderer_draw()`. The preview window is letterboxed and uses 4x
  MSAA; the export also uses 4x MSAA and reads back from the resolve image.
  `tests/test_render.c` checks pixel positions, z order, group transforms and
  byte-identical repeat renders. `test_readback` now also covers 4x MSAA.
- Determinism: two exports of 01_shapes are byte-identical mp4 files.
- Export quotes the output path for the shell (tested with an apostrophe) and
  ignores SIGPIPE while writing, so a missing ffmpeg is an error, not a crash.

Stubbed / not started:
- Text objects are created and positioned but not drawn (no font rendering).
- Stroke is drawn only for lines; rect and circle outlines aren't drawn yet.
- No animation: the frame index exists in preview and export but nothing reads it.
  The seed is stored, but there is no RNG API yet.
- Preview window visuals were not checked by me (no screen capture permission). It
  runs without validation errors.
- No test covers the swapchain path. The depth-format mismatch I hit there was only
  caught by running the preview.

Build: clean build went to 6.4 s serial and is 3.0 s with `-j` (now the default).
