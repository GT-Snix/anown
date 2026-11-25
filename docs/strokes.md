# Strokes

Status: implemented (2026-11-24). Code: `src/path.c` (paths), `src/stroke.c`
(triangulator), `src/nozzle.c`. Tests: `tests/test_stroke.c`. Visual check:
`./build/examples/04_stroke_stress out.png`.

## Paths

- An `an_seg` list (`AN_MOVE`, `AN_LINE`, `AN_QUAD`, `AN_CUBIC`, `AN_CLOSE`) is
  flattened **once, when the object is created**: curves by adaptive subdivision
  (split at t = 0.5 until both control points are within `ANI_FLATNESS` = 0.05
  local px of the chord, depth ≤ 16), circles directly from the radius
  (`ceil(π / acos(1 - 0.05/r))` points, 16..2048).
- The result is stored in the scene arena: points, per-point arc length within each
  subpath, and each subpath's length and start offset. **Rendering only reads it.**
  Nothing is cached or updated while rendering.
- Built-in shapes are paths too:
  - **rect:** closed, starting at the top-left corner, clockwise
  - **circle:** closed, starting at the top, clockwise
  - **line:** open
  - **arrow:** the shaft, then the head as one stroke through the tip
- A rect or circle fill uses the same polygon as its outline.
- A flatness of 0.05 local px holds up to about 4x on-screen scale. Beyond that,
  curves may show facets (see Limits).

## Width

Width at arc length `s` in a subpath of length `len` is
`stroke_width × world scale × taper(s) × pressure(s / len)`, where:

- `taper_in`, `taper_out` are arc lengths in local px. Each uses the curve
  `x(2 - x)` (quadratic ease-out): the pen lands and widens fast, then levels off.
  If `taper_in + taper_out > len`, both shrink proportionally.
- `pressure` is an optional array of multipliers, evenly spaced along the subpath
  and linearly interpolated.
- The world scale is `sqrt(|det|)` of the world transform.

Where the width varies (taper zones, or anywhere a pressure curve is set), the
triangulator inserts samples every 3 local px. Without them, a long straight
segment would linearly interpolate the width between its two endpoints and lose
the profile. This bug was found in the stress PNG and is now caught by the test.

## Triangulation

For each subpath (one pen stroke), revealed up to the draw progress:

1. Collect the centerline points up to `progress × total` arc length. The last
   segment is cut at the exact arc length.
2. Merge points closer than 1e-3 px (duplicates, zero-length segments). If one
   point is left, draw a **dot**.
3. **Segments:** draw a trapezoid per segment, using the half width at each end.
4. **Joins:** draw a round fan on the **outer** side of every interior point,
   radius = half width there. The inner sides overlap, and the outer side never
   has a gap. A 180° reversal is a half-disc through the tip.
5. **Caps:** draw a round half-disc at each open end.
6. **Overshoot** (`brush.overshoot`, in stroke widths): at corners turning more
   than 25°, a spur continues along the incoming direction. Its length is
   `overshoot × width × f`, where `f` ramps from 0 at 25° to 1 at 90°. It narrows
   to 0.55 × the width and ends in a round tip.
7. A closed subpath is a seamless loop (no caps, joins all around) only when it's
   fully drawn and untapered. Otherwise it's an open stroke that returns to its
   start.

Every triangle goes through `ani_mesh_tri`, which drops triangles with non-finite
coordinates or an area under 1e-6 px². Arc steps keep the sagitta ≤ 0.2 px.

### Guarantees, and how they're tested

`tests/test_stroke.c` runs the stress set (`tests/stroke_stress.h`: 180°
reversals, 1° acute angles and 1° turns, zero-length segments, duplicate points,
dots, closed paths, a cusp, a self-loop, a spiral, curves tighter than the stroke
width, a pressure curve that reaches zero, taper longer than the path, very wide
strokes on short segments) at draw progress 0.13, 0.5, 0.77, 0.999 and 1. It
checks:

- no NaN or inf, and no triangle under 1e-6 px²
- rib normals are unit length; constant-width strokes have exactly w/2
- between ribs, the linearly interpolated width matches the brush profile
- **no spikes:** every vertex is within half the width (× (1 + 2 × overshoot) for
  spurs) of the centerline
- **no gaps:** points at 0.9 × half width across every segment, on both bisectors
  of every join, and past every open end are covered by a triangle

I checked that the test fails when the code is broken on purpose:

| broken version | result |
|---|---|
| no join fans | 2,623 gaps |
| no start caps | 61 gaps |
| widths 10% too wide | 3,286 width failures |
| unlimited miter joins | 332 spikes |
| width only at path points (the bug above) | 75 profile failures |

## Draw-on and the nozzle

- `AN_PROP_DRAW` (0..1) and `AN_PROP_STROKE_WIDTH` are ordinary animatable
  properties, recorded as tracks through the same `(target, property)` handle as
  everything else.
- `an_draw(o, opts)` always starts from 0: it's a track with an explicit start
  value. Create the object with `.undrawn = true` so nothing shows before.
- `an_draw_to` continues from the current value (take-over).
- Fills ignore draw progress.
- `an_obj_nozzle(o)` returns the pen tip at the frame last passed to `an_seek`,
  as a pure function of (path, frame):
  - **position:** the point at arc length `draw × total`, through the world
    transform at that frame
  - **direction:** the path tangent there
  - **speed:** `|d(draw)/dt| × total × |world tangent|`, where `d(draw)/dt` is the
    analytic derivative of the draw track's easing (spring velocities included).
    Speed is 0 when progress is clamped at 0 or 1. Object motion isn't included.
  - **width:** the stroke width there (taper and pressure applied)
- The test checks the nozzle's position, speed (linear, and in-out at its
  midpoint), parent scaling, and that it doesn't depend on seek order.

## Performance

200 strokes, each 7 cubics, with taper and overshoot, drawn on in a stagger, at
1920×1080 on an M4: tessellation averages **2.2 ms per frame** over all 330
frames, and is **4.4 ms per frame** with all strokes fully drawn (about 173k
triangles).

Each stroke allocates small scratch buffers per frame; that's an obvious first
optimization if it matters.

## Limits (known, not yet handled)

- **Translucent strokes double-blend where the ribbon overlaps itself** (the inner
  sides of joins, self-crossing paths, spurs). At opacity 1, which is the normal
  case, this is invisible. Fading a stroke shows darker spots. The fix is a
  per-stroke stencil or rendering strokes to coverage before blending. Not done.
- Flattening happens at creation in local units, so scaling a curve up more than
  ~4x can show facets.
- Only rects and circles have fills. Free-form strokes are outlines only.
- No miter or bevel joins; every join is round, which suits the hand-drawn look.
