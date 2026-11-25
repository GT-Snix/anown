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

## Seeded RNG (2026-11-24)

No new example: the RNG has nothing visible to show until the timeline exists.
Test: `tests/test_rng.c`.

What works:
- PCG32 (XSH-RR), checked against the reference pcg32 output for
  `srandom(42, 54)`.
- A stream for the scene (`an_scene_rng`) and one per object (`an_obj_rng`), each
  derived from `an_scene_desc.seed` and a key through SplitMix64. An object's key is
  a hash of its `.name` if it has one, otherwise its creation id.
- `an_rng_u32`, `an_rng_below` (no modulo bias), `an_rng_int` (inclusive),
  `an_rng_float` ([0,1), 24-bit), `an_rng_range`.
- Tests: same seed gives identical scene and object sequences; different seeds
  differ; drawing from one stream doesn't move another; appending objects doesn't
  change earlier ones; a named object keeps its stream when an object is inserted
  before it. I confirmed the tests fail when every object shares one stream.

Limitation (by design, documented): an unnamed object's stream depends on its
creation order, so inserting an object earlier in the script changes it. Naming
the object pins it.

Timeline: `docs/timeline.md` is written. The timeline is not implemented yet;
it's waiting on review.

## M2: timeline (2026-11-24)

Example: `./build/examples/02_timeline` (preview: Space pauses, arrows step) or
`./build/examples/02_timeline --export out.mp4` (712 frames, 0..711).
Spec: `docs/timeline.md`.

What works:
- The script records tracks at a cursor kept in integer microseconds. Frames
  appear only when sampling, using exact integer comparisons. Ten
  `an_wait(0.1)` at 24 fps are exactly 24 frames (tested).
- `an_play`, `an_play_async`, `an_wait`, `an_par`, `an_stagger`, `an_seek`,
  `an_obj_get`, `an_scene_length`, `an_scene_time`.
- Animatable: position, scale, rotation, opacity (multiplies down the tree), fill
  and stroke (OKLab with gamut clamp), z (steps). Easings: linear, cubic in-out,
  and an analytic spring for damping ratios from 0.01 up to critical.
- Take-over: the later-starting track wins. A new spring gets the analytic
  velocity of whatever it interrupts, so the handoff is smooth in position and
  velocity.
- Spring duration is its settle time, with tolerance max(1e-3 × travel,
  per-property floor). Its last frame is exactly the target.
- Setters record instant take-over tracks at the cursor (or write the base when
  that's equivalent). Objects appear at the cursor where they were created.
  Subtrees with zero effective opacity emit nothing.
- Tracks address a generic property handle (target kind + property), so scene
  and camera targets can be added without touching tracks or sampling.
- `an_hash` for render-time noise (stateless, values pinned by a test).
  Duplicate object names are rejected. The name hash is documented.
- Export renders frames 0..L inclusive and returns the frame count (-1 on failure).
- Determinism: two exports of 02_timeline are byte-identical.
- Tests: `test_ease` (golden easing, spring values at 30 and 60 fps computed with
  bc, settle, OKLab golden values and gamut clamp), `test_timeline` (cursor, ticks,
  seek order independence, take-over, setters, lifetime, opacity). I confirmed
  `test_timeline` fails if sampling writes into the base, or if the spring
  velocity handoff is dropped.

Stubbed / not started:
- Geometry (rect size, radius, line endpoints) and stroke width aren't animatable.
- Nested anims (par inside stagger), `an_remove`, and updaters.
- Additive tracks (shake, wiggle). Only take-over exists.
- The scene target kind exists, but no scene property is animatable yet
  (background, camera).
- Duplicate-name checks scan a list: O(named objects) per creation.
- Preview playback wasn't checked visually by me (no screen capture permission).
  It runs without errors.

## M3: strokes (2026-11-24)

Examples:
- `./build/examples/03_strokes` (preview), `--export out.mp4`, or `--png N out.png`.
  A rect, circle and arrow drawing on with tapered charcoal (#16161A) strokes and
  corner overshoot, plus an underline with a pressure curve. Prints nozzle samples.
- `./build/examples/04_stroke_stress out.png`: the stress set, for checking joins by eye.

Spec: `docs/strokes.md`.

What works:
- Paths: polylines plus quadratic and cubic beziers in subpaths, flattened once at
  creation (adaptive subdivision); circles generated from the radius.
- Triangulator: variable-width ribbons with round caps, outer-side round joins,
  dots, seamless closed loops, and corner overshoot spurs. Widths follow taper and
  pressure profiles, sampled every 3 px where they vary.
- Rect, circle, line and arrow (new), plus free strokes (`an_stroke`), all go
  through the same stroke code. Rect and circle outlines are opt-in (`.stroke` or
  `.stroke_width`); `.no_fill` gives outline only.
- Draw progress and stroke width are animatable through the existing
  `(target, property)` track handle. `an_draw`, `an_draw_to`,
  `an_stroke_width_to`, and setters for both.
- Nozzle (`an_obj_nozzle`): position from arc length at the sampled progress,
  speed from the analytic derivative of the draw easing. Pure function of
  (path, frame).
- `an_scene_save_png` (stb_image_write v1.19, pinned in fetch-deps.sh).
- Geometry test over 20 stress cases at 5 progresses: no NaN, no degenerate
  triangles, widths match the profile, no spikes, no gaps. I confirmed the test
  fails for missing joins, missing caps, wrong widths, unlimited miters, and
  width sampled only at path points.
- Tessellation, 200 strokes (7 cubics each, taper and overshoot) at 1080p:
  2.2 ms/frame average while drawing on, 4.4 ms/frame with everything drawn
  (about 173k triangles).
- Determinism: two exports of 03_strokes are byte-identical.

Bug found by eye in the stress PNG and fixed: width was sampled only at path
points, so tapers on long straight segments were smeared across the whole
segment (a short line with long tapers vanished). The geometry test now checks
the width between ribs against the profile.

Stubbed / not started:
- Translucent strokes double-blend where the ribbon overlaps itself (inner sides
  of joins, crossings, spurs). Fine at opacity 1, visible during fades. Needs a
  per-stroke stencil or a coverage pass.
- Curves flatten at creation in local units: facets possible above ~4x scale.
- Fills only for rect and circle. No miter or bevel joins (round only, by choice).
- The nozzle's speed excludes object motion. Nothing uses the nozzle yet (drips
  are a later milestone).
- Preview not checked visually by me; it runs without errors.

## 5A: compositing architecture (2026-11-24)

Spec: `docs/layers.md`. Phase 5A only: no visible effects yet. 5B (riso,
shadow, GPU hash, paper/grain, palettes) and 5C (boil, patterns, look example,
bitrate) haven't started.

Example: no new example. `02_timeline`'s fades now go through layers (two exports
byte-identical). Bench: `./build/examples/spike_frame_bench [0|1|2]`.

What works:
- **Chaos target.** `AN_PROP_CHAOS` on the scene target (the scene's first
  animatable property) and on objects as a multiplier, through the existing
  `(target, property)` tracks. `an_set_chaos` (C11 `_Generic`: scene or object),
  `an_chaos_to`, `an_scene_chaos_to`, `an_scene_chaos`, `an_obj_chaos` (scene
  times the multipliers down the tree). Defaults to 1. Nothing consumes it yet.
- **Command lists.** The tessellator emits commands per target (main, or a
  layer): triangle runs with a paint, stencil resets, and composites. A fully
  opaque frame is still one draw call.
- **Isolated layers.** Objects and groups at opacity < 1, and objects with
  translucent paint colors, render into their own layer, sized to their bounds
  in target pixels plus 2 px. Layers render deepest first, resolve 4x→1x at the
  end of their own pass, and are composited premultiplied with a 1:1
  `texelFetch` quad. Group opacity composites as one unit; nesting multiplies.
- **Coverage.** Translucent paints are stencil-tested (write once per sample per
  paint). Opaque paints use the plain pipeline. Before a translucent object in a
  shared layer, a stencil reset covers only that object's bounds.
- **Pool.** MSAA scratch per 128 px size class (48 B/px) plus 1x resolve
  textures (4 B/px), reused across frames, 512 MB budget. Over budget, a layer is
  replayed directly with its opacity, with a one-time warning.
- **Renderer API:** `ani_renderer_prepare` (outside passes), then
  `ani_renderer_draw` (in the main pass). Export, PNG, preview, tests and the
  bench all use it.
- **Tests:** `test_layers` (GPU, hand-computed colors) covers overlap uniformity
  at opacity 0.5 and with ink alpha 0.5, group opacity, nesting, translucent
  fills sharing a layer, forced-isolated vs direct, the budget fallback and pool
  reuse. `test_timeline` covers chaos. I confirmed failures for: stencil off,
  groups not isolated, isolation off, and no per-object stencil reset.

Found while implementing:
- **My doc's ±4 bound for forced vs direct was wrong.** Where two objects'
  edges split a pixel's samples, resolve-then-composite differs by up to
  1/4 of the color range ("conflation"). Measured: 0.027% of pixels differ by
  more than 1, maximum 59. The test bound is now 99.9% within 1 and every pixel
  within 65, derived in the doc.
- **A single stencil ref per paint is wrong for group layers,** since a later
  object's fill would be blocked by an earlier one's. Fixed with per-object
  stencil resets.
- **The first version reset the whole layer between every pair of objects,**
  which cost several ms on a 250-object group fade. Now resets cover only
  translucent objects, over their own bounds.

Performance (1080p, 4x MSAA, M4), measured under machine load (another app at
100% CPU, load average 2.5–3.5). Even unchanged paths showed spikes.

| scenario | before 5A, worst | after 5A: avg / p50 / p95 / worst |
|---|---|---|
| 0: 200 strokes + 50 shapes, opaque | 7.9–9.2 ms | 3.95–4.75 / 3.8–5.2 / 7.2–7.7 / 7.8–11.1 ms (1 of 5 runs had one 14.7 ms frame, alongside a 22 ms CPU-only stall) |
| 1: the 50 shapes fading together (50 layers/frame) | n/a | 5.5–6.6 / 5.7–6.3 / 7.2–9.4 / 9.0–10.6 ms |
| 2: everything in one fading group (1 full-frame layer) | n/a | 4.3–5.8 / 4.2–6.2 / 7.8–11.0 / 8.4–21.9 ms; 3 of 5 runs had 1–8 frames over 12 ms |

Tessellation is unchanged at 2.3 ms average.

Stubbed / not started:
- Everything in 5B and 5C.
- The group fade doesn't reliably meet the 12 ms ceiling under load; it needs a
  quiet-machine measurement before deciding on atlas packing or smaller classes.
- The preview window wasn't checked visually by me; it runs without errors.

## 5A follow-up and 5B: style (2026-11-24)

Examples:
- `./build/examples/05_style` (preview), `--export out.mp4` (3.00 s, 181 frames),
  or `--frames DIR [STEP]` (PNG every STEP-th frame, default 5 = 12 fps).
  Outputs written: `out/style.mp4` and `out/style_frames/` (37 PNGs).
- `./build/examples/06_palettes DIR`: `swatch_<name>.png` per palette, every color
  on cream and on charcoal. Written to `out/`.

What works:
- **5A follow-up.** The bench now excludes 30 warm-up frames and reports median,
  p99, frames over 12 ms, page faults and memory growth (results in
  docs/layers.md). New tests: a three-level nested fade with overlapping inner
  objects (hand-computed colors), and a fade sweep of two touching shapes from
  1.0 to 0.98, written to `out/test/` as full-frame and 8x-zoom PNGs.
  The sweep shows the conflation seam that separate fades produce, and that a
  group fade avoids.
- **Style.** `an_style` in `an_scene_desc`, with zero fields defaulting. Every
  effect is scaled by effective chaos.
- **Riso:** the fill is offset in scene space, so it doesn't rotate with the
  object. It applies only to objects with ink; `.riso = AN_ON / AN_OFF`
  overrides per object; `style.riso_off` turns it off scene-wide.
- **Hard shadow:** the union of fill and ink as one paint, stencil-tested when
  translucent. `style.shadow` is the scene default and `.shadow = AN_ON / AN_OFF`
  overrides per object. No shadow at chaos 0.
- **GPU `an_hash`** (`shaders/hash.glsl`, 64-bit emulation on 32-bit pairs)
  matches the CPU bit for bit on 4,096 inputs, including edge values.
- **Paper background:** the palette's paper color, plus grain from `an_hash` per
  scene pixel at 12 Hz (or every frame), plus a vignette. Chaos 0 gives exactly
  the paper color.
- **Palettes:** `AN_PALETTE_RIOT`, `_HAZARD`, `_ACID`, `an_palette_named`.
  The scene's paper and default shadow color come from the palette.
- **Tests:** `test_style` (riso, rotated riso, riso rules, shadow edges, shadow
  union, grain against the formula, 12 Hz step boundaries at 30 fps, vignette,
  palettes) and `test_gpu_hash`. I confirmed each fails against a deliberately
  broken version.
- Two exports of 05_style are byte-identical.

Found while implementing:
- **My first riso test had wrong hand coordinates:** pixels I'd placed inside an
  ink band. The test was fixed; the renderer was right.
- **My first GPU-hash sabotage didn't actually apply** (a regex `?`), so a "no
  carry" build looked like it passed. Redone properly, it gives 2,923
  mismatches.

Performance: see docs/layers.md "Results". Summary:
- Scenario 0 median is 5.7–5.9 ms with 5B, against 4.6 ms in 5A. The paper
  shader accounts for about 1.1 ms of that; riso is free.
- Shadows on every object (scenario 3) break the 12 ms ceiling in every run,
  because each shadowed stroke is tessellated twice.

Stubbed / not started:
- Concrete background. A faster shadow (tessellate once, draw twice). Reserving
  mesh capacity to avoid mid-sequence doubling.
- 5C: boil, patterns, the look example, the bitrate report.
- The preview wasn't checked visually by me; it runs without errors.

## Before-5C fixes and 5C boil (2026-11-24)

Outputs: `out/style_frames/` (37 PNGs at 12 fps, every boil step) and
`out/style.mp4` (3.00 s, byte-identical across two exports); swatches in
`out/`.

What works:
- **Shadow:** riso blue by default (`an_palette.shadow`), offset measured from the
  ink's outer edge. **Hazard:** orange/cyan/lime.
- **Shadowed strokes** are tessellated once and copied. **Capacity** is reserved
  per scene. With shadows on every object, frames over 12 ms went from 105–111
  per run to 0–2 (median 7.1 ms, p99 10.0–11.7 ms).
- CLAUDE.md rule 13: touching shapes that fade together must be grouped.
- **Line boil** on strokes, outlines and fills, keyed by (object stream,
  arc-length bucket, 12 Hz integer step). It's byte-identical to no boil at
  chaos 0, doesn't crawl while drawing on (bit-identical points), and doesn't
  depend on vertex density. `test_boil` plus the stroke stress set with boil.

Not done / problems:
- **Boil breaks the 12 ms ceiling** on the 200 + 50 bench: tessellation 2.3 → 4.7
  ms, about 95 frames over 12 ms per run. The cause is measured and three CPU
  fixes are proposed in docs/layers.md; none implemented yet (stopped as asked).
- Halftone/hatch patterns, `05_look`, the bitrate report: not started.

## 5C: boil performance, patterns, chaos tree, look example (2026-11-24)

Outputs: `out/look_frames/` (55 PNGs at 12 fps) and `out/look.mp4`
(4.5 s, 271 frames, byte-identical across two exports).

What works:
- **Golden hashes:** `tests/test_boil_golden.c`, 12 hashes of the full triangle list
  (24/30/60 fps × frames 0, 7, 40, 180), recorded before the boil performance work.
- **Boil performance:** one hash per bucket, displacement during the gather pass,
  reused scratch. The output is bit-identical (golden test). Stroke build for 200
  strokes went from 4.0–4.8 to 3.44 ms average, and from 8.0–8.6 to 6.84 ms with
  everything drawn.
- Docs: which space boil lives in (buckets in object space, amplitude in scene
  pixels), and its behavior under a future camera zoom.
- Halftone and hatch fills (object space, size, angle and density per object).
  Chaos multiplier render tests. `06_look`.
- 15 test programs pass.

Not done / open:
- Shadows show through pattern gaps (decision needed; see docs/layers.md).
- Frame-bench numbers after the boil fixes and after patterns are not reliable
  (machine under load). Re-run on a quiet machine:
  `for i in 1 2 3 4 5; do ./build/examples/spike_frame_bench 0; done`
- The bitrate report (deferred, as asked).

## Shadow cut, halftone AA fix, bench guard, vertex layout evaluation (2026-11-24)

What works:
- **Shadow cut** (decision (a), whole silhouette, stencil): the shadow never
  shows inside the object's silhouette (registered fill, riso-moved fill, ink).
  Halftone/hatch gaps and the riso sliver show what's beneath. Opaque shadows
  cut on the main target and in layers; translucent shadows keep the
  write-once union. Main targets now have a depth-stencil buffer
  (`ani_offscreen_make_ds`). Details and tests: docs/layers.md "Hard shadow" and
  "Shadow cut, halftone AA fix".
- **Tests:** hand pixels (sliver = paper, crescent = shadow, halftone gaps =
  paper, the moved fill's strip) and a per-MSAA-sample check with boil on over 3
  frames, opaque and translucent. Four sabotages, each caught. Golden boil hashes
  still match with the silhouette ranges skipped.
- **Halftone AA fix:** the AA width now comes from the continuous pattern
  coordinate. Layer vs direct went from 13 levels to ≤ 2; the pattern
  translation test's rim noise went from 1,426 pixels to 0.
- **Bench guard:** `spike_frame_bench` refuses to report (exit 3) when the
  1-minute load average is above 2.0, checked before and after the run.
  `scripts/bench.sh` runs 5 runs per scenario and summarizes median, p99, frames
  over 12 ms and tessellation; it stops at the first refusal.
- 15 test programs pass. `out/look_frames` and `look.mp4` were re-exported
  with the cut (the mp4 is byte-identical across two exports).

Evaluated, not adopted yet: **24-byte vertices.** The pattern affine moves from
each vertex to the draw command as a per-draw uniform (a patterned fill is its own
command; the vertex shader computes the coordinate from position). It was built
in a scratch copy of the tree, where all 15 tests pass. 06_look differs in 656 of
20.7 M pixels, each by 1 level. Clean build: 4.0 s for both. Stroke build: 7%
faster (3.45–3.55 vs 3.72–3.74 ms avg; all drawn 6.80–6.90 vs 7.34–7.47), under
load 2.7 but alternated run by run. Frame time: waiting for a quiet machine.

## Cleanup before text (2026-11-24)

- **36-byte vertices kept.** The 24-byte experiment is saved as
  `docs/experiments/vertex24.patch` (applies with `patch -p1`, checked with
  `--dry-run`), with its measurements in `docs/experiments/vertex24.md`.
- **Outputs moved to `out/`** (gitignored): exports, PNG frames, and the
  fade-sweep PNGs from `test_layers` (`out/test/`). `make clean` only deletes
  `build/`. `scripts/time-clean-build.sh` times a clean build in a throwaway copy
  (4.1 s).
- **Bench guard by CPU idle** instead of load average: it refuses when other work
  keeps more than 1.5 cores busy over a 1 s sample (85% idle on 10 cores), checked
  before and after each run, and prints the measurement. `scripts/bench.sh`'s
  summary was checked against the raw output of a 5-run scenario-0 set: every
  median, p99, over-12 count and tessellation average matches.
- **That run misses the 12 ms ceiling, on a quiet CPU** (93–96% idle): scenario 0,
  median 7.80–8.33 ms, p99 13.6–15.0 ms, 56–89 frames over 12 ms per run (worst
  around frames 291–325, everything drawn, boil on). Tessellation averages 4.0 ms.
  Not addressed yet.
- **`.paper_under`** per object: the silhouette filled with the background color
  before the object's paints (docs/layers.md). Tests and a sabotage check
  (dropping the paper command fails all three cases).
