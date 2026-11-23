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
   `an_window_run(&(an_window_desc){ .title = "hi", .clear = AN_CREAM });`
   Every field must have a sensible zero default.
4. **Public API prefix.** Everything public is `an_` (functions and types) or `AN_`
   (macros and constants). Everything public lives in `include/anown.h`.
   Internal symbols are `static` or live in `src/*_internal.h`.
5. **Milestones.** Every milestone ends with
   - a runnable example in `examples/NN_name/` that `make` builds, and
   - an entry in `docs/progress.md` saying what works and what is stubbed.
   A milestone isn't done until both exist and the example runs.

## Build

- One `Makefile`, no CMake. `make` builds the library, the examples and the tests.
  `make run EX=00_hello`, `make test`, `make clean`, `make deps`.
- Clean build must stay under about 5 seconds. Check with `make clean && time make`.
  If it gets slower, find out why before adding more code.
- Our code compiles with `-std=c11 -Wall -Wextra -Wpedantic -Werror`. Vendor headers
  are included via `-isystem` so their warnings don't break the build. The sokol
  implementation TU (`src/sokol_impl.m`) is Objective-C (required on macOS) and is the
  only file built without `-Werror`.
- The toolchain is Apple clang from the Xcode Command Line Tools
  (`xcode-select --install`); the macOS SDK is needed for Metal/AppKit.

## Layout

```
include/anown.h      public API (the only header users include)
src/                 library implementation (+ sokol_impl.m)
shaders/             sokol-shdc annotated GLSL
examples/NN_name/    one directory per runnable example, main.c inside
tests/               headless tests (no window), run by `make test`
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
