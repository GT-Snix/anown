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
   Random numbers from streams (`an_rng_*`) are drawn only while the script
   builds the scene. Anything random computed while rendering must come from
   `an_hash(seed, stream, counter)`, which is stateless. The preview may use the
   wall clock to choose WHICH frame to show; what a frame looks like never
   depends on it.
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
   (both have a depth-stencil buffer: the shadow cut needs stencil on the main
   target; offscreen targets get it from `ani_offscreen_make_ds`).
8. **Coordinates.** Scene pixels at the scene's own width/height: origin top-left,
   y down, rotation in radians, clockwise. Draw order is depth-first, siblings sorted
   by `(z, creation id)`.

9. **Timeline.** docs/timeline.md is the spec. Objects keep a BASE state; the
   state at frame N is always base + tracks sampled at N, computed fresh. Never
   write sampled values back into objects or carry state between frames.
   `tests/test_timeline.c` checks seek order independence; keep it passing.

10. **Strokes.** Every outline (rect, circle, line, arrow, free strokes) goes
    through `ani_stroke_emit` (`src/stroke.c`); never add a second outline path.
    Paths are flattened at object creation; rendering, the nozzle and sampling
    never write to objects. One exception: graph edges (`an_edge`) build their
    path each frame from their nodes' sampled state, in the mesh's frame arena,
    never on the object (docs/graphs.md). `tests/test_stroke.c` checks the stress set for
    NaNs, degenerate triangles, widths, spikes and gaps; after changing the
    triangulator, also look at `./build/examples/04_stroke_stress out.png`.
    See docs/strokes.md.

11. **Layers.** Rendering goes through `ani_renderer_prepare` (outside any pass:
    tessellate, then render and resolve isolated layers, deepest first) and then
    `ani_renderer_draw` (inside the main pass). Opacity is never baked into
    vertex colors: anything below opacity 1 is isolated and composited
    (premultiplied). Translucent paints are stencil-tested; opaque paints aren't.
    `tests/test_layers.c` checks overlap uniformity, group/nested opacity and
    forced-vs-direct against hand-computed colors. Perf ceiling: 12 ms worst
    frame for `examples/spike_frame_bench` (200 strokes + 50 shapes, 1080p).
    See docs/layers.md.

12. **Style.** Effects scale with effective chaos; exact-pixel tests set
    `an_set_chaos(s, 0)` or turn the specific effect off in `an_style`. The GPU
    `an_hash` (`shaders/hash.glsl`) must stay bit-identical to the CPU one:
    `tests/test_gpu_hash.c` checks 4,096 inputs including edge values on Metal.

13. **Touching shapes that fade together must be grouped.** Each separately faded
    object is composited from its own resolved layer, so where two shapes share an
    anti-aliased edge, paper shows through as a pale seam the moment both are
    below opacity 1 (measured: the seam's green goes from 120 to 140 at opacity
    0.995). Put them in one group and fade the group: one layer, no seam. See
    docs/layers.md "Conflation seam" and the fade-sweep PNGs from `test_layers`.

14. **Text.** docs/text.md is the spec. Glyph outlines are flattened once per
    (font, glyph) and drawn through the same pipeline as shapes; fills are
    stencil-then-cover, non-zero (stencil bits: `0x80` silhouette, `0x70`
    write-once marks, `0x0F` winding). Letters of one text batch into one
    draw. `tests/test_text.c` takes every expected value from the fonts' own
    tables; keep it that way. The fonts live in `assets/fonts/` with their
    licenses and are embedded by `src/fonts.S`.
15. **No-op changes are proven, not assumed.** A change that must not alter
    output (a dependency bump, a refactor) is checked with
    `scripts/ref-frames.sh` before and after (identical PNG bytes), plus a
    targeted check when the reference frames don't exercise the changed code.

16. **Tessellation cache.** `src/tess.c` replays an object's vertices and
    commands when its key (object identity + everything sampled that the
    emission reads) repeats. Anything new that an emission reads must go into
    the key (`emit_memo`, `emit_glyphs_memo`). `tests/test_memo.c` requires
    identical meshes and pixels with the cache on and off in forward, reverse
    and shuffled order and under eviction; keep it passing, and break a key
    field on purpose when you change the key to see that it fails.

17. **Git.** Commit when asked. **Never push without asking first**, every time:
    approval for one push doesn't cover the next. The remote is
    https://github.com/GT-Snix/anown (private).

## Build

- One `Makefile`, no CMake. `make` builds the library, the examples and the tests.
  `make run EX=00_hello`, `make test`, `make clean`, `make deps`.
- Clean build must stay under about 5 seconds. Time it with
  `scripts/time-clean-build.sh`, which builds in a throwaway copy of the tree; never
  run `make clean` in the real tree just to time it.
  If it gets slower, find out why before adding more code. The Makefile sets
  `-j<ncpu>`. The vendor objects (`src/sokol_impl.m` 2.4 s, `src/stb_impl.c`
  1.0 s) are cached in `.vendor-cache/` (gitignored, kept by `make clean`,
  keyed by the vendor headers + impl file + flags), so a clean build is ~3 s;
  `scripts/time-clean-build.sh --cold` times it without the cache (~4.5 s).
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
                     00_hello 01_shapes 02_timeline 03_strokes 04_stroke_stress
                     05_style 06_look 07_palettes 08_text
tests/               headless tests (no window, but some need a Metal device)
                     each test is a program; shared helpers in tests/check.h
vendor/              single-header deps (sokol, stb_image_write, stb_truetype)
tools/               downloaded binaries (sokol-shdc), not committed
scripts/             fetch-deps.sh, bench.sh (frame bench, 5 runs per scenario,
                     refuses on a busy CPU), time-clean-build.sh, ref-frames.sh
assets/fonts/        the two built-in fonts (OFL), licenses, sources
out/                 exports and PNG frames (gitignored; make clean never touches it)
docs/experiments/    tried and not adopted, with measurements (e.g. vertex24)
docs/progress.md     milestone log: what works, what is stubbed
build/               all outputs (gitignored)
```

## Look

- Background is cream paper, `#F3EDE2` (`AN_CREAM`).
- The ink palette, outlines, grain and misregistration effects get designed in a
  later milestone. Until then, don't add them ad hoc.
