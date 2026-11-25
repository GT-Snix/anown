# Layers, style and effects

Status: design signed off (2026-11-24). **5A and 5B implemented**; 5C not started.
Measured results and findings are in "Results" at the end.

This covers the compositing architecture (layers, coverage, MSAA) and the style
features on top of it (chaos, riso offset, hard shadow, boil, patterns, paper,
palettes).

## Phases

| phase | scope | visible change |
|---|---|---|
| **5A** | Scene chaos target and object chaos multiplier, `an_set_chaos`. Command lists. Isolated layers with stencil coverage. Premultiplied compositing. Layer pool. | Translucent objects and groups composite as one unit, with no self-overlap darkening. Nothing else. |
| **5B** | Style struct. Riso offset. Hard shadow. GPU `an_hash` port. Paper/concrete background with grain and vignette. Palettes and swatch PNGs. | The look, without boil or patterns. |
| **5C** | Boil (CPU). Halftone and hatch patterns, with the 16-byte vertex format and per-object buffer they need. `examples/05_look`. Bitrate report. | Boil and patterns. |

Every phase reports the 200 + 50 benchmark before and after. **The hard
ceiling: 12 ms worst frame at 1080p.**

## The problem

Today every frame is one triangle list drawn in one call, blended straight into
the main 4x MSAA target.

1. **Overlap darkens translucent ink.** A stroke's ribbon overlaps itself (inner
   sides of joins, crossings, spurs). At opacity 0.5, each overlapping triangle
   blends again, so overlaps come out darker.
2. **Group opacity is per child.** A group at 0.5 fades each child separately,
   so where children overlap you see through one into the other.

## The model

Every object is drawn as up to three **paints**, in this order: **shadow**
(from 5B), **fill**, **ink**. An object, or a whole group, is either drawn
**directly** into its parent target, or **isolated**: rendered into its own
offscreen **layer**, then composited into the parent as one unit with its opacity.

```
frame N
  tessellate -> vertices + command lists (one per target) + layer list
  upload all vertices once (one transient buffer for the frame)

  for each isolated layer, deepest first (post-order):
    layer pass into the MSAA scratch for its size class (4x color + 4x stencil, cleared)
      its commands: paint triangles (stencil), stencil-clear quads, composites of deeper layers
    resolve 4x -> its own 1x layer texture

  main pass (4x MSAA: export offscreen target or preview swapchain)
    background (5B: paper shader)
    main commands: direct triangles, composites of top-level layers
  resolve main 4x -> 1x (export: resolve image + readback; preview: swapchain)
```

### When something is isolated

| condition | isolated? |
|---|---|
| effective opacity 1, every paint color opaque | no: drawn directly. Overlapping opaque triangles of one color write the same value, so nothing accumulates |
| object opacity < 1 (and > 0) | yes, its own layer |
| group opacity < 1 (and > 0) | yes: the whole subtree in one layer, so the group fades as one unit |
| any paint color with alpha < 1 | yes (stencil coverage below). Inside a group's layer, a child with a translucent color needs no layer of its own: the stencil handles it there |

- Opacity 0 skips the subtree, as before.
- A static scene at full opacity takes exactly the direct path: one draw call per
  run of direct triangles, as before.

## Coverage: stencil

Inside a layer, each paint of each object must cover a sample **at most once**,
however many of its triangles overlap there. The three options:

- **Max blend** only works for a single color. Fill and ink are different colors,
  and `max(yellow, charcoal)` keeps the yellow, so ink over fill would vanish.
- **Depth** (each paint at its own depth, `LESS`, write on) gives write-once per
  sample, but needs depth values threaded through every paint, and spends a depth
  buffer to express "already written".
- **Stencil** says exactly that. Paint `k` uses ref `k`, test `NOT_EQUAL`, pass op
  `REPLACE`: the first triangle to reach a sample writes it and marks it `k`;
  later triangles of the same paint fail there; the next paint (`k+1`) passes
  again, so ink still covers fill. It's per sample, so 4x MSAA edges stay correct.
  It's one pass, with normal blending.

**Choice: stencil.** As implemented:
- **Only translucent paints are stencil-tested.** Opaque paints never accumulate
  (overlapping triangles of one opaque color write the same value), so inside a
  layer they use the plain paint pipeline and don't touch the stencil. So does
  the direct path, which only draws opaque paints.
- sokol bakes the stencil ref into the pipeline (there's no dynamic ref call in
  this version), so the stencil paints use fixed pipelines: shadow ref 1 (5B),
  fill ref 2, ink ref 3.
- **Shared layers.** A group's layer can hold several translucent objects whose
  fills share ref 2, so object B's fill would be blocked wherever A's had
  written. Before a translucent object, if an earlier one already dirtied the
  layer's stencil, the tessellator inserts a **stencil-clear quad** (color writes
  off, stencil `ALWAYS` / `REPLACE` ref 0) covering **only that object's
  bounding box**. The layer pass starts with stencil cleared to 0.
- The first version cleared the whole layer between every pair of objects:
  about 249 full-layer quads for a group of 250. That alone added several ms to
  a group fade. Clearing only under translucent objects, over their own bounds,
  removed it.

## Premultiplied alpha

- Layers are cleared to (0, 0, 0, 0). Paints keep the existing blend: color
  `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, alpha `ONE, ONE_MINUS_SRC_ALPHA`. Drawn into a
  transparent target, that stores **premultiplied** color: `(c·a, a)`, and over
  existing premultiplied content it's exactly premultiplied "over". So every
  layer texture is premultiplied.
- **Compositing** reads the layer texel `(C, A)` (premultiplied) and outputs
  `(C·o, A·o)` for opacity `o`, with blend `ONE, ONE_MINUS_SRC_ALPHA` (color and
  alpha). Result: `C·o + dst·(1 − A·o)`.
  - This is correct for the main target (opaque background) and for a parent layer.
  - Nested opacities multiply exactly: a child at 0.5 inside a group at 0.5 ends
    up at 0.25 (tested).

## Where the 4x MSAA resolve happens

1. **Each isolated layer** renders into a 4x color + 4x depth-stencil scratch
   target and **resolves at the end of its own pass** into its own 1x texture.
   The scratch color and stencil are discarded (`DONTCARE`).
2. **Compositing** draws that 1x texture into the parent's 4x target as a quad
   covering the layer's pixel rectangle. The shader uses `texelFetch` at
   `fragcoord − layer origin`, both in whole target pixels, so there's no
   resampling or blur. The layer's edge anti-aliasing is already in its alpha;
   compositing writes the same premultiplied value to all 4 samples of a pixel.
3. **The main target resolves once**, at the end of the frame (export: resolve
   image and readback; preview: the swapchain resolve).

Order: layers render deepest first, so every layer is resolved before the pass
that samples it begins.

**Direct vs isolated at opacity 1.** For a single opaque object over an opaque
background, both give `a·c + (1 − a)·bg` at an edge pixel with coverage `a`; the
only difference is 8-bit rounding. Where **two** objects' edges split one pixel's
samples, resolve-then-composite differs from per-sample blending. This is the
known **conflation** artifact. Example: A covers samples {1, 2} and B covers
{3, 4}. Direct gives `0.5A + 0.5B`. Composited gives `0.5B + 0.5(0.5A + 0.5bg)` =
`0.5B + 0.25A + 0.25bg`. The difference is `0.25(A − bg)`, at most a quarter of
the color range (64). If the samples overlap instead (A on {1, 2}, B on {2, 3}),
both give `0.25A + 0.5B + 0.25bg`. Measured on the test scene: 17 of 64,000
pixels (0.027%) differ by more than 1, with a maximum of 59. The test asserts
99.9% within 1 and every pixel within 65.
(My pre-implementation guess of "±4 everywhere" was wrong.)

## Layer bounds, pool and memory

- **Bounds.** A layer covers its content's bounding box: every vertex emitted
  into it, including nested layers' rectangles (and, from 5B, shadow and riso
  offsets), plus 2 pixels. It's converted to **target pixels** (the same
  scene→target mapping as the main viewport, whose origin is snapped to whole
  pixels), snapped outward to whole pixels, and clipped to the target. An empty
  layer is dropped along with its composite.
- **Pool**, owned by the renderer, kept across frames (GPU resources, not scene
  state):
  - **MSAA scratch:** one per size class (width and height each rounded up to a
    multiple of 128 px, capped at the target size). Layers render one at a time,
    so one scratch per class is enough.
  - **Resolve textures:** 1x RGBA8, per size class, as many as a frame needs.
    Each stays alive until its parent's pass has composited it; all return to
    the pool at the end of the frame.
- **Bytes per pixel:** MSAA scratch 4 samples × (4 B color + 8 B
  Depth32Float_Stencil8) = **48 B/px**. Resolve texture **4 B/px**.
- **Max layer memory: 512 MB** for the whole pool. At 1080p, a full-frame layer
  (class 1920×1152) is 106 MB of scratch (once per class) plus 8.8 MB per resolve
  texture. So the budget covers the full-frame scratch plus about 45 full-frame
  resolve textures, or far more small ones.
- **Over budget:** when a layer can't get memory within the budget, that layer
  falls back to **direct** drawing (the old behavior: translucent overlaps
  darken), and a warning is printed once per renderer. Correctness degrades
  visibly but the frame still renders.
- **Nesting:** unlimited depth (any group or object below an isolated group can
  itself be isolated). Each nested layer adds one resolve texture for the frame.
  Nesting doesn't add scratch: layers render deepest first, one at a time.
- **Cost per isolated layer:** one extra pass, plus the clear and resolve of its
  size class at 4x. Measured below. If hundreds of simultaneous fades get too
  slow, the next step is packing sibling layers into shared atlas pages.
  Not done.

## Style and the chaos dial

```c
typedef struct an_style {      /* 5B */
    an_vec2 riso_offset;       /* fill offset, scene px at chaos 1; default {4, -3} */
    bool shadow;               /* scene default for objects (per-object override) */
    an_vec2 shadow_offset;     /* scene px; default {8, 8} */
    an_color shadow_color;     /* default palette ink */
    float boil;                /* 5C: amplitude, px at chaos 1; default 1.5 */
    float boil_spacing;        /* 5C: default 15 */
    float grain;               /* default 0.06 */
    float vignette;            /* default 0.25 */
    an_background background;  /* AN_BG_PAPER (default), AN_BG_CONCRETE, AN_BG_FLAT */
    bool grain_every_frame;    /* default false: grain steps at 12 Hz */
    const an_palette *palette; /* default &AN_PALETTE_RIOT */
} an_style;
```

- **Every effect magnitude = style value × effective chaos.**
- **Chaos defaults to 1** (decided), so the look is on by default. Exact-pixel
  tests call `an_set_chaos(s, 0)`.
- **Effective chaos** of an object at frame N =
  `scene chaos(N) × the product of obj chaos(N) from the top level down to it`.
  The object multiplier defaults to 1, so calming a group calms its children.
- **Chaos is a property on both target kinds** (implemented in 5A):
  `AN_PROP_CHAOS` on the scene target (`ANI_TARGET_SCENE`, its first property),
  and on objects as the multiplier. Same channels, tracks, take-over and
  sampling; the scene's channels live on the scene struct. Nothing consumes
  chaos until 5B.
- **API:**
  ```c
  #define an_set_chaos(target, v) _Generic((target), \
      an_scene *: an_scene_set_chaos, an_obj *: an_obj_set_chaos)(target, v)
  an_anim an_chaos_to(an_obj *o, float v, const an_anim_opts *opts);
  an_anim an_scene_chaos_to(an_scene *s, float v, const an_anim_opts *opts);
  float an_obj_chaos(const an_obj *o);     /* effective, at the seek frame */
  float an_scene_chaos(const an_scene *s); /* at the seek frame */
  ```

## Effects (5B, 5C)

All are pure functions of (scene, frame). Nothing writes state while rendering.

### Offsets live in scene space

The riso offset and the shadow offset are vectors in **scene pixels**, added
after the object's world transform. They don't rotate or scale with the object
(a shape rotated 90° still has its fill offset by (+4, −3) on screen), and they
won't be scaled by camera zoom when a camera exists: the camera transform will
apply before these offsets, which stay in final scene pixels. Both scale only
with effective chaos.

### Riso misregistration (5B)

- The fill paint is translated by `riso_offset × effective chaos`, relative to the
  ink, inside the object's target. That's a **geometry offset**, not a composite
  offset: fill and ink share one layer, so offsetting at composite time would
  need two layers per object plus resampling.
- **Applies only to objects that have both fill and ink** (decided). A per-object
  `.riso = AN_ON` forces it on for fill-only objects; `.riso` defaults to `AN_AUTO`.

### Hard shadow (5B)

- **Shadow = the union of the fill silhouette (at its riso offset) and the ink
  silhouette**, drawn in `shadow_color`, translated by `shadow_offset × effective
  chaos`, before the fill.
  - In a layer, it's drawn **inside the layer** with stencil ref 1, so the union
    covers each sample once and fades with the object's opacity.
  - On the direct path (opacity 1, opaque shadow color), the same geometry is
    drawn straight in; overlaps can't accumulate there, so it's pixel-equivalent
    to the layer path. The forced-isolation test covers that.
- **Zero blur by construction:** plain geometry, never a filtered texture, so
  pixel-aligned edges are fully sharp.
- **Shadow cut** (decided 2026-11-24): the shadow never shows where the object
  itself is. Its **silhouette** = the fill where it's registered, the fill where
  riso moved it, and the ink. It's cut out with the stencil, not a geometry
  boolean, so it's exact per MSAA sample, boil included:
  1. `SILHOUETTE`: draw the silhouette with color writes off, setting stencil
     bit `0x80`.
  2. The shadow commands pass only where that bit is clear:
     - opaque (`SHADOW_CUT`, main target or layer): `EQUAL 0` on read mask
       `0x80`, writing nothing;
     - translucent (`SHADOW`, always in a layer): `ref 0x7F GREATER stencil`
       over all bits. That fails inside the silhouette (≥ `0x80`) and where this
       layer already has a shadow mark (`0x7F`), so the union still covers each
       sample once. It then writes `0x7F` to the low bits.
  3. `SILHOUETTE_CLEAR`: the same geometry, clearing bit `0x80`, so a later
     object's shadow can still fall across this one.
  Fill and ink write-once marks (2, 3) use read/write mask `0x7F` and never see
  the silhouette bit. So a halftone or hatch gap, and the riso sliver between
  the ink and the moved fill, show **what's under the object** (paper, or an
  object beneath), never its own shadow.
  - Costs: the silhouette geometry (fill twice plus an ink copy) and four more
    commands per shadowed object. The main target now needs a depth-stencil
    buffer. Export and the tests create one with `ani_offscreen_make_ds`, and
    the preview swapchain already has one. A renderer set up without a stencil
    buffer skips the silhouette and draws shadows uncut.
  - The over-budget replay (layer drawn straight into its parent) has no stencil,
    so it draws shadows uncut, like everything else it simplifies.
- **`.paper_under`** (per object, default off): the same silhouette geometry is
  drawn in the scene's background color right after the shadow and before the
  object's own paints. Pattern gaps and the riso sliver then show paper even when
  the object sits on top of another one (without it they show what's beneath,
  e.g. graph edges through a halftone node).
  - Order per object: silhouette set, shadows, silhouette clear, paper, fill, ink.
  - Cost: one extra command, reusing the silhouette vertices (colored paper; the
    stencil passes ignore color). It costs the silhouette geometry even without a
    shadow.
  - The paper is the flat background color. With grain on, a paper_under area has
    no grain and can read as a slightly flatter patch at high chaos.
  - Tests: `test_paper_under` (a gap and the sliver over a charcoal rect, with and
    without the flag, and with a shadow).
- **Enabling** (decided): `an_style.shadow` is the scene default. Per object,
  `.shadow = AN_ON` or `AN_OFF` overrides it, and `AN_AUTO` (the zero value)
  follows the scene. The riso override uses the same three-way `an_toggle`:
  `.riso = AN_ON` forces it, `AN_OFF` stops it, `AN_AUTO` means "only with ink".
  This is one type in place of the separate `.shadow` / `.no_shadow` booleans the
  draft proposed.

### Line boil (5C)

- CPU, in the tessellator, before the ribbon is built. The centerline is
  displaced along its normal, so widths, joins and caps stay valid. Closed fills
  use the same displacement function, so fill and outline wobble together.
- Subdivision only when the amplitude is above 0: extra points every
  `boil_spacing` (~15 local px) of arc length.
- **Noise:**
  ```
  step  = (N * 12) / fps                                  // integer: exactly 12 Hz
  h(k)  = 2 * an_hash_float(seed, object stream key,
                            step << 32 | subpath << 24 | k) - 1
  d(s)  = A * lerp(h(k), h(k+1), smoothstep(frac)),  k = floor(s / spacing)
  A     = style.boil * effective chaos
  ```
  - Keyed by **arc length**, never by vertex index: no crawl while drawing on, and
    no change when the path is truncated.
  - `(N * 12) / fps` steps every 2 frames at 24 fps, every 5 at 60, and on frames
    3, 5, 8, 10… at 30 (each step starts on the first frame at or after `k/12` s).
- **Chaos 0 or boil 0:** no subdivision and no displacement, so the triangles are
  byte-identical to no boil (tested with `memcmp`).
- **CPU, not vertex shader,** unless the after-measurement breaks the ceiling. A
  vertex shader can only move finished ribbon vertices, which bends joins and
  caps. If it moves, the GPU hash must equal `an_hash` (the 5B test already
  guarantees that).

### Halftone and hatch (5C)

- Per object, `.pattern = { .kind, .size, .angle, .density }`; zero is solid.
  Computed in the fill's fragment shader.
- **Anchored to object space** (decided): the pattern is evaluated in object-local
  coordinates, using the inverse world transform from the per-object buffer.
- **Behavior under object scale:** the pattern scales with the object.
  - Scaling an object 2x doubles the dot cell, the dot size and the hatch spacing
    on screen.
  - Non-uniform scale stretches dots into ellipses and changes the hatch spacing
    differently along each axis.
  - Rotation rotates the pattern.
  - Animated scale makes the pattern grow and shrink in place; it never slides.
  - If constant on-screen dot size is wanted later, a `.pattern.screen_size` flag
    can divide the cell by the world scale while keeping the object anchoring.
- **Edges:** anti-aliased with `fwidth`. MSAA runs the fragment shader once per
  pixel, so the shader handles the pattern's own edges.

### Paper / concrete background (5B)

- A full-screen shader, drawn first in the main pass, in scene-pixel coordinates.
- **Grain:** `an_hash(seed, x | y << 16, step)`. The grain steps at **12 Hz**
  (decided), the same clock as boil: `step = (N * 12) / fps`. Per-frame grain is
  available with `.grain_every_frame`.
- **Concrete:** adds value noise at 4, 16 and 64 px.
- **Vignette:** radial darkening. Grain and vignette both scale with chaos.

### GPU hash = CPU `an_hash`, bit for bit (5B)

- An exact GLSL port in `shaders/hash.glsl`. SplitMix64's 64-bit multiply is
  emulated on (hi, lo) 32-bit pairs: `umulExtended` for the low×low product
  (compiles to Metal `mulhi`; checked) plus the two cross terms.
- **Test against real Metal output:** a render target where each pixel writes the
  hash of its own inputs as 4 bytes, read back, and compared with the CPU
  `an_hash`. The inputs include edge values for each of seed, stream and counter:
  - 0, `UINT64_MAX`
  - every power of two 2^0 … 2^63
  - values straddling 2^32: 2^32 − 1, 2^32, 2^32 + 1, 0xFFFFFFFF00000000,
    0x00000000FFFFFFFF
  - plus random values
  
  It also checks the pinned golden values from `test_rng`.

### Palettes (5B)

Accepted as placeholders:

| palette | paper | ink | accent a | accent b | accent c |
|---|---|---|---|---|---|
| `AN_PALETTE_RIOT`   | `#F3EDE2` | `#16161A` | `#00C2FF` electric cyan | `#FF2E88` hot magenta | `#C6FF00` acid lime |
| `AN_PALETTE_HAZARD` | `#F3EDE2` | `#16161A` | `#FF6A13` safety orange | `#00C2FF` electric cyan | `#16161A` charcoal |
| `AN_PALETTE_ACID`   | `#F3EDE2` | `#16161A` | `#C6FF00` acid lime | `#FF2E88` hot magenta | `#FF6A13` safety orange |

5B renders a swatch PNG per palette: every color as a filled block on cream and
on charcoal.

## Determinism

- Everything is a pure function of (scene, frame). GPU integer hashing is exact,
  and GPU float math is the same for the same inputs on the same GPU and driver.
- Two exports must be byte-identical (checked each phase).
- Across different GPU models, results may differ by last-bit rounding. That's
  not a goal.

## Tests

All expected values are computed by hand from the configured numbers, never by
calling the code under test. Effects not under test are off.

**5A (implemented):**
- **Overlap:** a self-overlapping scribble at object opacity 0.5 (opaque ink), and
  separately with an ink color at alpha 0.5 (object opacity 1). Every interior
  pixel (all 8 neighbors also covered) equals `round(0.5 × ink + 0.5 × paper)`,
  worked out by hand, ±1.
- **Group opacity:** two overlapping opaque rects in a group at 0.5. Overlap and
  non-overlap pixels of the top rect are the same color, worked out by hand.
- **Nesting:** a rect at opacity 0.5 inside a group at 0.5 gives
  `0.25 × fill + 0.75 × paper`, ±1.
- **Forced-isolated vs direct at opacity 1:** the whole test scene rendered both
  ways. At least 99.9% of pixels within ±1, and every pixel within ±65, the
  conflation bound above. Measured: 0.027% over 1, maximum 59.
- **Translucent fills sharing a layer:** two alpha-0.5 fills overlapping in a
  group at 0.5. By hand, the overlap is `0.25B + 0.125A + 0.625P` =
  (183.75, 187.125, 211.0). Without the per-object stencil reset it would show A
  only.
- **"Interior" pixels in the overlap tests** are defined geometrically: centers
  at least 1.5 px inside the ribbon, by distance to the centerline. Not "all
  neighbors are inked", which wrongly admits anti-aliased pixels in thin gaps
  between nearly parallel segments.
- Each 5A test was checked against a deliberately broken build:

  | broken build | result |
  |---|---|
  | stencil disabled | 4,407 of 10,768 interior pixels wrong |
  | groups not isolated | group and nested checks fail |
  | isolation off | every overlap and group check fails |
  | no per-object stencil reset | shared-layer check fails |
- **Chaos:** scene and object chaos are animatable; setters are instant at the
  cursor; effective chaos is the product down the tree; seek order doesn't matter.
- The layer pool respects the budget and falls back to direct when over it.

**5B:**
- **Riso:** a pixel-aligned rect at center (100, 80), size 60×40, ink width 4,
  offset (+4, −3). By hand: the fill spans x 74..134 and y 57..97; the ink spans
  x 68..132 and y 58..102. Assert fill at column 133 and paper at 134; fill at
  row 57 and paper at 56; paper in columns 72..73 between the ink's inner edge
  and the fill. Also: rotated 90°, the offset is unchanged on screen; fill-only
  objects aren't offset unless `.riso = true`.
- **Hard shadow:** a pixel-aligned rect with shadow offset (+8, +8). The pixel
  just inside the shadow's edge is exactly the shadow color, and the one just
  outside is exactly the paper, at the right and bottom edges.
- **GPU hash** against Metal, as above.

**5C:**
- **Boil:** chaos 0 byte-identical; no crawl across draw progress; truncation
  doesn't change the remaining points; 12 Hz step boundaries at hand-listed
  frames for 24, 30 and 60 fps.

## Performance

`examples/spike_frame_bench.c`: 200 strokes of 7 cubics with taper and overshoot,
plus 50 filled and outlined shapes, all drawn on; 1920×1080, 4x MSAA, M4.
**Ceiling: worst frame ≤ 12 ms.**

| | tessellation avg | full frame avg | full frame worst |
|---|---|---|---|
| before 5A (3 runs) | 2.29 ms | 4.1–4.4 ms | 7.9–9.2 ms |
| after 5A, scenario 0 (5 runs) | 2.30 ms | 3.95–4.75 ms | 7.8–11.1 ms (one run 14.7) |
| after 5A, 50 shapes fading together (5 runs) | 2.30 ms | 5.5–6.6 ms | 9.0–10.6 ms |
| after 5A, everything in one fading group (5 runs) | 2.31 ms | 4.3–5.8 ms | 8.4–21.9 ms |

These were measured with the machine under load (another app at 100% CPU, a
load average of 2.5–3.5). Even code paths 5A didn't change spiked: one scenario 0
run had a 22 ms CPU-only tessellation frame. Medians and p95 are the more
reliable figures:

| | p50 | p95 |
|---|---|---|
| scenario 0 | 3.8–5.2 ms | 7.2–7.7 ms |
| 50 fading | 5.7–6.3 ms | 7.2–9.4 ms |
| group fade | 4.2–6.2 ms | 7.8–11.0 ms |

The group fade breaks the ceiling in 3 of 5 runs (1, 7 and 8 frames over 12 ms).
Re-measure on a quiet machine before deciding whether it needs more work (atlas
packing, or smaller size classes).

## Bitrate comparison (5C)

- `an_export_desc.bitrate_kbps`.
- `examples/05_look` exported lossless (x264 `-qp 0`), at 20 Mbps and at 8 Mbps.
- Measured on a background-only crop: PSNR and SSIM against the lossless
  reference, and **grain energy** (the standard deviation of frame − 5×5 blur),
  reported as percent of the reference's.
- Done for grain at 12 Hz and every frame.

## Naming

`an_set_chaos` (not `anown_set_chaos`), `examples/05_look` (not `examples/look.c`),
following CLAUDE.md.

## Results

### 5A, re-measured on a quieter machine (5 runs each, frames 30+)

| scenario | median | p99 | frames over 12 ms per run |
|---|---|---|---|
| 0: opaque | 4.55–4.72 ms | 7.5–8.5 ms | 0, 0, 0, 0, 0 |
| 1: 50 fading | 6.70–6.82 ms | 10.7–11.5 ms | 0, 1, 0, 0, 0 (12.03 ms) |
| 2: group fade | 4.8–6.1 ms | 8.2–11.5 ms | 0, 0, 1, 1, 0 (12.03, 12.09 ms) |

- The ceiling is missed by 3 frames out of about 5,000, by at most 0.09 ms.
  Atlas packing was **not** implemented, as instructed; these numbers were
  reported first. It wouldn't help scenario 2 anyway, since that has only one
  layer per frame.
- **Early frames:** about 1,160 page faults in frame 0, 17–115 in frames 1–2
  (the layer pool being allocated in the fade scenarios), and 0 after that.
  Frame 0 takes 10–14 ms. This is one-time setup, not ongoing growth.
- **After warm-up:** a median of 10 page faults per frame (per-stroke scratch
  allocations). The mesh arrays and vertex buffer double about 13 times as the
  scene draws on. In scenario 1, two of the slowest frames coincide with that
  doubling. In scenarios 0 and 2 the slowest frames have no growth: they're the
  peak-geometry frames. Reserving capacity up front would remove the doubling
  hitches.

### Conflation seam on touching shapes that fade separately

The fade sweep in `tests/test_layers.c` writes `out/test/fade_sweep_*.png`.
It shows that two shapes sharing an anti-aliased edge get a pale seam **the
moment** each fades on its own, even at opacity 0.995. Each shape is composited
from its own resolved layer, so paper shows through along the shared edge.
Measured as the seam's green channel: 120 at opacity 1, 139–140 when faded
separately (hand bound 149.25), 121–122 when both are in one fading group. **Put
shapes that fade together into a group;** the group fades as one layer and has no
seam. A conflation-free mode (shared-edge-aware compositing) isn't planned.

### 5B

- **GPU `an_hash`:** 4,096 inputs (800 edge-value vectors from 76 edge values,
  plus random) match the CPU exactly on Metal. Removing the carry from the
  64-bit add gives 2,923 mismatches; removing a cross term from the multiply
  fails on every golden value.
- **Riso, shadow, grain, vignette and palettes** are tested against hand-computed
  pixels in `tests/test_style.c`. Grain matches the documented formula on all
  192,000 channel values within ±1. I confirmed failures for: riso applied to
  every fill, the shadow drawn as two paints (breaks the union), and grain
  stepping every frame.
- **Concrete background:** not done. `AN_BG_PAPER` and `AN_BG_FLAT` only.
- **Observations for review:**
  - The default shadow color (palette ink) merges with the ink outline, so on
    right and bottom edges it reads as a thicker outline, not a separate shadow.
  - `AN_PALETTE_HAZARD`'s accent c is charcoal, the same as its ink.
  - The grain is expensive to encode: 05_style's 3 s mp4 is 11 MB at CRF 18
    (about 29 Mbps), and its PNGs are 3.4 MB each.

**Timing (5 runs each, frames 30+).** The machine was busier during these runs
(load 2.9–4.8); scenario 1's spread reflects that.

| scenario | 5A (quiet) median / p99 | 5B median / p99 | 5B frames over 12 ms per run |
|---|---|---|---|
| 0: opaque, look on | 4.55–4.72 / 7.5–8.5 ms | 5.68–5.93 / 8.4–9.3 ms | 0, 0, 0, 0, 0 |
| 1: 50 fading | 6.70–6.82 / 10.7–11.5 ms | 6.63–8.84 / 9.5–18.7 ms | 0, 0, 60, 0, 77 |
| 2: group fade | 4.8–6.1 / 8.2–11.5 ms | 6.02–6.28 / 9.0–10.2 ms | 0, 0, 0, 0, 0 |
| 3: opaque, shadows on every object | n/a | 8.62–8.81 / 14.6–15.4 ms | 105–111 in every run |
| 4: opaque, grain and vignette off | n/a | 4.45–4.52 / 7.9–8.6 ms | 0, 0, 0 (3 runs) |

- The paper shader (per-pixel 64-bit hash emulation) costs **about 1.1 ms per
  frame** at 1080p (scenario 0 vs 4). Riso is free.
- **Shadows on all 250 objects break the ceiling in every run.** Tessellation
  doubles (2.3 → 4.6 ms), because each shadowed stroke is tessellated twice.
  The next step would be to tessellate each stroke once and draw it twice with
  an offset and color uniform (or instancing). Not implemented; reported first.

### Before 5C: shadow, hazard, single-build shadows, capacity (2026-11-24)

- **Shadow default color:** a new `an_palette.shadow`, riso medium blue
  `#3255A4`, distinct from the charcoal ink. The shadow offset is now measured
  **from the ink's outer edge**: each axis of the style offset gets the object's
  ink width added (all scaled by chaos), so the shadow always clears the outline.
  Example: ink 4, offset (8, 8) gives (12, 12). Hand-computed in `test_style`.
- **Hazard palette:** accent c is acid lime (it duplicated the ink). Swatches now
  include the shadow color.
- **Shadowed strokes are built once:** the ink's triangles are tessellated a
  single time, then copied, translated and recolored into a shadow command
  placed before the fill. `test_style` checks the copy vertex for vertex.
- **Capacity is reserved per scene** from an estimate (flattened points, taper
  and boil samples, ×12 vertices per point, ×2 with shadows). After warm-up,
  memory growth frames went from 13 to 0, and page faults on the opaque
  benchmark from 4,404 to 2,367.

**Shadows on every object (scenario 3), 5 runs:**

| | before | after |
|---|---|---|
| tessellation | 4.6 ms | 2.6 ms |
| median frame | 8.6–8.8 ms | 6.9–7.3 ms |
| p99 frame | 14.6–15.4 ms | 10.0–11.7 ms |
| frames over 12 ms per run | 105–111 | 0, 0, 2, 0, 0 |

### 5C: line boil

- **Implemented exactly as specified** (see "Line boil" above): `an_hash(seed,
  object RNG key, step << 32 | subpath << 24 | k)`, `k` = arc-length bucket
  (15 local px, tiled exactly on closed subpaths), `step = (N * 12) / fps` in
  integers, smoothstep between buckets. Centerline points are displaced along
  the undisplaced normal before the ribbon is built. Fills use the same ring
  (`ani_boil_ring`), so fill, ink and shadow wobble together.
  `style.boil` / `boil_spacing` / `boil_off`.
- **Samples are interpolated on the original path segment, never toward the
  draw cut.** The first version interpolated toward the cut point, and points
  near the head moved by about 1e-4 px between progresses: float rounding, but
  "identical" means bit-identical. Now they are.
- **Tests (`tests/test_boil.c`):**
  - chaos 0 is byte-identical to no boil, for scene chaos 0 and for per-object
    chaos 0
  - no crawl: 168 drawn points identical between progress 0.5 and 1.0, and at
    0.1 … 0.9
  - displacement depends only on arc length: a line as 1 segment vs 3 segments
    has identical displacement at all 20 bucket points
  - 12 Hz step boundaries listed by hand for 24, 30 and 60 fps, and identical
    meshes at the same instant across fps
  - keyed by name (same name → same noise in another scene), seam continuity on
    closed paths, an amplitude bound, and fill ring = outline points
  - the stroke stress set passes every geometry check with boil on
  - I confirmed failures for: keying by vertex index (14 of 20 points differ),
    step computed assuming 60 fps, and chaos 0 still subdividing. Two of my
    first sabotages proved nothing: one was masked by the second amp-0 guard,
    and one wasn't distinguishable by the no-crawl test. Hence the
    vertex-density test.
- **Cost.** Tessellation for the 200 strokes (`test_stroke`), 3 runs:

  | | before | after |
  |---|---|---|
  | average | 2.20–2.23 ms | 4.0–4.1 ms |
  | all drawn | 4.36 ms | 7.9–8.7 ms |
  | triangles | 173k | 217k |

  The 200 + 50 frame bench (scenario 0) breaks the 12 ms ceiling with boil on:
  tessellation 4.7 ms average, median frame 8.3–8.6 ms, about 95 frames over
  12 ms per run.
- **Where the time goes** (all drawn, 5.1 → 8.3 ms):
  - `an_hash` costs 6.9 ns, and the current code calls it twice per displaced
    point: about 0.9 ms. Once per bucket would be 0.08 ms.
  - The rest is the second displacement pass over every point (a copy, a
    normal with a square root, re-deduplication) and 22% more vertices.
- **CPU or vertex shader?** **Stay on the CPU for now.** A vertex shader would
  remove the hashing and the second pass, but it still needs the 15 px
  subdivision (more vertices, plus a per-vertex arc length and normal), and it
  can only move finished ribbon vertices, which bends joins and caps. The CPU
  fixes are direct and keep the geometry exact:
  1. one hash per bucket, cached per subpath (about −0.8 ms)
  2. displace during the gather pass instead of a second copy (about −1 ms)
  3. reuse stroke scratch buffers across strokes instead of a malloc per subpath
  
  Measure again after those; move to the GPU only if the ceiling still breaks.
  The GPU `an_hash` already matches bit for bit.

### Boil: which space everything lives in, and camera zoom

| quantity | space | why |
|---|---|---|
| bucket position (`k = floor(s / spacing)`) | **object space**: `s` is the path's own arc length, before any transform | the wobble pattern is attached to the object; moving, rotating or scaling it never makes the noise slide along the line |
| bucket spacing (15) | object space, per unit of the path's arc length | same |
| displacement amplitude (`style.boil` × chaos) | **scene pixels** | the wobble's strength is a property of the drawing, not of the object's size |
| displacement direction | normal of the centerline **after** the world transform, in scene space | displacement is added to scene-space points |

Consequences today:
- **Scaling an object** spreads its buckets on screen (a 2x object has 30 px
  between wobble peaks) but keeps the amplitude (1.5 px at chaos 1). A big shape
  wobbles more gently; a small one more tightly. That's deliberate: a pen line
  doesn't get thicker wobble when the drawing is bigger.
- **Moving or rotating** changes nothing in the noise; only the 12 Hz step does.

**Under a future camera zoom `z`:** "scene pixels" means **final scene pixels,
after the camera**, the same as for the riso and shadow offsets. So the camera
transform is applied to the centerline first, and the displacement is added
after it, along the post-camera normal.
- **Amplitude:** constant on screen (1.5 px at any zoom).
- **Wavelength on screen:** `15 × object scale × z`. Zooming in makes lines
  calmer (longer, equally deep wobble); zooming out makes them busier.
- **Below about 6 px of on-screen wavelength,** the wobble becomes per-pixel
  noise. The planned fix: pick a coarser bucket level (spacing × 2^j, still keyed
  in object space) whenever `15 × scale × z` drops below a minimum. The anchoring
  stays and the on-screen wavelength stays above the floor. Not implemented:
  there's no camera yet.

### Boil performance work (2026-11-24)

- **Golden hashes first.** `tests/test_boil_golden.c` records FNV-1a 64 of the
  full triangle list (positions and colors, in order) for a fixed boiled scene at
  frames 0, 7, 40, 180 and 24, 30, 60 fps. They were recorded **before** the
  changes, and the output after the changes matches them **exactly**. Replacing
  smoothstep's `3.0` with `3.0000001` breaks the test, so it really is bit-exact.
- **One `an_hash` per bucket:** a cache of the last two bucket values per subpath.
- **Displacing in the gather pass:** each point is displaced as soon as its
  successor arrives, one point behind, written in place with the same duplicate
  merging. Seamless closed loops, whose first point needs the last, are
  displaced in place right after gathering. Fill rings still interpolate between
  world-space points (that's what the golden hashes pin).
- **Scratch buffers** live in the mesh and are reused; no malloc per stroke.

| 200-stroke build (`test_stroke`, 3 runs) | before | after |
|---|---|---|
| average | 4.0–4.8 ms | 3.44–3.46 ms |
| all drawn | 8.0–8.6 ms | 6.84–6.86 ms |

- Boil's overhead (versus no boil) went from about 3.2 ms to about 1.7 ms with
  everything drawn. Most of what's left is the extra geometry: 22% more vertices.
- **Frame benchmark:** could not be measured cleanly. Another app was at 95% CPU
  again (load 4.6–4.8), and identical runs gave medians from 7.5 to 12.9 ms.
  It needs a quiet machine before a verdict.

### 5C: halftone and hatch, chaos tree, 06_look (2026-11-24)

- **Patterns:** `.pattern = { .kind = AN_PATTERN_HALFTONE | AN_PATTERN_HATCH, .size,
  .angle, .density }` on rect and circle fills.
  - Each fill vertex carries pattern coordinates: the inverse world transform
    of its final position, minus the riso offset, rotated by −angle and divided
    by size. That's affine in object space, so interpolation is exact, and the
    pattern stays attached to the object under motion, rotation, scale, boil and
    riso.
  - The shape shader computes coverage: halftone dots of radius
    `sqrt(density / π)` cells (dot area = density), and hatch lines of width
    `density` × spacing. Edges are anti-aliased with `fwidth`.
  - The vertex grows from 24 to 36 bytes (pattern u, v, and kind + density).
    The golden boil hashes (positions and colors) are unaffected.
  - Under object scale, the pattern scales with the object.
- **Tests (`tests/test_pattern.c`, hand geometry):**
  - dot centers and radii at angle 0 and 45°
  - measured coverage equals density (0.5019 for 0.5, 0.2012 for 0.2, 0.2499
    for hatch 0.25)
  - a hatch band at its hand-computed rows
  - the pattern follows the riso offset
  - moving the object by 37 px keeps every pixel classified as "deep inside a
    dot" or "clearly outside" by hand geometry exactly the same. Anti-aliased
    rims differ by up to 9 levels, because GPU varying interpolation depends on
    screen position (about 0.015 px).
  - I confirmed failures for a pattern computed in screen space and for a
    wrong radius formula.
- **Chaos tree:** the object multiplier (`an_set_chaos(obj, v)`, `an_chaos_to`)
  multiplies down the tree and animates. A render test checks a group at 0.5
  with a child at 0.5, then the child animated to 1.0, through the riso offset's
  hand-computed pixel edges.
- **`examples/06_look`:** shapes only (array cells with solid, halftone and hatch
  fills; a graph; a wave; an arrow; a legend pinned at chaos 0). The scene dial
  holds at 0, ramps to 1 over 3 s, then holds. The palettes example moved to
  `examples/07_palettes`.
- **Shadows showed through pattern gaps.** Fixed by the shadow cut (decision
  (a), 2026-11-24). See "Hard shadow" above and "Shadow cut" below.
- **Timing after patterns:** not reliable this session (load 4.8–6.0, another app
  at 92% CPU). The stroke build read 3.8–4.1 ms against 3.44 ms before patterns;
  the frame bench medians were 13.7–15.8 ms. The effect of the 36-byte vertex
  can't be separated from the load; it needs a quiet-machine re-run.

### Shadow cut, halftone AA fix (2026-11-24)

- **Shadow cut**, as described under "Hard shadow". Tests (`tests/test_style.c`):
  - **Hand pixels:** the riso sliver inside the outline is paper (columns 72..73,
    row 97), under the ink's and the fill's shadow respectively. The crescent
    outside the silhouette is shadow. With a halftone, a gap inside the moved
    fill's strip beyond the ink is paper. Halftone gaps inside the shadow are
    paper; outside the fill the shadow shows.
  - **Per MSAA sample, boil on, 3 frames (boil steps 0, 2, 7).** No hand list is
    possible once outlines boil, so three renders of the same geometry are
    compared: A (the scene), B (shadows off) and M (only the silhouette commands,
    white on black, so M = 255·s/4 for s of 4 samples inside). Each pixel must
    satisfy A − B = k·(shadow − paper)/4 with whole k, 0 ≤ k ≤ 4 − s; a fully
    inside pixel (s = 4) must have A = B. This runs twice: an opaque shadow on
    the main target (exact), and a translucent shadow in a layer (±2, the
    8-bit resolve). 50,400 fully inside pixels, 1,463 edge pixels, 6,934
    shadowed, 0 wrong.
  - **Sabotage, each one caught:**
    - the main shadow ignores the bit (hand pixels plus 27,458 wrong samples);
    - the layer shadow ignores the bit (26,837 wrong samples, layer variant only);
    - the silhouette without the registered fill (sliver and halftone pixels);
    - the silhouette without the moved fill (the new halftone strip pixel, plus
      177 and 142 wrong samples).
    M is built from the implementation's own silhouette commands, so the
    per-sample test checks the stencil mechanics. The hand pixels check what the
    silhouette contains.
  - The golden boil hashes skip silhouette vertex ranges and still match the
    hashes recorded before the boil work, so the cut changed no other vertex.
- **Halftone AA fix (found by the per-sample test):** the halftone took its AA
  width from `fwidth(length(fract(p) − 0.5))`. `fract` jumps at every cell edge,
  so `fwidth` blew up there and smeared coverage into the gaps. Derivatives come
  from 2×2 pixel quads, so the smear depended on the target's pixel parity, and
  a layer drew the same halftone up to 13 levels differently from the main
  target. The AA width now comes from the continuous pattern coordinate: one
  pixel in cells is `sqrt(|det(dFdx(p), dFdy(p))|)`. Forced vs direct with
  halftone went from 13 to ≤ 2. In `test_pattern`'s "moved by +37 px", rim
  pixels differing by more than 1 went from 1,426 to 0, so that "rim noise" was
  this bug.

