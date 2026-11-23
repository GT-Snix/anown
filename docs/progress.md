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
- The Metal glue lives in `examples/spike_export_mtl.m`, not in the library.
- No audio, no error handling beyond the basics, quad shader is throwaway.
