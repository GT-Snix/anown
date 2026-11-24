# anown

An animation library in C11 for making explainer videos, in the spirit of Manim,
but with a loud street-art / risograph / zine look instead of dark minimalism:
cream paper, flat ink colors, heavy outlines, misregistration, grain.

## Platform and dependencies

- Target macOS on Apple Silicon (M4) first. Other platforms come later, if ever.
- Rendering uses **Metal** through `sokol_gfx` + `sokol_app` (`SOKOL_METAL`).
  **Do not use OpenGL**, not even as a fallback or for a quick test.
- Shaders are written in sokol-shdc's annotated GLSL (`shaders/*.glsl`) and compiled
  with `tools/sokol-shdc -l metal_macos` into generated headers under `build/`.
  Never hand-write MSL, and never commit generated shader headers.
- `src/sokol_impl.m` must define `SOKOL_NO_ENTRY` before the sokol_app implementation.
  anown is a library and user programs own `main()`; `an_window_run()` calls
  `sapp_run()` itself. Without it, sokol_app defines its own `main()` and the link
  fails with `Undefined symbols: _sokol_main`.
- Use the **latest version of every dependency**. Before using or changing a sokol
  API, read the header in `vendor/` or the current upstream docs. Do not rely on
  memory: sokol's API changes often (for example, the pass/swapchain and view APIs).
- Dependencies are pinned by commit SHA in `scripts/fetch-deps.sh`. To upgrade, bump
  the SHAs to upstream HEAD, run `make deps`, rebuild, run every example, and record
  the bump in `docs/progress.md`.
- Only single-header (or single-file) deps, vendored in `vendor/`. No package managers.

## Scope (v1)

Visualizing algorithms, data structures, and simple math:
arrays, graphs, trees, axes/plots, and code snippets.

Out of scope for v1: LaTeX, 3D, audio, a GUI editor, and scripting-language bindings.

## Hard rules

1. **Determinism.** Time is a frame index (`int64_t frame`), never wall-clock.
   Do not call `time()`, `clock()`, `sapp_frame_duration()` or similar to drive
   animation. All randomness comes from a seeded RNG owned by the scene; no `rand()`,
   no global RNG state. The same input must produce byte-identical frames.
   Avoid anything order-dependent that isn't specified, such as hash iteration order
   or pointer comparisons.
2. **Memory.** Each scene owns an arena allocator. Every object is allocated from it
   and freed all at once when the scene is destroyed. Users never free individual
   objects, and the public API has no per-object `free`/`destroy`.
3. **API style.** Options are passed as structs using C11 designated initializers and
   compound literals, with zero meaning "default":
   `an_rect(s, &(an_rect_opts){ .pos = {960, 540}, .size = {200, 100} });`
   Every field must have a sensible zero default, and NULL opts means all defaults.
   Shared object fields come from the `AN_OBJ_FIELDS` macro so they stay flat.
   Setters (`an_obj_set_*`) take values literally; only opts apply defaults.
4. **Public API prefix.** Everything public is `an_` (functions and types) or `AN_`
   (macros and constants). Everything public lives in `include/anown.h`.
   Internal symbols are `static` or live in `src/*_internal.h` with the `ani_`
   prefix, so they never collide with future public `an_` names. Tests and spikes
   may include internal headers; `examples/NN_name/` may not.
5. **Milestones.** Every milestone ends with
   - a runnable example in `examples/NN_name/` that `make` builds, and
   - an entry in `docs/progress.md` saying what works and what is stubbed.
   A milestone isn't done until both exist and the example runs.
6. **Readback is guarded.** `src/offscreen.m` reads pixels back with native Metal,
   because sokol_gfx has no readback API. It relies on sokol's command buffer
   ordering. `tests/test_readback.c` checks exact pixels; after any sokol upgrade it
   must pass before anything else lands.
7. **One draw path.** Preview and export both draw through `ani_renderer_draw()`
   (`src/render.c`), fed by the CPU tessellator (`src/tess.c`). Never add
   output-specific drawing. Only the pass, its formats and the sample count differ
   (the preview swapchain has a depth buffer, offscreen targets don't).
8. **Coordinates.** Scene pixels at the scene's own width/height: origin top-left,
   y down, rotation in radians, clockwise. Draw order is depth-first, siblings sorted
   by `(z, creation id)`.

## Build

- One `Makefile`, no CMake. `make` builds the library, the examples and the tests.
  `make run EX=00_hello`, `make test`, `make clean`, `make deps`.
- Clean build must stay under about 5 seconds. Check with `make clean && time make`.
  If it gets slower, find out why before adding more code. The Makefile sets
  `-j<ncpu>`; `src/sokol_impl.m` alone takes ~2.5 s and everything else overlaps it.
- Our code compiles with `-std=c11 -Wall -Wextra -Wpedantic -Werror`. Vendor headers
  are included via `-isystem` so their warnings don't break the build. Our
  Objective-C (`src/*.m`, Metal glue) also uses `-Werror`. The sokol implementation
  TU (`src/sokol_impl.m`) is the only file built without it.
- Keep Objective-C confined to `src/*.m` behind a C header. Public headers, examples
  and tests are plain C11.
- The toolchain is Apple clang from the Xcode Command Line Tools
  (`xcode-select --install`); the macOS SDK is needed for Metal/AppKit.

## Layout

```
include/anown.h      public API (the only header users include)
src/                 library implementation (+ sokol_impl.m, offscreen.m)
shaders/             sokol-shdc annotated GLSL
examples/NN_name/    one directory per runnable example, main.c inside
examples/spike_*.c   throwaway experiments; may use internal headers
tests/               headless tests (no window, but some need a Metal device)
                     each test is a program; shared helpers in tests/check.h
vendor/              single-header deps (sokol)
tools/               downloaded binaries (sokol-shdc), not committed
scripts/             fetch-deps.sh
docs/progress.md     milestone log: what works, what is stubbed
build/               all outputs (gitignored)
```

## Look

- Background is cream paper, `#F3EDE2` (`AN_CREAM`).
- The ink palette, outlines, grain and misregistration effects get designed in a
  later milestone. Until then, don't add them ad hoc.
