# API fixes after 11_bst: design

Status: proposal (2026-11-25), waiting for approval (CLAUDE.md rule 18).

This answers the friction list in docs/graphs.md with general mechanisms, not
example-specific patches. There are nine items, one commit each, in the order
below.

**Gate.** Golden hashes were recorded before any change:
- `out/golden/quicksort_5d0ab00.sha256` (273 frames). This is identical to
  `quicksort_treekeys.sha256`, so nothing since the tree-keys change touched
  quicksort.
- `out/golden/bst_5d0ab00.sha256` (263 frames; a second run matched).
- Both are checked with `scripts/example-golden.sh check EXAMPLE PREFIX MANIFEST`.
- After each commit both are checked. Every changed frame is explained in the
  commit message and in docs/progress.md, as for the polygon and tree-key
  changes. A change I can't explain blocks the commit.

## 1. Graphs own their edges

```c
typedef struct an_graph_opts {
    AN_OBJ_FIELDS
    bool directed;               /* default for its edges */
    an_color state_colors[4];    /* ACTIVE / DIMMED / DONE shared by nodes and edges;
                                    [AN_STATE_BASE] is ignored (each keeps its own base) */
} an_graph_opts;
an_obj *an_graph(an_scene *s, const an_graph_opts *opts);           /* a group: AN_KIND_GRAPH */
an_obj *an_graph_node(an_obj *g, const an_node_opts *opts);           /* a child of g, registered */
an_obj *an_graph_edge(an_obj *g, an_obj *a, an_obj *b, const an_edge_opts *opts);
an_obj *an_graph_edge_between(const an_obj *g, const an_obj *a, const an_obj *b);
```

**Ownership:**
- The graph keeps a list of its nodes and an edge list `{a, b, edge}` in the
  scene arena, in creation order.
- `an_graph_edge` returns the edge object, which is the handle for states,
  draw-on and fades. It rejects:
  - nodes that aren't the graph's;
  - a second edge between the same ordered pair (it returns the existing one,
    with a message).
- **Draw order:** edges are drawn under nodes without the user setting z. The
  graph gives edges z −1 and nodes z 0 (an explicit `.z` in opts still wins).
  So creating an edge before its nodes are drawn is no longer needed.

**Lookup:** `an_graph_edge_between(g, a, b)` returns a→b if it exists, else
b→a, else NULL. Searched in creation order, so the result is deterministic.

**States:**
- Edges get the same named states as nodes (item 5: `AN_STATE_*`), with
  `an_edge_state_at_cursor(edge)`.
- Colors come from the graph's `state_colors`, so a path's nodes and edges
  match by default (friction 1).
- `an_edge_opts` also gets `state_colors[4]`, to override per edge, as nodes
  can.
- Edge BASE is its stroke color; node BASE is its fill.

**Path highlights take nodes:**
```c
typedef struct an_path_opts {
    float gap;               /* seconds between steps; default 0.25 */
    float duration;          /* each object's transition; default 0.3 */
    an_ease ease;
} an_path_opts;
an_anim an_highlight_path(an_obj *const *nodes, int n, an_state state, const an_path_opts *opts);
```
- The nodes must belong to one graph, found from `nodes[0]`'s parent.
- Between consecutive nodes the edge is looked up with
  `an_graph_edge_between` and styled in sequence: node, edge, node, edge, ...
  Each step starts `gap` after the previous one.
- A missing edge is reported once (with the two node indices) and skipped; the
  nodes are still highlighted.
- `an_edge` (a free edge, not in a graph) stays for one-off arrows between
  objects.

**Tests (hand-computed):**
- The edge list order.
- `edge_between` in both directions, and NULL for a pair with no edge.
- A 3-node path: the start times of its 5 objects are 0, g, 2g, 3g, 4g.
- At the end, node and edge colors equal the graph's ACTIVE color exactly.
- A missing edge: 2 objects get tracks, not 3.

## 2. `an_text_set`: text that changes on the timeline

```c
typedef enum an_text_change { AN_TEXT_CROSSFADE = 0, AN_TEXT_LETTERS } an_text_change;
typedef struct an_text_set_opts {
    an_anim_opts anim;       /* default 0.3 s */
    an_text_change mode;
    float gap;               /* LETTERS: stagger between letters; default 0.03 */
} an_text_set_opts;
an_anim an_text_set(an_obj *text, const char *str, const an_text_set_opts *opts);
```

**Model:**
- A text object holds **versions**. Version 0 is the creation string, with its
  letters exactly as today, so existing scenes don't change.
- Each `an_text_set` adds a version at script time: the new string is laid out
  with the text's own font, size, alignment, anchor and kerning, by the same
  layout function. The layout is deterministic: same inputs, same positions.
- The letters are flattened once per (font, glyph) as always (rule 14).
- Version v's letters are parts `"v<v>"`, index k, so their random streams
  don't depend on other versions.
- `an_text_glyph` / `an_text_length` answer for the version current at the
  cursor.

**Modes:**
- **CROSSFADE:** the old version fades out and the new one fades in, over the
  same span. Each version is one layer (letters of a version batch and fade
  together; rule 13 doesn't apply since versions don't share edges by design).
- **LETTERS:**
  - Old and new strings are matched by longest common subsequence of
    codepoints. Ties go to the earliest match, so the result is deterministic.
  - Matched letters glide from their old place to their new one; the glide
    reuses the old letter object, with no new geometry.
  - Unmatched old letters fade out and unmatched new ones fade in, `gap`
    apart, left to right.
  - So "insert 42" → "insert 17" keeps "insert " still and swaps the digits.
    A centered string whose width changes slides its kept letters, not
    flickers.

**Tests:**
- Version 0 unchanged: existing text tests and reference frames are
  identical.
- After a set, the letter positions at the cursor equal a fresh `an_text` with
  the new string at the same opts, exactly.
- CROSSFADE at half the duration: old opacity 0.5, new 0.5 (linear).
- LETTERS on "insert 42" → "insert 17": 7 letters matched with zero movement;
  2 fade out, 2 fade in, with start times 0 and gap.
- The LCS tie case "abab" → "ba" matches by hand.

## 3. `an_axes_point` in scene space

```c
an_vec2 an_axes_point(const an_obj *axes, float x, float y);  /* scene space, at the cursor */
an_vec2 an_axes_local(const an_obj *axes, float x, float y);  /* the axes' own space */
```
- **Scene space by default:** `an_axes_point` composes the axes' world
  transform at the cursor (parents, scale, rotation, and verb transforms,
  through `ani_obj_world_at`). That's the space a callout or any top-level
  object wants (friction 7).
- **Local space:** `an_axes_local` is the old behavior. Plots use it
  internally, since they are children of the axes.
- **Limit:** a value at the cursor. If the axes move later, a point taken
  earlier doesn't follow. Parent the object to the axes and use
  `an_axes_local` for that.
- **Tests (hand-computed):**
  - Axes at (200, 100), scale 2: data (5, 2.5) → local (400, 250) → scene
    (1000, 600).
  - The same axes inside a group at (50, 30) rotated 90°: by hand.
  - Plots are unchanged under both.
- 11_bst's callout drops its `+ axes pos`. Its frames must stay identical.

## 4. One rule for effects on small objects

**The cap:** the riso and shadow offsets are capped by the object's size on
screen, measured in scene px:
- `size` = the shorter side of the object's local bounding box (fixed at
  creation; for text, the em size) × its world scale (√|det| of the world
  transform, verb included).
- **Riso:** the offset is at most 0.10 × size.
- **Shadow:** the offset beyond the ink is at most 0.15 × size.
- The vector keeps its direction; only its length is cut. The cap applies
  after chaos scaling.

At the defaults (riso (4, −3), length 5; shadow 8 beyond the ink), only objects
smaller than about 50 px (riso) or 55 px (shadow) are touched. Array cells
(80 px), nodes (76 px) and titles keep their look. 22 px plot points get a riso
of 2.2 px and a shadow of 3.3 px.

**Cache key:** the cap is computed from the world transform (in the key) and
the creation-time box (fixed per object identity). Nothing new to key; the
memo test is extended with a scaling object to prove it.

**Callout labels:** shadow off by default, like code text. `.shadow = AN_ON`
brings it back.

**Expected golden changes** (to be explained frame by frame):
- **quicksort:**
  - the index labels and pointer labels (small text);
  - the pointer triangles;
  - the code marker triangle, if it's under the threshold;
  - the "sorted!" label (shadow off).
- **bst:** the plot points, the tick labels and the callout label.

**Tests:**
- An 8 px square at chaos 1: the shadow and riso offsets measured in pixels
  equal the capped values by hand.
- A 200 px square is unchanged.
- The same 8 px square scaled ×10 is uncapped.

## 5. `AN_STATE_*`

- `an_cell_state` becomes `an_state`, with `AN_STATE_BASE`, `_ACTIVE`,
  `_DIMMED` and `_DONE`. The internal `AN_PROP_CELL_STATE` becomes
  `AN_PROP_STATE`.
- No aliases: the project is private and pre-1.0. Examples, tests and docs are
  updated in the same commit.
- `an_array_set_state` / `an_node_set_state` / `an_edge_set_state` keep their
  names.
- **Golden:** none can change. Verified identical.

## 6. Layouts with an origin, written into the tree

```c
typedef struct an_layout_opts {
    an_vec2 spacing;        /* default {100, 100} */
    an_vec2 origin;         /* where the root (tree) or the top middle (layered) goes */
} an_layout_opts;
bool an_layout_tree(const an_layout_node *nodes, int n, const an_layout_opts *opts, an_vec2 *out);
bool an_layout_layered(int n, const int (*edges)[2], int m, const an_layout_opts *opts, an_vec2 *out);
an_anim an_layout_animate(an_obj *const *objs, const an_vec2 *to, int n, const an_anim_opts *opts);
```
- The pure layouts gain `origin`. `an_tree_node` becomes `an_layout_node`,
  since `an_tree_*` is wanted for trees.
- **`an_layout_animate`** glides every object from wherever it is at the start
  (continuation tracks) to `to[i]`, as one anim and one handle. The arrays are
  copied into the scene. It's the "glide between two layouts" step: compute the
  new layout, animate to it.
- **Trees in a graph** (written into the existing tree):
  ```c
  an_obj *an_graph_child(an_obj *g, an_obj *parent, int side, const an_node_opts *opts); /* node + edge */
  an_anim an_graph_layout_tree(an_obj *g, const an_layout_opts *opts, const an_anim_opts *anim);
  ```
  - `an_graph_child` records the parent and side (−1 / +1 / 0) and makes the
    edge.
  - `an_graph_layout_tree` lays out the graph's own tree. It returns an
    `an_layout_animate` of all nodes (duration 0: an instant set at the
    cursor).
  - A node created after layout is placed by the next layout: its creation
    position is its slot in the layout computed at that moment, so the 11_bst
    pattern "pop in place while the others glide" is one call each.
- **Tests:**
  - The existing layout cases with an origin of (960, 300): every position
    shifted exactly.
  - `an_layout_animate` at half-way (linear): exact midpoints. A mid-flight
    retarget continues from the sampled point.
  - `an_graph_layout_tree` on the 7-key BST equals the pure function's output
    plus the origin.

## 7. Containers by default

- `an_graph` (items 1 and 6) returns a group, and everything it makes is
  inside it. Fading, moving or slapping the graph acts on the whole tree as one
  layer (rule 13), with no planning ahead (friction 10).
- Arrays already are containers: cells, labels and pointers are children of
  the array. A test makes that explicit: a faded array is one isolated layer,
  pointers included.
- Callouts and code panels already are. Plots are children of their axes.
- This item is mostly about trees (there is no separate tree object; a tree is
  a graph built with `an_graph_child`), plus the array test.
- **Golden:** none expected.

## 8. Data points keep their index

- A point outside the axes' range is still created at its unclamped mapped
  position, with a flag that the tessellator skips. So `an_plot_item(p, k)` is
  always the k-th input.
- Animating it is harmless: it never draws.
- Bars already keep their index (clamped).
- The message stays, with the indices.
- **Test:** values {3, 7, −1} on 0..5 → items 0, 1 and 2 exist, only item 0
  draws (pixels at the other two's spots are paper).
- **Golden:** none expected.

## 9. Edge endpoints, verbs and the camera (stated)

**Verbs:** edge endpoints come from each node's world transform at the frame,
which includes its verb transform (scale and rotation about its pivot; verbs
don't translate).
- A slapped node's outline swells, and its edges' ends follow the outline, so
  they stay touching. `gap` is in the edge's parent space and doesn't scale.
- A squash (non-uniform scale) clips at the true ellipse.

**Camera:** endpoints are resolved in scene space, before the camera. A pan,
zoom or shake moves nodes and edges together, because the camera is applied to
the finished frame's geometry. Edges never see the camera.

**Tests:**
- **Slap:** a node of r 30 slapped at a frame where its sampled verb scale is
  s (read from the track). The edge end is center + (30 s + gap) along the
  direction, by hand from s. At the slap's end (s = 1) it's back to 36.
- **Camera pan:** the same graph rendered at camera (960, 540) and
  (860, 540), on the pure-translation fast path, effects off. The panned frame
  equals the unpanned one shifted 100 px, pixel for pixel, in the region
  covering both nodes and the edge.

## Commits

One per item, in this order: 5 (rename: no pixel change, so the other diffs
read cleanly), 1, 9, 6, 7, 3, 8, 2, 4. 11_bst is rewritten with the new API
across items 1, 6 and 2, and the golden diffs are explained at each step. Then
stop.
