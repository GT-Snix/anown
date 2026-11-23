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
