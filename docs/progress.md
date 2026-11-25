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

## Text, step 1: stencil re-split (2026-11-25)

No behavior change. The 8 stencil bits are re-budgeted for glyph fills
(docs/text.md, problem 1): `0x80` silhouette (unchanged), `0x70` write-once
marks (shadow `0x70`, fill `0x20`, ink `0x30`, previously `0x7F`, 2 and 3 on
the low 7 bits), `0x0F` reserved for the winding counter. Every state reads
and writes only its own field.

- All 15 test programs pass.
- `scripts/ref-frames.sh` (new): 60 PNGs from every example that writes them,
  plus the test PNGs. Byte-identical to the baseline.
- Those frames don't use translucent paints, which is where the marks live, so
  a scratch program also rendered 24 frames of translucent fills, inks and
  shadows (halftone, paper_under, a faded group, forced isolation, chaos 0 and
  1) against the baseline commit and the re-split tree: raw pixels identical.
  Giving fill the ink's mark on purpose changed all 24, so the check sees it.
- stb: the pin was already upstream HEAD (`6b5799b`, truetype v1.28);
  `fetch-deps.sh` now also fetches `stb_truetype.h` and stb's LICENSE. All 60
  reference PNGs byte-identical after the re-fetch.
- Fonts in `assets/fonts/` with their OFL texts and copyright notices
  (`assets/fonts/README.md` has sources, commits and hashes).

## Profile of the frame bench, scenario 0 (2026-11-25, before text)

`spike_frame_bench 0 --profile` splits every measured frame (30..360) into
phases: CPU timestamps (CLOCK_UPTIME_RAW) around tessellation, the vertex
upload and encoding, and the GPU's own start/end times for sokol's command
buffer and the readback blit. The phases sum to the measured frame time
(within 0.03 ms). 5 runs that passed the CPU guard before and after (93–97%
idle; a sixth was refused after its run and isn't counted). Not optimized.

| phase (ms) | median frame | p99 frame |
|---|---|---|
| tessellate (CPU) | 4.17–4.71 | 7.68–7.95 |
| vertex upload (memcpy into the transient buffer) | 0.37–0.46 | 1.28–1.72 |
| encode + commit | 0.02 | 0.02 |
| commit until the GPU starts | 0.39–0.64 | 0.40–0.65 |
| GPU: render + MSAA resolve | 1.66–2.34 | 3.5–4.1 |
| GPU: readback blit | 0.09–0.13 | 0.13–1.12 |
| blit end until the wait returns | 0.12–0.16 | 0.08–0.22 |
| copy out (8.3 MB memcpy) | 0.21–0.23 | 0.21–0.38 |
| **frame** | **7.76–7.86** | **14.2–14.9** |

Tessellation is 55–60% of the median frame and about half of the p99 frame;
the GPU is under 2 ms at the median. The p99 frames are the late ones (frames
285–352: every stroke drawn, boil on), where tessellation and the GPU both
roughly double. Raw output: `out/profile/`.

## Text (2026-11-25)

docs/text.md is the spec, with an "As built" section at the end.

**Order of work.** Step 1 (the stencil re-split) was its own commit, checked
bit-identical before any text code. Glyph fills, layout and kerning, per-letter
transforms, draw-on and stagger, and the code block were then written in one
pass: the shared tessellator path made them hard to land separately. All tests
ran after that pass, not between those steps.

Works:
- `an_text` with fonts (`an_font_display`, `an_font_mono`, both embedded;
  `an_font_load`), em sizes, kerning (kern + GPOS through stb), multi-line,
  `.align` per line, `.anchor`, `.line_spacing`. One object per letter
  (`an_text_glyph`, `an_text_length`), pivot at the middle of the advance at
  half the x-height.
- Glyph fills by stencil-then-cover, non-zero; outline through
  `ani_stroke_emit` (brush, taper, overshoot); riso, hard shadow, the shadow cut
  and `.paper_under`; boil per contour; patterns.
- The size factor on chaos (with the 30% cap), from the on-screen stem every
  frame, overridable (`.size_chaos = AN_OFF`); included in `an_obj_chaos`.
  Riso offset scaled by display size. Code text has riso and shadow off.
- Snapping relative to the text's origin when calm, not under rotation or
  non-uniform scale.
- Tag-draw (`an_draw(text)`), typewriter, per-letter stagger with ordinary anims.
- `an_code`: line numbers, per-line highlight bars (`an_code_line`), a box, line
  texts (`an_code_text`); the code is calm, the box and bars boil.
- `examples/08_text`: a title drawn on, a staggered subtitle, a letter wave and
  a spin, a code snippet typed in with a stepping highlight.
  `out/text_frames/` (84 PNGs, every 10th frame) and `out/text.mp4` (836 frames,
  1080p60). Re-rendered frames are byte-identical.

Tests: `tests/test_text.c`, 13 groups, all expected values from the fonts'
tables (read by the test's own reader) and worked out in comments: layout and
anchors, the H's stems and crossbar in pixels, the O's counter, kerning in
pixels (and the same check failing with kerning off), overlap (non-zero) and
translucent overlap (write-once), winding overflow (15 and 17 fill, 16 doesn't),
a letter rotated about its pivot with its neighbors unchanged, snapping (unit
and pixel), draw-on / typewriter / stagger frames, the code block's number
alignment and bar rows, the size factor, the shadow cut on a halftone "o" (also
over a rect, with and without paper_under), and text plus shadowed shapes in
one faded layer. Breaking the implementation on purpose five ways (non-zero
lost, kerning ignored, no snapping, no silhouette on text, no size factor)
fails 8, 2, 3, 2 and 3 checks. `test_scene`'s object count changed: a text is
now a node plus its letters.

Measured: small-text quality against stb's rasterizer (fix 2 not needed), the
code-block bench scenario (median 6.1 ms, p99 under 7), and scenario 0 after
the text changes (tessellation 4.0 ms avg, unchanged). Clean build 4.14 s. All
16 test programs pass; the 60 reference PNGs are byte-identical to the
baseline.

Stubbed / not done:
- No shaping (GSUB): no ligatures, no complex scripts, Latin only. No wrapping.
- A per-letter fade costs a layer per fading letter (the "fill-only letters
  skip isolation" optimization isn't done).
- The 12 ms ceiling is still missed by scenario 0 (p99 13–15 ms), unchanged.

## Text-level paint reaches the letters (2026-11-25)

Setters and animations of a text's fill, stroke and stroke width apply to its
letters unless a letter was overridden (its own setter, or from its own first
track on). Opacity multiplies down the tree. docs/text.md, "Text-level paint
and the letters". `test_propagation` covers it; making letters never follow
fails 19 checks, and starting a take-over from the letter's own value instead
of the text's fails the no-jump check. All 16 test programs pass.

## Bitrate report (2026-11-25)

Question: the lowest bitrate where the 12 Hz paper grain still reads as grain
while saturated text and riso edges stay clean.

Method (`scripts/bitrate-study.sh`, results in `out/bitrate/results.tsv`):
- Two 3 s clips (180 frames, 1080p60) rendered as PNGs: 08_text frames 400..579
  (title with riso, code typing, highlight) and 06_look frames 90..269 (the
  chaos ramp to full). Each at paper grain 0.03, 0.045 and 0.06 (the default),
  via the examples' new `--grain G` and `--frames DIR STEP FIRST LAST`.
- Encoded with export's own arguments at CRF 18, 21, 24, 27, as yuv420p (High)
  and yuv444p (High 4:4:4), x264 preset medium.
- Measured against the source PNGs: PSNR and SSIM (RGB, all frames), and grain
  on a paper-only patch (`spike_grain_measure`): *retained* = high-pass energy
  decoded / source, *corr* = correlation of the two high-passes (1 = the same
  grain; 0 = gone or replaced by codec noise).
- 4x crops of text and riso edges: `out/bitrate/crops/` (README there).

Default grain (0.06), Mbps (text clip / look clip) and grain kept:

| CRF | 4:2:0 Mbps | 4:4:4 Mbps | SSIM 420 | SSIM 444 | PSNR 420 | PSNR 444 | grain retained | grain corr |
|---|---|---|---|---|---|---|---|---|
| 18 | 26.2 / 22.1 | 26.0 / 21.8 | 0.935 / 0.927 | 0.943 / 0.937 | 34.4 / 33.0 | 38.2 / 38.0 | 0.97 / 0.68 | 0.80 / 0.49 |
| 21 | 12.9 / 9.3 | 12.8 / 9.2 | 0.871 / 0.880 | 0.880 / 0.890 | 33.2 / 32.2 | 35.7 / 35.9 | 0.73 / 0.37 | 0.46 / 0.19 |
| 24 | 3.7 / 2.8 | 3.7 / 2.8 | 0.821 / 0.849 | 0.830 / 0.860 | 32.2 / 31.5 | 34.0 / 34.5 | 0.29 / 0.20 | 0.09 / 0.03 |
| 27 | 1.45 / 1.44 | 1.46 / 1.43 | 0.816 / 0.844 | 0.826 / 0.855 | 31.6 / 30.8 | 33.4 / 33.5 | 0.12 / 0.13 | 0.00 / 0.00 |

Lower grain amplitude, CRF 18 (4:2:0): 0.045 costs 20.1 / 15.7 Mbps and keeps
corr 0.64 / 0.29; 0.03 costs 10.8 / 8.9 Mbps, corr 0.31 / 0.12.

Findings:
- **The grain is the bitrate.** Without it the clips are ~1.4 Mbps at any CRF.
  x264 keeps it at CRF 18, keeps part of it at 21, and drops it entirely from
  24 (the paper turns flat). There's no middle ground: grain is noise, and a
  codec either pays for it or removes it.
- **Lowering the amplitude doesn't buy efficiency:** grain 0.045 at CRF 18 costs
  more than 0.06 at CRF 21 and keeps a similar share; 0.03 is faint even before
  encoding.
- **x264 `-tune grain`** (`out/bitrate/tune_grain.txt`): at equal bitrate it keeps
  about the same grain (text: 23.5 Mbps, corr 0.76, vs plain CRF 18 at 26.2, corr
  0.80). Not adopted.
- **4:4:4 vs 4:2:0:** within 2% in size at the same CRF, +2.5 to 5 dB PSNR and
  +0.01 SSIM (the chroma of saturated edges: the cyan riso sliver, the lime bar,
  magenta text), and the same grain (it's luma). But High 4:4:4 doesn't play in
  browsers, QuickTime or most hardware decoders, so it's for masters only.
- **Master** (`out/bitrate/master.txt`), 4:4:4: CRF 12 = 48.5 / 48.8 Mbps, PSNR
  42.9 / 42.4, SSIM 0.98, grain retained 1.00, corr 0.95 / 0.88. CRF 15 = 37 / 35
  Mbps, corr 0.90 / 0.75.
- **Color tagging was broken:** with ffmpeg 9.1, `-color_primaries bt709
  -color_trc bt709` on the output are overridden by the frames' "unknown", so
  exports were tagged only with the matrix and range. Fixed by marking the
  frames with `setparams` in the filter chain; both modes now report
  bt709/bt709/bt709/tv, and a cyan rect round-trips within 1 level. (The study's
  encodes predate the fix; tags don't change pixels.)

Export API: `an_export_desc.quality` = `AN_EXPORT_SHARE` (default: 4:2:0, High,
`AN_EXPORT_SHARE_CRF` 18) or `AN_EXPORT_MASTER` (4:4:4, High 4:4:4 Predictive,
`AN_EXPORT_MASTER_CRF` 12); `.crf` still overrides. Both color-tagged, with
`-movflags +faststart` (checked: moov before mdat).

Recommended defaults (set): **share = 4:2:0 CRF 18** (about 22–26 Mbps at 1080p60
with the default grain: the lowest tested setting where the grain survives in
both clips); **master = 4:4:4 CRF 12** (about 48 Mbps). If size matters more than
grain, CRF 21 halves the share file and keeps the grain on text-heavy frames
but loses most of it on 06_look; I'd judge that from the crops. Platforms that
re-encode on upload (YouTube etc.) will strip the grain at their own bitrates
regardless, so upload the master or a CRF 18 share file.

`out/bitrate/` holds 3.5 GB (the source PNGs are most of it; gitignored).

## Memoized tessellation (2026-11-25)

Per-object (and per letter batch) cache of emitted vertices and commands,
keyed by everything the emission reads; docs/layers.md "Memoized
tessellation" has the design. Gate (`tests/test_memo.c`): with the cache on,
meshes and pixels equal the cache-off render frame by frame in forward, reverse
and shuffled order and under a 64 KB budget; leaving any of six key fields out
fails it. Boil goldens unchanged; 60 reference PNGs byte-identical; all 17
test programs pass.

New bench scenario 6: everything drawn from the start, 25 of the 250 objects
(every 10th) moving for the whole 6 s, boil on. 5 guarded runs each (3 refusals
while Spotlight indexed the bitrate study's PNGs; `out/` now carries
`.metadata_never_index`):

| scenario | cache | median ms | p99 ms | frames > 12 ms | frames > 16.7 ms | tessellation avg | replayed |
|---|---|---|---|---|---|---|---|
| 0 (draw-on) | off | 7.77–8.13 | 12.63–13.47 | 37–72 | 0 | 4.01–4.04 | |
| 0 (draw-on) | on | 5.47–5.60 | 12.76–13.46 | 14–21 | 0 | 2.23–2.38 | 59% |
| 6 (10% moving) | off | 12.10–12.28 | 13.02–13.93 | 197–299 | 0–1 | 7.55–7.62 | |
| 6 (10% moving) | on | 5.71–6.33 | 13.20–13.65 | 53–66 | 0 | 3.76–3.94 | 72% |

What it does and doesn't fix:
- **The median roughly halves; the p99 doesn't move.** The cache helps only
  objects whose state is unchanged since the last frame. Boil re-rolls every
  boiled object together at 12 Hz, so every 5th frame (at 60 fps) is a full
  miss: those frames are the p99, and they now also pay for storing entries.
  Scenario 6's 72% replay rate is that cadence (4 of 5 frames x 90% static).
- Tessellation-only worst frames got worse (8.0 -> 9.0 ms in scenario 0, 10.8
  -> 14.0 in 6): the full-miss frames copy their output into the cache. In full
  frames this stays inside the p99 noise above.
- The 12 ms ceiling is still missed at p99 in both scenarios.

Proposal, not implemented: **parallel tessellation for the misses**, merged in
traversal order so the output stays identical:
1. Sequential pass (cheap: sampling only): walk the tree as now, decide each
   object's target layer, its key, and whether it dirties the layer's stencil
   (computable from its sampled style without emitting), so each object's
   "layer already dirty" input is known up front. Hits are resolved here.
2. Parallel pass: emit every miss into its own buffer (vertices + commands
   relative to the object), on a fixed pool of worker threads, each with its own
   stroke scratch. Emission is already a pure function of (object, key inputs).
3. Sequential merge in traversal order: copy each object's buffer and relocate
   its commands exactly as a cache hit is replayed today, then `merge_runs`.
Output is the same bytes whatever the thread count or schedule, since step 3's
order is fixed; test_memo's comparison extends to "1 thread vs N threads". On
the M4's 4 performance + 6 efficiency cores, a full-miss frame's ~8 ms of
tessellation would be expected around 2 ms, which would bring the p99 under
12 ms (an estimate to be measured, not a result).

## Grain modes (2026-11-25)

`an_style.grain_mode`: `AN_GRAIN_STATIC` (one fixed paper texture: grain step 0
on every frame), `AN_GRAIN_ANIMATED` (re-rolled at 12 Hz, or every frame with
`grain_every_frame`), and `AN_GRAIN_AUTO` (the zero value): static in
`AN_EXPORT_SHARE` exports, animated in master exports, PNGs and the preview.
Test (`test_grain_modes` in test_style): static frame 30 equals animated frame 0
byte for byte (step 0), animated frame 30 differs, and AUTO follows the export.
The 60 reference PNGs are unchanged (PNGs keep animated grain).

Sweep with static grain (`out/bitrate/static.tsv`; default amplitude 0.06; same
clips, method and measures as the bitrate report; crops `out/bitrate/crops/*_g0.06s.png`):

| clip | grain | CRF | 4:2:0 Mbps | 4:4:4 Mbps | SSIM 420 | grain retained | grain corr |
|---|---|---|---|---|---|---|---|
| 08_text | static | 18 | 4.98 | 4.90 | 0.970 | 1.00 | 0.96 |
| 08_text | static | 21 | 3.67 | 3.63 | 0.959 | 1.01 | 0.93 |
| 08_text | animated | 18 | 26.15 | 26.04 | 0.935 | 0.97 | 0.80 |
| 08_text | animated | 21 | 12.87 | 12.79 | 0.871 | 0.73 | 0.46 |
| 06_look | static | 18 | 7.91 | 7.97 | 0.948 | 0.70 | 0.62 |
| 06_look | static | 21 | 5.37 | 5.36 | 0.927 | 0.54 | 0.44 |
| 06_look | animated | 18 | 22.05 | 21.83 | 0.927 | 0.68 | 0.49 |
| 06_look | animated | 21 | 9.29 | 9.15 | 0.880 | 0.37 | 0.19 |

- Static grain is 3–5x cheaper at the same CRF and keeps more of the grain: the
  encoder codes it once and predicts it afterwards. SSIM is higher too (bits go
  to edges instead of noise); text and riso edges in the crops are clean at
  both CRFs.
- 06_look costs more than 08_text because its chaos ramps over the clip: the
  grain's amplitude scales with chaos, so the "static" texture still changes
  until the ramp ends.
- Recommended (set): share exports use static grain (AUTO) at **CRF 18**: about
  5 Mbps for text frames, 8 for a scene whose chaos is moving, with the grain
  intact. CRF 21 saves another ~30% and still keeps the grain on text (corr
  0.93) but loses about a third of it during a chaos ramp. Master exports keep
  animated grain at CRF 12.

## Camera, step 1: properties (2026-11-25)

docs/camera.md is the spec. Works:
- Camera pos / zoom / rotation on the scene target (`an_camera_move_to`,
  `_zoom_to`, `_rotate_to`, setters, `an_camera_get`); zoom interpolates in log
  space; the paper, grain and vignette stay on the frame.
- The translation fast path: a camera with zoom 1 and rotation 0 is a per-draw
  offset, so the tessellation cache keeps hitting while it pans (scenario 7:
  median 6.2 ms vs 12.4 with the camera in the transforms; numbers in
  docs/camera.md).
- `an_scene_desc.max_zoom` (default 4): flattening at 0.2 / max_zoom px and
  1 / (1000 max_zoom) em, the same as before at the default; the renderer warns
  once when any drawn object's largest singular value on screen exceeds it
  (spring overshoot included, since it's measured on rendered frames).
- Text snapping only at an effective scale of exactly 1; the text size factor
  includes the camera zoom (`an_obj_chaos` too).
- Boil buckets coarsened by powers of two below a 6 px wavelength on screen,
  from the scale at the start of the boil step.
Tests: `tests/test_camera.c` (zoom vs riso offset in pixels, max_zoom,
zooming into code text: no shadow, snapping off once and back, chaos 0.81 at
3x; boil coarsening at 1, 0.4, 0.3, 0.1 and only at step frames); test_memo's
scenes pan and zoom. 18 test programs pass; 60 reference PNGs byte-identical.

## Camera, step 2: shake (2026-11-25)

`an_shake(s, intensity, opts)` (an `an_anim` on the scene; default 0.5 s):
translation only, in screen px after zoom and rotation; trauma 1 -> 0 linearly,
amplitude intensity x trauma^2, smooth (smoothstep) value noise at 15 Hz from
`an_hash` (seed, event index, sample). A pure function of the frame; outside an
event's window nothing is computed, so the camera is exactly at rest.
`an_camera_get` reports the shake separately from the base position. Shakes
stay on the translation fast path.
Tests (test_camera): offsets bit-identical in forward, reverse and shuffled
order; exactly {0, 0} before the trigger and from the end of the decay on;
frame 21 recomputed by hand from the formula; the rest frame byte-identical to
frame 0. test_memo's shapes scene shakes twice (overlapping) while panning:
cache on == off in all orders.

## Camera, step 3: verbs (2026-11-25)

The verb transform (scale vec2, rotation, pivot defaulting to the object's
center: bounding-box center for paths, the block center for text) and three
verbs as `an_anim`s: `an_slap` (2.5x and a -0.06 rad tilt to identity, spring
0.45 / 0.22 s), `an_pop` (0 to 1, spring 0.5 / 0.3 s, 16% overshoot),
`an_squash_stretch` ({1.3, 1/1.3} to identity, spring 0.35 / 0.3 s). The slap's
impact frame is computed analytically (t* = 0.0799 s: frame 5 at 60 fps) and
`.impact_shake` records a camera shake there. A verb at identity skips its
matrix entirely, so a finished verb is bit-identical to none. The text size
factor and riso scale use the scale without verbs; the nozzle includes verbs.
Tests (test_camera): last frame byte-identical to no slap; frame 0 at 2.5x and
tilted, by hand at the center lines and near two corners; impact frame and the
shake starting exactly there; a det-1 squash keeps a 6 px ink band; a custom
pivot stays fixed every frame; a slapped 28 px Archivo letter keeps chaos
0.6188. Breaking the impact frame (floor) or dropping the verb rotation fails
them.

`examples/09_camera`: a title slapped in letter by letter with an impact shake,
a node popping in and others squash-stretching, a camera pan and 2.5x zoom
onto a node and back, a final slap with a bigger shake. `out/camera.mp4`: 555
frames (9.25 s, 1080p60, share mode: static grain, CRF 18), 5.5 MB, 4.8 Mbps,
exported in 6.3 s wall (1.46x realtime; 25.3 s CPU including ffmpeg).
`out/camera_frames/` (111 PNGs, every 5th frame). Re-rendered frames
byte-identical. 18 test programs pass; reference PNGs byte-identical. Clean
build 4.92 s: at the 5 s budget, so the next addition should look at build time
first.

## History-free text under zoom; vendor object cache (2026-11-25)

- Text snapping and the size factor depend only on the frame's own sampled
  transforms (snapping: the text's world transform and the letter's own; the
  factor: the scale without verbs, camera zoom included). New test
  (`test_no_history` in test_camera): after a zoom round trip 1 -> 2.2 -> 1,
  frames 30, 119, 120, 135 and 150 rendered straight from a fresh renderer are
  byte-identical to the sequential render with a warm cache, with the same
  snapped-letter count (3 at zoom 1, 0 otherwise) and chaos (0.6188).
- Per-file compile times (serial, one at a time): sokol_impl.m 2.41 s,
  stb_impl.c 0.99, offscreen.m 0.63, tess.c 0.44, test_text.c 0.36,
  test_style.c 0.30, everything else <= 0.24; 11.4 s summed.
- The vendor objects are cached in `.vendor-cache/` (Makefile): clean build
  2.9–3.3 s with the cache, 4.3–4.6 s cold. That keeps the 5 s budget without
  splitting any of our files (the largest, offscreen.m at 0.63 s, isn't on the
  critical path). `make clean-cache` drops it.

## Explainer objects, 1: arc track and array / bars (2026-11-25)

docs/explainer.md is the spec (its doc diff against the first proposal is in
`out/explainer_doc.diff`).
- `an_move_arc`: a pos track along a cubic Bézier with end tangents along the
  chord's upward normal; position = the curve at the eased progress; the last
  frame is exactly the target; a linear ease inherits the channel's velocity.
- `.paper_under` is now an `an_toggle` (AUTO / ON / OFF), like `.riso` and
  `.shadow`; AUTO is off except for patterned array cells. `= true` still
  compiles (AN_ON is 1); reference frames byte-identical.
- `an_array` (cells or bars): one group of element groups (cell/bar + value
  label) and index labels; `an_array_item` / `_cell` / `_slot_pos`;
  `an_array_highlight` / `_unhighlight` (fill tracks); `an_array_swap`: lifted
  arcs, both at or above the row, so nothing touches and nothing crosses the
  labels below; heights scale with distance and clamp to the frame (warning).
  Script-time slot permutation updated when a swap is played.
- Found while implementing (both in the doc): sliding the lower element along
  the row passed it through the elements in between (fixed: both arc above);
  plain Bézier arcs grazed touching neighbors by a fraction of the swap
  distance, 4 px measured on a two-slot swap (fixed: lifted arcs, no travel
  until clear).
Tests (`tests/test_explainer.c`): arc midpoint by the Bézier formula (and
downward / vertical arcs), exact end, velocity carried over by a linear
take-over; array layout, bar heights; swap midpoint heights by hand (249.02 /
144), exact ends, zero overlap at every frame, nothing below the row, seek order;
seeking into the middle of a swap = the sequential render; highlight follows its
element through a swap; the array fades as one layer with no seam;
paper_under default. Breaking the lift or the permutation fails them. 19 test
programs pass.

## Explainer objects, 2: code marker (2026-11-25)

`an_code_marker(code, opts)` (a bar behind the line box and an arrow left of the
block, created hidden) and `an_code_goto(code, line, opts)`: the first goto
shows it at the line with a 0.25 s fade (no slide), later ones slide it
(0.35 s, in-out; springs are replaced by in-out so it can't overshoot into the
next line). Ordinary pos and opacity tracks; `an_code_current_line` is
script-time. Test (test_explainer): hidden before the first goto; mid-fade at
line 3 by hand (alpha 0.5935); at line 5 the bar's rows are exactly the line
box (221..259, row 220 half, 260 paper), and line 3 is clear again.
The ▶ triangle became an arrow: no filled arbitrary polygons (friction list).

## Explainer objects, 3: callouts and pointers (2026-11-25)

`an_callout` (AN_KIND_CALLOUT: a label and a curved arrow from the label's box
edge to a target, `gap` short, bend along the up-screen normal; head as two
strokes) and `an_callout_draw` (arrow draw + typed label as one anim).
`an_array_pointer(arr, label, slot)` (a callout below the index labels, arrow
up at the slot) and `an_array_point(ptr, slot, opts)` (a pos track). No
re-aiming; 9B's edges are noted in the doc. `anown.h` now includes
<stddef.h> (NULL for "all defaults" didn't compile without another include).
Tests (test_explainer): arrow start at the label's edge and end gap short,
straight and bent (by hand), left-side targets; drawing on; the pointer's
geometry and an exact slide. 19 test programs pass; reference PNGs identical.

## Explainer objects, 4: examples/10_quicksort (2026-11-25)

Lomuto quicksort on 9 distinct values (1..20) from the scene's seeded RNG,
sorted at script time in plain C while recording: title slap (per letter, impact
shake), cells popping in, the code panel fading in, per partition the active
range bright and the rest dimmed (fill tracks, not opacity), the pivot in
magenta, `i` / `j` pointers, the code marker on the executing line, swaps on
lifted arcs, each placed pivot slapped green, and a final array slap with a
"sorted!" callout. Value labels 34 px (80 px cells).
`make example-quicksort` exports it unattended: `out/quicksort.mp4` (2730
frames, 45.5 s, 1080p60, share mode, 23.9 MB, 4.2 Mbps, BT.709) and every 10th
frame in `out/quicksort_frames/` (273 PNGs); 279 s wall for both. No swap
clamped or overlapped (no warnings).
The API friction found while writing it is listed in docs/explainer.md
("API friction from writing examples/10_quicksort"): 11 items, the main ones
being that `an_play_async` doesn't return its end time (parallel-with-a-gap
needs hand-typed default durations), verbs can't hold their start state before
they begin, `an_array_pointer` has no options, and there's no script-time
query of an object's state at the cursor.

## API fixes (after 10_quicksort), 9: frame dumps (2026-11-25)

Golden hashes of every 10th quicksort frame from `ad288d0` are in
`out/golden/quicksort_ad288d0.sha256` (273 frames); `scripts/quicksort-golden.sh`
re-dumps and compares them.
`an_scene_save_frames(s, &(an_frames_desc){ .dir, .prefix, .every, .first, .last })`:
one renderer for the range, the same pixels as `an_scene_save_png` (PNG grain:
animated), PNGs encoded on 8 threads. 10_quicksort takes `--export FILE`,
`--frames DIR`, `--every N` (default 10); `make example-quicksort` writes only
the mp4, `make example-quicksort EVERY=10` also dumps frames. Every 10th frame:
18 s instead of ~230 s, all 273 byte-identical to the golden set.

## API fixes, 1: handles (2026-11-25)

`an_play` / `an_play_async` / `an_par` / `an_stagger` return `an_handle`
{scene, start, end}; `an_wait_until(h)` (docs/timeline.md, "Handles"). The array,
marker and callout calls are anims, so they get handles when played. The
quicksort script's hand-typed arithmetic (`an_wait(0.9 - 0.15 - 0.35 + 0.1)` and
the paint waits) is gone: `an_wait_until(swap)` plus explicit beats. Golden
comparison: all 273 frames identical (the cursor lands on the same ticks).
Test: `test_handles` in test_timeline (hand-computed starts and ends).

## API fixes, 2: entrances (2026-11-25)

`an_verb_opts.enter` (docs/camera.md, "Entrances"): hidden from creation until the
verb starts; AUTO = on for pops. The quicksort script's per-object opacity
flips (array cells, title letters) are gone; frames identical to the golden set.
Test: `test_enter` in test_camera.

## API fixes, 3 and 4: cursor queries and named cell states (2026-11-25)

`an_obj_at_cursor`; `an_array_set_state` / `an_array_state_at_cursor` with
`an_array_opts.state_colors` (docs/explainer.md). The quicksort script's own
bookkeeping (`done[]`, the color variables, `paint`) is gone; frames identical
to the golden set. Test: `test_cursor_and_states` in test_explainer.

## API fixes, 5: containers reach their children; pointer options (2026-11-25)

Fill, stroke, stroke width and draw progress of any container reach its
children unless they were overridden (docs/explainer.md, "Containers");
continuation tracks blend each child from its own value, explicit-start tracks
apply as is. `an_callout_draw` removed (`an_draw(callout)`); `an_array_pointer`
takes `an_pointer_opts` (label, hidden, size, color, offset). Quicksort frames
identical to the golden set; reference PNGs identical. Test: `test_containers`
in test_explainer (blends by hand, overrides, nesting, explicit start, pointer
options).
