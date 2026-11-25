# Explainer objects (9A): design

Status: decided 2026-11-25 (review answers folded in). Scope: an array/bar
visualizer, a code panel with a current-line marker, and callouts. Then
`examples/10_quicksort` as the proof, written the way a user would write it.

## Principles

1. **No hidden state at render time.** Every visible change is an ordinary
   track on an ordinary object (pos, fill, opacity, draw, the verb transform),
   recorded at the cursor and sampled fresh at every frame. Seeking stays
   order-independent, and the tessellation cache key needs nothing new.
2. **Script-time bookkeeping is allowed, and stated.** Some helpers need to
   know "what's where now": which element sits in slot 3 after five swaps. That
   is script state, like the cursor. It's updated as tracks are recorded and
   never read while rendering. The rules for it are spelled out per object
   below.
3. **Every helper returns `an_anim`s or objects**, so `an_play`, `an_par` and
   `an_stagger` compose them with everything else. A helper that needs more
   than one track uses the same mechanism as the verbs: one `an_anim` that
   records several tracks when played.

## Array / bar visualizer

```c
typedef enum an_array_style { AN_ARRAY_CELLS = 0, AN_ARRAY_BARS } an_array_style;

typedef struct an_array_opts {
    AN_OBJ_FIELDS        /* pos: center of the row (see layout);
                            fill: cell fill (alpha 0: palette accent a);
                            stroke / stroke_width: cell outlines (default ink, 5);
                            pattern: cell fill pattern;
                            paper_under: AN_AUTO = on when the pattern isn't solid */
    const int *values;   /* copied */
    int count;
    an_array_style style;
    an_vec2 cell;        /* cells: cell size (default {96, 96}); bars: bar width and
                            the height of the largest value (default {56, 320}) */
    float gap;           /* between cells; default 0 (touching), bars default 12 */
    bool index_labels;   /* indices under the slots */
    bool no_values;      /* hide the value labels */
    an_font *font;       /* value and index labels; NULL: an_font_mono */
} an_array_opts;

an_obj *an_array(an_scene *s, const an_array_opts *opts);
an_obj *an_array_item(const an_obj *arr, int slot); /* the element in `slot` now (at the cursor) */
an_vec2 an_array_slot_pos(const an_obj *arr, int slot); /* a slot's center, in the array's space */
an_anim an_array_swap(an_obj *arr, int i, int j, const an_anim_opts *opts);
an_anim an_array_highlight(an_obj *arr, int slot, an_color fill, const an_anim_opts *opts);
an_anim an_array_unhighlight(an_obj *arr, int slot, const an_anim_opts *opts);
```

**Structure.** `an_array` returns a group (`AN_KIND_ARRAY`):
- **One element group per value:** a rect (the cell, or the bar) plus its value
  label. The two always move together, because a swap moves the element group.
- **Index labels:** children of the array, fixed under the slots. They don't
  move in a swap; they label positions, not elements.

**Layout.**
- **Cells:** slot k's center is at x = (k − (n − 1)/2) × (cell.w + gap), y = 0.
  The row is centered on `pos`.
- **Index labels:** 0.6 × cell height below the cells' bottom edge, in a dimmer
  color.
- **Bars:**
  - height = cell.h × value / max(values) (at least 4 px), with the bottoms on
    a shared baseline at y = 0;
  - the value label sits above the bar;
  - negative values aren't drawn in v1 (stated).
- **Value labels:** the mono font at 0.42 × cell height. Code-sized labels are
  calm by the text size factor, but cells keep the scene's chaos and boil.

**Rule 13: touching shapes that fade together are grouped.**
- With `gap = 0` the cells share edges. Each separately faded cell would be
  composited from its own layer, and a pale seam would show along every shared
  edge (docs/layers.md, "Conflation seam").
- The array is always one group, so `an_fade_to(arr, ...)` fades one layer and
  can't seam.
- The helpers never touch a cell's opacity. Highlighting is a fill track, so
  highlighted cells stay opaque and draw directly, with no layer per cell.
- A user who fades a single cell of a touching array gets the seam rule 13
  warns about. The docs say so at `an_array`. The library doesn't try to
  prevent it, since that would need state.

**`.paper_under` default for cells with a pattern fill.**
- A halftone or hatch cell shows what's behind it through its gaps. That's
  wanted on a page, but wrong in an array:
  - during a swap a cell passes over the other cell's arc, the index labels,
    and whatever is beneath the array;
  - a cell over the code panel would show code through its dots.
- So for cells only: if `.pattern` isn't solid, `paper_under` defaults to on.
  Pattern gaps then show the paper, as a printed cell would.
- `.paper_under` becomes an `an_toggle` (AUTO / ON / OFF) everywhere, the same
  type as `.riso` and `.shadow`. For ordinary objects AUTO means off, exactly as
  the old `false`; for array cells it means "on if patterned". `= true` in
  existing code still compiles and means ON (`AN_ON` is 1).

### Swap: arcs above the row

`an_array_swap(arr, i, j, opts)` returns one `an_anim`. Played, it records the
tracks below, a small squash-stretch on each element as it lands (verb tracks
at their ends), and the script-time slot permutation: the slots swap owners
when the swap is played (at the cursor), never at render time.

**Nothing ever goes below the row**, so the index labels and pointers there are
never crossed. **And nothing touches.**

Swaps use a *lifted* arc (an internal variant of the arc track):
- **Lift:** the element rises straight up until it clears its neighbors, and
  only then travels.
- **Height profile:** 4 H u (1 − u) up the screen, u the eased progress.
- **Travel:** a smoothstep over [lift, 1 − lift] of u, and nothing before or
  after. `lift` is the progress at which the height first reaches the
  clearance c: lift = (1 − √(1 − c/H)) / 2.
- **Why:** a plain Bézier arc (`an_move_arc`) starts moving sideways at
  once. My first version used it, and touching neighbors grazed by
  d (3u0² − 2u0³). That is a fraction of the swap distance, not of a cell:
  4.6 px for a two-slot swap, growing with distance. The test measured 4 px,
  which is how I found it.

Two cases:
- **Adjacent slots:**
  - A (from slot i) arcs over at H = max(1.5 h, 0.35 d), with clearance h
    (B's height).
  - B slides straight along the row to slot i, in the window where A's eased
    progress is in [lift, 1 − lift]. A is at least h up then, so A's bottom
    clears B's top.
- **Slots further apart: both arc above, at different heights:**
  - B arcs at H_B = max(1.5 h_mid, 0.25 d), with clearance h_mid (the tallest
    element between the slots). It travels only once it's above all of them.
  - A uses the same lift and arcs higher: H_A = H_B + 1.05 h / (4 u1 (1 − u1)).
    Here u1 = lift + k1 (1 − 2 lift) is the progress where the two first
    overlap horizontally (smoothstep S(k1) = (1 − w/d) / 2). Where their paths
    cross, A is at least B's height above B.
  - *(Changed during implementation: the first version slid B along the row
    for every swap, and that passed B straight through the elements in
    between. The first render showed it.)*

**Heights scale with distance and are clamped to the frame:**
- adjacent: H = max(1.5 h, 0.35 d);
- others: H_B and H_A as above.
- The clamp: at script time, the array's position at the cursor (its base
  state and tracks, and the camera at that time) gives the row's top edge on
  screen. If A's peak would come within 24 px of the frame's top, both heights
  are scaled down by the same factor. The swap then still runs and prints one
  warning:
  `anown: an_array_swap(4, 6): arcs clamped to 398 px by the frame (520 px
  needed); cells may touch mid-swap`.
- Things above the row (a title, the code panel) aren't avoided. Placing them
  is the script's job. At 96 px cells, an adjacent swap rises 144 px and a
  swap across two slots about 250 px.

### `an_move_arc`: the arc track

`an_move_arc(o, to, height, opts)` is a public timeline addition: a pos track
that follows a cubic Bézier instead of a straight line.

- **Shape:**
  - P0 = from (the channel's value at the start, as for any track);
  - P3 = to;
  - P1 = P0 + n c and P2 = P3 + n c, where c = 4 height / 3, so the peak
    is `height`.
  - n is the unit normal of the chord P0 → P3 that points up the screen
    (n.y < 0; for an exactly vertical chord, toward −x). A negative height
    bulges the other way.
  - The end tangents are along n, so the object lifts off and lands
    perpendicular to its travel.
- **How progress maps to position:** u = ease(τ), τ = elapsed / duration, and
  the position is B(u), the Bézier at parameter u. This is Bézier parameter,
  not arc length, so the speed along the curve follows both the ease and the
  curve. It's smooth and stated; it isn't constant speed along the path. The
  velocity (for the nozzle and take-overs) is B'(u) u'(τ) / duration,
  analytically.
- **Take-over (inherited tangent velocity):** when the arc starts while the
  channel is already moving (another track running), it inherits the
  channel's velocity v at the start as the start tangent.
  - With an ease whose start slope u'(0) is nonzero (linear), P1 becomes
    P0 + n c + v · duration / (3 u'(0)), so position and velocity are
    continuous.
  - With the default in-out ease, u'(0) = 0, so every eased track starts at
    rest (docs/timeline.md). The arc does too, and v is not used. That's the
    same rule as for straight tracks.
  - Springs aren't supported for arcs; a spring ease is treated as in-out
    (stated, and warned once).
- **The end:** from the track's end on, the position is exactly `to` (an
  ended track returns its stored end value), not B(1) recomputed.
- **Tests:**
  - B(0.5) = (P0 + 3 P1 + 3 P2 + P3) / 8, checked at the frame where the
    eased progress is 0.5;
  - the end frame is exactly `to`;
  - a take-over from a moving linear track keeps velocity continuous;
  - seeking straight into the middle of an arc (a fresh renderer, one frame)
    gives the same pixels as the sequential render.

### Highlight

- `an_array_highlight(arr, slot, fill, opts)` = `an_fill_to` on the element now
  in `slot` (its rect).
- `an_array_unhighlight` = `an_fill_to` back to the array's fill.
- They're plain fill tracks. A highlight follows its element through later
  swaps, because it's on the element, not the slot. To mark a *position*
  (quicksort's `i` and `j`), use a pointer callout (below).

## Code panel with a current-line marker

The existing `an_code` block, plus:

```c
an_obj *an_code_marker(an_obj *code, const an_code_marker_opts *opts); /* created hidden */
an_anim an_code_goto(an_obj *code, int line, const an_anim_opts *opts);
```

- **The marker:** a group with a bar behind the line (the line box, like the
  highlight bars, in its own color: the palette's accent b by default) and an
  arrow left of the block. *(As built: an arrow, not a filled ▶ triangle. There
  is no filled arbitrary polygon in the API; see the friction list.)*
  - It's created hidden at `first_line`.
  - Its first `an_code_goto` fades it in at that line's position, with no
    slide from first_line. Later ones slide it, as a pos y track, from line to
    line.
  - Its y for line n is computed from the block's layout:
    `pad_y + round(asc) + (n − first) × pitch`, plus the box offset.
- **No hidden state:**
  - `an_code_goto` is one `an_anim` on the marker's pos. A
    "first goto" (fade in, no slide) is decided at script time by whether the
    marker has any tracks yet.
  - The current line is never stored; it's the marker's sampled position.
  - `an_code_current_line(code)` (script time) returns the last line
    recorded, for scripts that branch on it.
- The per-line highlight bars (`an_code_line`) stay as they are, for
  highlighting several lines at once.

## Callouts and annotation arrows

```c
typedef struct an_callout_opts {
    AN_OBJ_FIELDS          /* pos: where the label sits; parent: attach to an object
                              (the callout then moves with it); stroke: arrow color */
    const char *text;
    float size;            /* label em size; default 34 */
    an_vec2 target;        /* the point the arrow points at, in the callout's parent's space */
    float bend;            /* arrow curvature, -1..1 (0 straight); default 0.25 */
    float gap;             /* stop short of the target by this much; default 10 */
} an_callout_opts;
an_obj *an_callout(an_scene *s, const an_callout_opts *opts);
```

- **Structure:** a group of a text label and an arrow (`an_stroke` with a
  quadratic segment and a head, through the existing stroke pipeline).
  - The arrow starts at the label's box edge facing the target and ends `gap`
    short of the target.
  - The label's alignment follows the side the target is on.
- **Drawing it on:** `an_callout_draw(callout, opts)` draws the arrow on and
  types the label. Two tracks (the arrow's draw and the label's draw) recorded
  by one anim. *(As built: `an_draw` on the callout group itself doesn't reach
  its children, so this is a separate function; see the friction list.)*
- **The arrow head** is two strokes back from the tip (±28°, 16 px), not a
  filled triangle.
- **Pointers** (quicksort's `i`, `j`, `pivot`) are callouts attached to the
  array, below the row (below the index labels), pointing up at a slot:
  `an_array_pointer(arr, "i", slot)` returns a callout.
  `an_array_point(ptr, slot, opts)` is a pos track to that slot's x. They're
  ordinary objects, so they can be faded, slapped or moved.
- **No re-aiming in 9A.** The arrow's geometry is fixed at creation (a path,
  rule 10). It moves with whatever it's attached to, and pointers are moved by
  position tracks.
- **9B design problem, not solved here:** edges whose endpoints follow moving
  nodes (graphs, trees). They need endpoints resolved after sampling: the
  nodes' positions at the frame first, then the edge geometry from them, in an
  order with no cycles. And the endpoints have to go into the tessellation
  cache key, since the edge's path would no longer be fixed at creation.

## Tests (hand-computed only)

- **Array layout:**
  - slot centers for n = 5, cell 96, gap 0: x = −192, −96, 0, 96, 192;
  - index labels' x equal their slots;
  - every cell's parent is an element group whose parent is the array;
  - bars' heights for values {1, 2, 4} at cell.h 320: 80, 160, 320.
- **Swap:**
  - The element from slot 1 is exactly at slot 3's center at the end frame,
    and vice versa.
  - At the midpoint frame, A is exactly H above the row (H by hand: 249.02 for
    a two-slot swap of 96 px cells, 144 for an adjacent one). B is halfway
    along the row (adjacent), or exactly H_B = 144 up (others).
  - A, B and the element in between never overlap at any frame (boxes from
    the sampled positions), for an adjacent swap and a two-slot swap.
  - Nothing ever goes below the row's bottom edge, so index labels and
    pointers are clear.
  - `an_array_item` after the swap returns the swapped elements.
  - A seek in reverse and shuffled order gives the same positions as forward.
- **Arc track:** the position at eased u = 0.5 is
  0.25 from + 0.5 control + 0.25 to (Bézier).
- **Highlight:** the element's fill is exactly the color at the end, and back
  to the array fill after unhighlight. It follows the element after a later
  swap.
- **Rule 13:** fading the whole array renders one layer
  (`layers_rendered == 1`), and a shared edge between two cells has no pale
  seam at opacity 0.995: the pixel equals the composite of the opaque array
  at 0.995, by hand.
- **paper_under default:** a halftone cell over a charcoal rect shows paper in
  a dot gap; a solid cell has `paper_under` off.
- **Code marker:** its bar's pixel rows equal line n's box (the same arithmetic
  as the highlight-bar test). After `an_code_goto(code, 5)` it sits exactly on
  line 5 at the end frame, and before its first goto it isn't drawn.
- **Callout:** the arrow's last centerline point is `gap` short of the target
  along the arrow's end tangent; the label's anchor follows the target's side.

## `examples/10_quicksort`

- **Content:** a 30–60 s video (input from the scene's RNG; see above):
  - the title slaps in;
  - an array of 9 values;
  - the Lomuto partition code in a panel with the marker tracking the current
    line;
  - the pivot highlighted;
  - `i` and `j` pointers;
  - swaps along arcs;
  - recursion shown by dimming the inactive ranges (fill tracks, not opacity:
    rule 13);
  - a final sorted state with a slap.
- **Script:** it runs the actual quicksort in C at script time and records the
  animation as it goes, as a user would.
- **Command:** `make example-quicksort` builds it and exports
  `out/quicksort.mp4` unattended (share mode), plus every 10th frame as PNGs.
- **Friction:** while writing it I'll list every place where the API was
  awkward or missing, without working around it quietly in the library.

## Parallel with a gap

No new helper. "Start the swap, and 0.2 s later start moving the code marker,
then continue when both are done" is:

```c
an_play_async(s, an_array_swap(arr, i, j, NULL));   // starts at the cursor
an_wait(s, 0.2f);
an_play(s, an_code_goto(code, 6, NULL));            // starts 0.2 s later, cursor to its end
```

The example uses this, and the friction list says whether it was awkward (for
instance, `an_play` only waits for its own anim: if the swap is longer, the
cursor stops before the swap ends).

## Quicksort input

The values come from the scene's seeded RNG (`an_scene_rng`): 9 cells, values
1..20 with no repeats. Value text is at least 30 px (cells 96 px tall: labels
at 40 px).

## Commits

One per object type, each with its tests and docs:
1. the arc track and the array / bars (swap, highlight, permutation);
2. the code marker;
3. callouts and pointers;
4. the example, `make example-quicksort`, and the API-friction list.

## API friction from writing `examples/10_quicksort` (2026-11-25)

Written the way a user would write it. Nothing below was worked around inside
the library; each item shows how the script coped.

1. **Parallel with a gap is awkward.** `an_play_async` returns nothing, so the
   script can't know when the async anim ends.
   - "Start the swap, 0.15 s later move the code marker, continue when both are
     done" became
     `an_play_async(swap); an_wait(0.15); an_play(goto); an_wait(0.9 - 0.15 - 0.35 + 0.1)`,
     with the swap's and goto's default durations typed in by hand.
   - Change a default and the script silently overlaps.
   - Wanted: `an_play_async` returning the anim's end time, plus an
     `an_wait_until(s, t)`, or a join (`an_play_all` that takes per-anim offsets).
2. **Default durations are invisible.** The script has to know that a swap is
   0.9 s and a goto 0.35 s to compose them (see 1). Wanted: a way to ask an anim
   how long it will run (`an_anim_duration`), and better, item 1.
3. **"Invisible until my verb starts" needs per-object opacity flips.** A pop
   starts from scale 0, but only once its track starts; before that the element
   shows at scale 1.
   - The script sets every element's opacity to 0 at the start, then to 1 at
     the moment of its pop (and the same for the title's letters).
   - It's correct, but only because the flip is instant. A fade on a cell of a
     touching array would seam (rule 13).
   - Wanted: verbs that own the "before" state (`.hold_from = true`: the from
     value applies from the object's birth until the verb starts).
4. **`an_array_pointer` takes no options.** There's no `.hidden`, size, color
   or offset.
   - The script hides pointers with opacity setters right after creating them.
   - The fixed geometry puts the pointer labels 2.2 cell heights below the row.
     Together with the swap arcs' room above it (also only in the doc), that
     fixed the whole layout: the array had to sit at y 800 with 80 px cells.
   - Wanted: `an_array_pointer_opts`, and a function that reports the space a
     swap needs above the row.
5. **`an_draw` on a group doesn't reach its children.** Hence
   `an_callout_draw`. The same will come up for any composite object; a
   group-level draw that spreads over the children (like a text's) would be
   more uniform.
6. **No filled arbitrary polygon.** Only rects, circles and glyphs fill. The
   code marker's triangle became an arrow, and callout heads are two strokes.
7. **`NULL` wasn't available from `anown.h`**, although passing NULL for "all
   defaults" is the documented style. Fixed: anown.h includes <stddef.h>.
8. **No script-time query of an object's state at the cursor.** `an_obj_get`
   reports the frame last passed to `an_seek`, not the cursor. So the script
   keeps its own `done[]` array to know which cells are green before dimming
   the others.
   - Wanted: `an_obj_get_at_cursor`, or `an_obj_get_at(o, seconds)`.
9. **Highlight / unhighlight only knows one base color.** With three states
   (active, dimmed, done), the script stops using unhighlight and paints fill
   tracks itself (`paint()`). Fine, but the helper didn't help.
10. **Code lines are referenced by number** (`line(5)`). Edit the snippet and
    every number in the script shifts. Wanted: named lines in the code text
    (a trailing `// @swap` marker, stripped from the display) and
    `an_code_goto_label(code, "swap")`.
11. **Layout needs numbers that live in the docs.** Arc heights (how much room
    a swap needs above the row) and pointer distances (below it) can't be asked
    for. The layout was found by reading docs/explainer.md, not by asking the
    API.

Parallel with a gap (doc item 5): demonstrated in `swap_slots`; it is awkward
(item 1).

## API fixes from the friction list (2026-11-25)

### Script-time queries and named states (friction 8, 9)

- `an_obj_at_cursor(o)`: the object's state at the cursor's time, from the
  tracks recorded so far. It's pure: it samples tracks like rendering does, at
  a different time.
- **Named states:** `an_cell_state` is BASE, ACTIVE, DIMMED or DONE, with
  colors in `an_array_opts.state_colors`.
  - Defaults: the fill, the palette's accent b, paper mixed 12% toward ink, and
    the palette's accent c.
  - `an_array_set_state(arr, slot, state, opts)` records a fill track to the
    state's color and an instant step on the cell's `AN_PROP_CELL_STATE`.
  - `an_array_state_at_cursor` reads that property. The state is set at the
    step, even while the fill is still changing, and it follows the element
    through swaps.
  - It's ordinary track data, not hidden state.
- The quicksort script's `done[]` array and its `paint` helper are gone.

### Containers (friction 4, 5)

The text rule, generalized to every container (group, array, element group,
callout, code block, text):

- A child's **fill, stroke, stroke width and draw progress** follow the
  nearest ancestor that *provides* them at that time, unless the child was
  overridden.
  - Provides: a setter on the ancestor, or a track of its own that has started.
    Containers in between pass it through.
  - Overridden: by a setter on the child itself at creation (for good), or by
    a track of the child's own (from that track's start).
- **How a child follows:**
  - From a setter (the container's base): the child takes the value.
  - From a track with an explicit start (`an_draw`, verbs): the child takes the
    container's value as is, since the deliberate start applies to it too.
    `an_draw(group)` draws every child on together.
  - From a continuation track (`an_fill_to`, `an_stroke_to`,
    `an_stroke_width_to`): each child goes from its own value just before the
    track started to the track's target, with the track's timing:
    child = own + f (target − own), with f the track's progress (measured on its
    widest component, in OKLab for colors). From the track's end on, it's
    exactly the target.
  - Without this, a child would jump to the container's own base value (black,
    for a group nobody colored) when the track started.
- **Letters** keep their rule: they follow their text's paint directly (they
  were created with it), and the text spreads its draw progress over them.
- **Opacity** still multiplies down the tree.
- So `an_callout_draw` is gone: `an_draw(callout)` reaches the arrow and the
  label.
- `an_array_pointer(arr, slot, &(an_pointer_opts){ .label, .hidden, .size,
  .color, .offset })`. The offset is from the row's bottom edge to the arrow's
  tip.
