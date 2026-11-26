# an_space: a 3D linear-transformation space with an orbit camera (design)

Status: **approved with changes** (2026-11-26). Built in two milestones (end of
this doc); milestone A first. For the seven-minute
eigenvector video (`out/eigen/seven/SCRIPT.md`, section 3:00 and the tilt at
the start of it). It is `an_plane` (docs/plane.md) with a 3×3 matrix, nested
frames and a camera that can leave the page. It fills in the `ANI_MAP_PROJ3`
slot that plane.md left for it.

Scope:
- 3D content drawn as ink: lines, paths, vectors, dots, spheres.
- Projected every frame and stroked by the existing 2D pipeline.
- It is not a 3D renderer: no lighting, no z-buffer, no meshes, no 3D text.

## The one idea

Every 3D point goes through the same chain at every frame, computed fresh from
sampled properties (rule 9):

```
p (frame units)  --frames' affine maps-->  world  --root matrix M-->  world'
                 --orbit camera (view)-->  camera space  --perspective-->  2D px
```

What comes out is a 2D polyline in the space's local px. From there on it is
exactly the plane's path: Liang–Barsky clip, then `ani_stroke_emit` (rule 10;
the frame-arena exception from plane.md now covers space geometry too). So ink
width, boil, riso and shadow behave as they do on the plane: constant on
screen, however M or the camera stretches things.

**The 2D plane is a special case.** At the default camera and M = I, content
at z = 0 lands exactly where `an_plane` puts it (the plane gate below checks
it).

That gate is a correctness check, not a seam. Existing 2D scripts stay on
`an_plane`. A video that goes from 2D to 3D cuts, or crossfades briefly, from a
plane scene to a space scene; nothing is ported into a space.

## Coordinates and camera

- **World axes:** right-handed, **z up**. The xy plane is the floor; at the
  default camera you look straight down on it, x to the right and y up the
  screen, the same as the 2D plane.
- **Unit:** `unit` px per space unit at the target's depth (default 100, like
  the plane).

**The orbit camera** belongs to the space (the scene's 2D camera still applies
on top: pan, zoom, roll and shake).

| property | value | default | meaning |
|---|---|---|---|
| `AN_PROP_SPACE_ORBIT` | {yaw, tilt, distance} | {0, 0, 10} | yaw: radians around world z. tilt: radians away from looking straight down (π/2 looks at the horizon). distance: space units from the target |
| `AN_PROP_SPACE_TARGET` | vec3 | {0, 0, 0} | the world point the camera looks at; it sits at the space's `pos` on screen |

**The camera's frame:**
- Position: `c = target + D (sin t sin y, −sin t cos y, cos t)`.
- Axes: right `r = (cos y, sin y, 0)`, forward `f = (target − c) / D`,
  up `u = r × f`.

**Precision.** Rotations (Rodrigues, the camera basis) and the whole projection
are computed in double; only the final local px are stored as float. Angles
(yaw and spin) are reduced modulo 2π before sin/cos, so a spin of 40π is as
exact as one of 0.3.

**Projection** of a world' point p, with `d = p − target`:
- `x_v = r·d`, `y_v = u·d`, `depth = D + f·d`
- `s = D / depth` (perspective). With `.ortho = true`, s = 1.
- On screen (local px): `(unit · s · x_v, −unit · s · y_v)`.

At the default camera, `depth = D − z`, so every z = 0 point has s = D / D = 1
exactly.

**Animating:**
- `an_space_orbit_to` interpolates yaw, tilt and distance linearly.
- Distance is in log space, like the camera's zoom, so a dolly from 10 to 40
  feels even.
- `an_space_target_to` moves the target linearly.

**The near plane:** `depth ≥ 0.05 D`.
- Segments are cut against it in camera space before dividing, inside
  `ani_map_path` (as plane.md planned).
- A point exactly at or behind the camera never reaches the divide, so there
  are no NaNs or flips.
- **Scene camera shake** happens after projection, in screen px, like the
  rest of the scene camera. It can't move anything across the near plane.

## Matrices, frames and spin

**The root matrix M** (3×3) is the transformation being taught, like the
plane's 2×2.
- `an_space_matrix_to` animates it entrywise, so every point travels in a
  straight line from M0 p to M1 p.
- That shrinks rotations mid-way, as it does in 2D. That is fine for showing
  "a transformation"; use spin (below) when a rotation must stay rigid.

**Frames** are nested affine groups inside a space. Each has:
- its own matrix F (entrywise);
- an origin o (vec3, linear);
- a spin: an angle θ around a fixed unit axis a.

A point p in a frame maps to `o + F · R(a, θ) · p`, then through its parent
frame, and so on up to the space. There, M and the camera apply.

- **Spin is exact:** R is built from θ by Rodrigues' formula at each frame
  (in double, θ reduced modulo 2π), so a spinning globe never shrinks.

**Spin order is `F · R · p`.** The spin turns the content in the frame's own
coordinates first; F is applied afterwards.
- So under a stretching F, a spinning sphere stays the same fixed egg shape,
  and its wireframe slides around over the surface.
- To turn a stretched egg rigidly, nest two frames: a spinning frame outside,
  with a stretching frame inside it.
- **Use cases in the video:**
  - the globe is a frame that spins (F = I);
  - the egg is a **separate** frame whose F stretches a sphere, with no spin;
  - the egg's eigen arrows sit in the egg's frame.

**Storage.** Property values are 4 doubles (`ani_value`), so a 3×3 is three row
properties, `AN_PROP_M3_ROW0..2` (vec3 each).
- One `an_space_matrix_to` / `an_frame_matrix_to` call returns one `an_anim`
  (a new verb kind, `AN_VERB_MATRIX3`). Playing it records the three row tracks
  with the same timing.
- The 9 entries travel in a new `float m3[9]` field on `an_anim`.
- This works with `an_play`, `an_par` and `an_stagger` like any verb.

**3D positions of children:**
- A new scalar property, `AN_PROP_POS_Z` (default 0), makes `(pos.x, pos.y,
  pos_z)` the 3D point of anything inside a space.
- `an_move3_to(obj, an_vec3, opts)` records pos and pos_z with shared timing.
- Vectors keep the plane's rule: v is their 3D position, scaled by their
  scale, so pop_arrow and moving tips work as on the plane.
- The object's 2D rotation property is ignored inside a space. A turning
  vector is either moved with `an_move3_to` or lives in a spinning frame.

## API

```c
typedef struct an_vec3 { float x, y, z; } an_vec3;
typedef struct an_mat3 { float m[9]; } an_mat3;                     /* row-major */
#define AN_MAT3_IDENTITY ((an_mat3){ { 1, 0, 0, 0, 1, 0, 0, 0, 1 } })
an_mat3 an_mat3_rotation(an_vec3 axis, float radians);               /* helper, Rodrigues */
an_mat3 an_mat3_scale(an_vec3 s);                                    /* helper, diagonal */

typedef struct an_orbit { float yaw, tilt, distance; } an_orbit;

typedef enum an_space_grid { AN_SPACE_GRID_XY = 0, AN_SPACE_GRID_NONE } an_space_grid;
typedef struct an_space_opts {
    AN_OBJ_FIELDS              /* pos: where the target appears on screen (default the frame center) */
    float unit;                /* px per unit at the target's depth; default 100 */
    an_space_grid grid;        /* the floor grid; default the xy plane */
    float grid_step;           /* default 1 */
    float grid_extent;         /* the grid spans [-e, e]^2; default 12 (covers 1080p at the default camera) */
    float grid_chaos;          /* default 0.3, as the plane */
    bool axes;                 /* x, y, z axes through the origin */
    bool ortho;                /* no perspective */
    an_orbit orbit;            /* initial camera; zero fields take the defaults */
    an_vec3 target;
} an_space_opts;
an_obj *an_space(an_scene *s, const an_space_opts *opts);           /* a group: AN_KIND_SPACE */

typedef struct an_frame_opts {
    AN_OBJ_FIELDS
    an_vec3 origin;
    an_mat3 matrix;            /* all zero: the identity */
    an_vec3 spin_axis;         /* default (0, 0, 1) */
    float spin;                /* radians */
} an_frame_opts;
an_obj *an_space_frame(an_obj *space_or_frame, const an_frame_opts *opts);

/* animation (all record ordinary tracks; seeking works as anywhere) */
an_anim an_space_matrix_to(an_obj *space_or_frame, an_mat3 m, const an_anim_opts *opts);
an_anim an_space_orbit_to(an_obj *space, an_orbit o, const an_anim_opts *opts);
an_anim an_space_target_to(an_obj *space, an_vec3 t, const an_anim_opts *opts);
an_anim an_frame_spin_to(an_obj *frame, float radians, const an_anim_opts *opts);
an_anim an_frame_origin_to(an_obj *frame, an_vec3 o, const an_anim_opts *opts);
an_anim an_move3_to(an_obj *o, an_vec3 p, const an_anim_opts *opts);
void    an_space_set_matrix(an_obj *space_or_frame, an_mat3 m);
void    an_space_set_orbit(an_obj *space, an_orbit o);

/* content (parent: a space or a frame) */
an_obj *an_space_vector(an_obj *parent, an_vec3 v, const an_arrow_opts *opts);         /* tail at the origin */
an_obj *an_space_line(an_obj *parent, an_vec3 p, an_vec3 dir, const an_line_opts *opts); /* infinite */
an_obj *an_space_dot(an_obj *parent, an_vec3 p, const an_circle_opts *opts);
an_obj *an_space_path(an_obj *parent, const an_vec3 *pts, int n, bool closed, const an_line_opts *opts);
typedef struct an_sphere_opts {
    AN_OBJ_FIELDS              /* fill: the silhouette is filled (behind the wireframe) */
    an_vec3 center;
    float radius;              /* default 1 */
    int meridians, parallels;  /* wireframe; default 12, 7; 0 0 with .outline_only */
    bool outline_only;
    float back;                /* opacity of the far side's wire: 0 hides it (default), 1 draws it */
} an_sphere_opts;
an_obj *an_space_sphere(an_obj *parent, const an_sphere_opts *opts);

/* queries (scene px at the cursor, like an_plane_point_at_cursor) */
an_vec2 an_space_point_at_cursor(const an_obj *parent, an_vec3 p);
float   an_space_depth_at_cursor(const an_obj *parent, an_vec3 p);  /* camera depth, units */
```

**Anchored children** (text, circles, any object parented to a space or frame
that isn't one of the kinds above):
- Their 3D position maps to a screen point, and they're drawn upright there,
  as billboards. Labels ride a tip and always face the camera.
- They keep their size at any depth.

## Drawing details

**Vectors:**
- The shaft is the segment 0 → v, mapped and near-cut.
- The head is the plane's `an_polygon` head, placed on the projected tip and
  pointing along the projected tangent.
- It follows the plane's short-vector rule: a vector pointing at the camera
  projects short, so its head scales down with it, then falls back to the
  unmapped direction. No NaNs.

**Lines:**
- The segment p ± L·dir, with L = 4 · (distance + grid extent).
- It is near-cut in camera space, then projected and clipped. Lines stay
  straight under perspective, so two endpoints are enough.
- A crushed direction draws as the dot at p, as on the plane.

**Paths** (meridians, curves, the egg's outline) are flattened once at creation,
in 3D, at the plane's tolerance, divided by 4 for headroom. Each frame:
- they're mapped and near-cut segment by segment;
- an edge whose ends land more than 24 px apart on screen is subdivided (at
  most 6 times), so perspective doesn't facet them.

**Spheres** (and ellipsoids: a sphere in a frame whose F stretches):
- **The wireframe** is meridian and parallel paths. A point on the surface
  with outward normal n is on the near side when
  `n'·(cam − p') > 0`, with n' = L⁻ᵀ n.
  - L is the **composed** linear part from the sphere's frame to world': the
    root M times every ancestor frame's F·R, outermost first.
  - The camera is in world'.
  - Paths are split where that sign changes (the crossing is found by
    bisection, 8 steps).
  - Far parts are drawn at opacity `back`, so the default 0 hides them.
  - If L is singular (|det L| < 1e-9), nothing is culled.
- **The silhouette** is exact for a sphere under perspective: it's the circle
  where the viewing cone touches the sphere. That circle has center
  c_s + (r²/d²)(cam − c_s), radius r·√(1 − r²/d²), and lies in the plane
  perpendicular to cam − c_s. It's drawn as a 3D path through the same
  projection.
  - For an ellipsoid, the same is done in the sphere's own space (the camera
    mapped back through the composed affine map's inverse). That's still exact.
  - With `.fill`, the silhouette is filled behind the wire, so the globe hides
    what's behind it.

**Draw order** is the usual `(z, creation id)`; nothing sorts by depth.
- **Axes and lines through a sphere draw over it.** They're see-through: the
  part inside or behind the globe still shows, on top of its fill. That's the
  ink-drawing reading (a skewer through a wire ball).
- To hide a part, the script splits the line itself.

**The grid:**
- These are lines k·step for |k·step| ≤ extent, along x and along y, at z = 0.
  Each line is a part (`"grid x"` / `"grid y"`, index k), so its boil stream
  doesn't depend on the view.
- They're finite segments, near-cut, projected and clipped.

## Cache key (rule 16)

`emit_memo` gains, for anything inside a space:
- the full composed map at the frame: M, every frame's F, o and spin, the orbit
  and the target (as the final 3×4 world-to-camera matrix, the 3×3 linear
  part, s's distance and the ortho flag);
- `pos_z`;
- for spheres, the camera position in the sphere's own space.

Breaking any one of these on purpose must fail `test_memo`.

A moving camera changes every key each frame, so orbits don't cache. That's
expected; perf is measured below.

## Gates and tests (tests/test_space.c, values by hand)

**The plane gate.** A space at the default camera, M = I, grid off, with
vectors, lines, dots and a label at z = 0, renders byte-identical to an
`an_plane` scene with the same content.
- Why it's possible: s = D/(D − 0) = 1.0 exactly and x_v = x.
- The grid gets a geometric check instead: every segment's endpoints equal the
  plane's within 1e-3 px. The plane's segments come from a different range
  rule before clipping, so bytes can differ in the last ulp.

**Projection values:**
1. Default camera (D = 10, unit 100, at 1920×1080), p = (1, 0, 1): depth 9,
   s = 10/9. It lands at (960 + 111.11, 540).
2. tilt = π/2, yaw = 0 (front view, z up), p = (1, 0, 1): depth 10. It lands at
   (1060, 440), within 1e-3 px (float π/2).
3. tilt = π/2, yaw = π/2: the camera is at (10, 0, 0).
   - p = (0, 1, 0) lands at (1060, 540).
   - p = (1, 0, 0) lands at (960, 540) with depth 9.

**Near plane:** a line through the camera's position, and a vector whose tip is
behind the camera.
- No NaN or infinite vertex.
- Every vertex is inside the grown clip box.
- The visible part matches the hand-cut segment.
- All of the above again with the scene camera shaking (an_shake 30 px over
  the test), at every frame.

**Orbit sweeps:** no NaN or infinite vertex at any frame of
- tilt 0 → π/2 at yaw 0.3 (through every inclination to the horizon);
- yaw 0 → 4π at tilt 0 (spinning in place straight overhead, where yaw and
  the view's roll coincide).

**Spin is rigid:** a frame spinning 0 → 4π around (1, 1, 1)/√3 over 2 s. At
every frame, |R v| = |v| within 1e-5, and the axis vector's tip doesn't move
(within 1e-4 px).

**Rotation eigenvector:** for R = an_mat3_rotation((0, 0, 1), 0.7), R·(0, 0, 1)
equals (0, 0, 1) exactly.

**Sphere culling:** a unit sphere at the origin, camera overhead at D = 10.
- A point is drawn near-side iff z > r²/D = 0.1. Check the split points on
  every meridian against 0.1, within 1e-3.
- The silhouette is the circle z = 0.1, radius √0.99, at screen radius
  100 · √0.99 · 10/9.9 ≈ 100.50 px.

**Seek order:** a space that orbits, dollies, changes M and spins a frame.
- Frames rendered forward, reverse and shuffled, with the cache on and off,
  are identical (`test_memo`, `test_timeline`).

**Perf:** an example frame with the grid, axes, a 12×7 globe, 24 vectors and
a moving camera stays under rule 11's 12 ms worst frame
(`scripts/bench.sh`). In milestone A, without the globe.

## Milestones

**A** (`examples/13_space`):
- the orbit camera (orbit and target properties, orbit_to and target_to);
- the 3×3 root matrix, entrywise (`AN_VERB_MATRIX3`, the three row
  properties);
- `pos_z` and `an_move3_to`;
- vectors, lines, dots and paths;
- the grid and the axes;
- near-plane cutting;
- the plane gate and the projection tests;
- the cache key additions and the seek-order tests;
- the orbit-sweep and near-plane-under-shake tests.

The example is a tilted, orbiting grid with axes and three vectors.

**B:**
- frames (matrix, origin, spin) and their composition;
- spheres: wireframe, the culling with the composed L, the exact silhouette,
  the fill;
- billboard labels and anchored children;
- the sphere culling and rigid-spin tests;
- a still of axes through a sphere;
- the example grows the globe and the egg.

Then the comic FX doc.

## Not in this doc (listed, not designed)

- **Comic FX for the To Be Hero X-style edit:**
  - shard and starburst panels;
  - glitch slices (horizontal offset bands with RGB split);
  - speed lines;
  - impact frames (one or two inverted or high-contrast frames);
  - halftone-filled backgrounds (the halftone pattern already exists).

  These are a separate design doc ("comic FX"), next, and each would land as
  ordinary objects and verbs.
- **Cut for now:** flat polygons (and back-face culling), depth sorting, grid
  fog, labels that scale with depth.
- **No light and no shading.** Depth cues are perspective and the culling of
  far wire.
- **No 3D text and no extruded glyphs.** Labels are billboards.
- **No z-buffer.** Draw order is the usual `(z, creation id)`.
- **No `an_plane` inside a space,** and no porting 2D scripts into spaces.
