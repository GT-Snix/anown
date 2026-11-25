# an_plane: a linear-transformation plane (design)

Status: approved with additions (2026-11-25). For
`~/Downloads/eigenvectors.txt`. 2×2 only; nothing 3D is built.

## API

```c
typedef struct an_plane_opts {
    AN_OBJ_FIELDS              /* pos: the origin on screen (default the frame center) */
    float unit;                /* px per plane unit; default 100 */
    float grid_step;           /* plane units between grid lines; default 1; < 0: no grid */
    bool axes;                 /* the two axes, thicker, through the origin */
    float grid_chaos;          /* the grid's chaos multiplier; default 0.3 (no shadow) */
} an_plane_opts;
an_obj *an_plane(an_scene *s, const an_plane_opts *opts);          /* a group: AN_KIND_PLANE */

typedef struct an_mat2 { float a, b, c, d; } an_mat2;               /* [[a, b], [c, d]] */
an_anim an_plane_matrix_to(an_obj *plane, an_mat2 m, const an_anim_opts *opts);
void    an_plane_set_matrix(an_obj *plane, an_mat2 m);              /* setter at the cursor */
an_mat2 an_plane_matrix_at_cursor(const an_obj *plane);
an_vec2 an_plane_point_at_cursor(const an_obj *plane, an_vec2 p);   /* scene space */

an_obj *an_plane_vector(an_obj *plane, an_vec2 v, const an_arrow_opts *opts); /* tail 0, tip M v */
an_obj *an_plane_line(an_obj *plane, an_vec2 p, an_vec2 dir, const an_line_opts *opts); /* infinite */
an_obj *an_plane_dot(an_obj *plane, an_vec2 p, const an_circle_opts *opts);
```

**Entrywise interpolation is kept on purpose.** A rotation animated entrywise
shrinks mid-way (I → R90 passes through [[0.5, −0.5], [0.5, 0.5]], length
0.71). Scripts that rotate use keyframes: the eigen script's chapter 4c goes
through 30°, 60° and 90°.

**The matrix** is a new 4-component property, `AN_PROP_MATRIX` (default the
identity). Tracks interpolate it entrywise in linear space, so M(t) =
(1 − e) M0 + e M1. That's the standard look: every point travels in a straight
line from M0 p to M1 p. Setters, continuation tracks and seeking work as for any
property (rule 9).

## Mapping before stroking

**What gets mapped:**
- Mapped geometry is geometry made by the plane: the grid, the axes, vectors,
  lines and `an_plane_stroke` (a free path in plane units).
- Its points are in plane units. At each frame the tessellator maps every
  flattened point through `p → unit · M p` (y flipped, so +y is up), then
  strokes the result with `ani_stroke_emit` under the plane's world transform.
- The mapped path is built in the frame arena, as edges are (the rule 10
  exception gains this case).

**What stays constant on screen:** M is applied to the points, not to the
transform. So ink width, taper, boil amplitude and spacing (in arc length of
the mapped path), the riso offset and the shadow offset stay the same however M
stretches.
- Straight lines stay straight under M, so the grid, axes and vectors are
  exact.
- Curves were flattened at creation. Their tolerance grows with ‖M‖, so they're
  flattened at 0.2 / max(max_zoom, 4).

**Other children** (labels, rings, any object the user parents to the plane)
are *anchored*: their position p (plane units) maps to M p, and their shape is
not deformed. So a label rides its vector's tip and stays upright.

**Vectors:**
- The tail is at M·0 (the origin) and the tip at M v.
- The head is an `an_polygon` child placed on the tip frame: the mapped tangent
  M v / |M v|, as for edges.
- The shaft stops at the head's base.
- If |M v| is shorter than the head, the head scales down to it (and fits at
  0). There is no flip or NaN: the direction falls back to the last valid
  one, then +x.

**Lines:**
- `an_plane_line` spans the whole frame: the segment p ± L·dir, with L chosen
  so that its image covers the frame, then clipped.
- If M crushes dir to zero (singular M), the line draws as the dot M p.

## The grid and clipping

**The grid, per frame:**
- Grid lines are the integer multiples of `grid_step` whose image meets the
  frame.
- With |det M| ≥ 1e-6, the frame's corners are mapped through M⁻¹, and their
  bounding box gives the line range.
- Otherwise the range at identity is used, × 4.
- At most 400 lines per frame; past that, every k-th (a message once).
- Each grid line is a part (`"grid x"` / `"grid y"`, index = its integer), so
  its boil stream doesn't depend on which lines are visible.

**Clipping to the frame** is geometry, as for plots (Liang–Barsky on the
mapped polyline):
- The clip box is the visible frame, mapped back through the camera at that
  frame, grown by half the ink width + boil + 2 px. So line ends sit just
  off-screen and never show.
- Arrows and anchored objects aren't clipped (they're meant to be seen).
- No stencil bit or layer is used.

## Cache key

`emit_memo` gains:
- **M at the frame** (4 floats; the matrix in the key);
- **the camera's position, zoom and rotation** (the clip box depends on them;
  on the pan fast path the pan isn't in `w`, so it must be added);
- for anchored objects, their mapped position (through `w`, as now).

Breaking the matrix key on purpose must fail `test_memo` (rule 16).

## Structured for 3×3 + projection later

All mapping goes through one internal interface:
```c
typedef struct ani_map { int kind; float m[9]; float unit; } ani_map;   /* kind: ANI_MAP_LINEAR2 now */
an_vec2 ani_map_point(const ani_map *, an_vec2 p);
an_vec2 ani_map_tangent(const ani_map *, an_vec2 p, an_vec2 d);         /* the Jacobian times d */
bool    ani_map_path(const ani_map *, const ani_path *, ani_arena *, ani_path **out);
```
- Grid ranges, vectors, lines, anchoring and clipping only call these.
- A later `ANI_MAP_PROJ3` (homogeneous 3×3 plus divide) adds:
  - a near-plane cut inside `ani_map_path`;
  - a per-depth flattening tolerance.

  Lines stay straight under projection, so the clipping and grid code carry
  over unchanged.
- The matrix property would become 9 components then; it's 4 now.

## As implemented (2026-11-25)

- **The grid range is always clamped to the identity range × 4**, not only
  when M is singular. So the line set changes continuously as det → 0; the
  singular frame uses the same box. A plane shrunk more than 4× shows the
  grid's edge.
- **No "message once" past 400 lines.** It would need a flag written while
  rendering; it takes every k-th line, silently.
- **A crushed vector takes its own unmapped direction** (then +x), not "the
  last valid one". That would carry state between frames, against rule 9.
- **A vector's v is its pos, turned by its rotation** (clockwise on screen, as
  everywhere) **and scaled by its scale.** So `an_move_to` / `an_rotate_to`
  animate it.
- **The grid has riso and shadow off.** Its ink is 75% of the way from ink to
  paper, 2 px.

## Gates and tests (hand-computed)

- **The identity is byte-identical.** At M = I:
  - the plane's frames equal the same plane with mapping switched off
    internally (`ani_plane_map_off`), byte for byte, with every effect on;
  - a scene that goes I → A → I gives, at its final I frames, the same bytes
    as before A.

  (1·x + 0·y = x exactly; −0 adds away.)
- **Seek order doesn't matter:** a plane animating I → A → [[−1, 0], [0, 2]] →
  the rotation R. Frames rendered forward, in reverse and shuffled, with the
  cache on and off, are identical (test_memo and test_timeline).
- **Mapping:**
  - The tip of v = (1, 1) under A = [[3, 1], [0, 2]] is at unit·(4, −2) from
    the origin, so (1360, 340) at 1080p.
  - Halfway (linear) it's at M = [[2, 0.5], [0, 1.5]], tip (2.5, 1.5) →
    (1210, 390).
  - Ink width: a grid line's stroke in pixels is the same at I and at A.
  - Under C = [[1, 1], [1, 1]], every grid point lands on y = x, and a line
    along (1, −1) draws as a dot.
- **Singular, reached from the identity:** I → C = [[1, 1], [1, 1]] over
  1 s.
  - The last frame has no NaN or infinity in any vertex.
  - The grid's line set changes only where the range rule says so: it's
    computed from the frame at every frame, and the frame where |det| drops
    under 1e-6 switches to the fallback range. Checked:
    - the fallback range contains the M⁻¹ range of the frame just before;
    - no frame's grid line count jumps by more than the lines entering at the
      frame's edge;
    - the final frame matches the hand-computed image: every grid point on
      y = x.
- **Clipping:** every mapped grid point is inside the grown frame box, and no
  line end is visible (the frame's border pixels are inked where a line
  crosses).

## Glyphs

| Glyph | Archivo Black | JetBrains Mono |
|---|---|---|
| λ U+03BB | **missing** | yes |
| → U+2192 | yes | yes |
| · U+00B7 | yes | yes |
| ² U+00B2 | yes | yes |

Checked in each font's cmap.

- **Math is set in JetBrains Mono (per the brief), so λ works there.** For λ
  in a title: set that word in mono, or spell "lambda", as the brief allows.
- **Missing glyphs are reported** (approved). A missing codepoint prints a
  message naming the character (U+XXXX and the UTF-8) and the font, once per
  (font, codepoint). The .notdef box is still drawn. There's no fallback font;
  v1 has two fonts by design.
- **Item 4 (the effect caps) is dropped.** Scripts set `.riso = AN_OFF` and
  `.shadow = AN_OFF` on small objects (arrowheads, dots, ticks, labels).

## Not in this doc (the brief needs them; listed, not designed)

- **Dashed lines** ("the original 45-degree line, dashed"). There is no dash
  brush yet.
- **Counter and readout text** ("22 of 24 left their line", the live angle).
  `an_text_set` covers both.
- **`--chapter N` with matching first and last frames.** That's script
  structure, not plane design.
