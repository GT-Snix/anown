# API fixes after 11_bst: design

Status: approved, revision 2 plus two additions (2026-11-25).
Revision 2 applies the review: the axes point is renamed, the size rule for the
caps is defined, text versions are bounded and the diff is specified, there are
pop and pan tests for edges, and the draw order in a graph is stated.

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
- **Draw order inside the graph's group** (rule 8 applies: siblings are sorted
  by z, then creation id):
  - Edges get z −1 and nodes z 0 unless opts give a `.z`. So every edge is
    drawn under every node, whatever order they were created in.
  - Edges draw among themselves in creation order, and nodes likewise.
  - An arrowhead is its edge's child, so it draws with its edge (under the
    nodes).
  - A node's shadow therefore falls on the edges beneath it.
  - Anything else the user parents to the graph is z 0 by default, so it draws
    among the nodes in creation order. A label meant to sit over everything
    needs a higher z.

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
    bool together;           /* all steps at once (added while implementing: with zero
                                meaning the default, gap 0 can't say "at once") */
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
- **Queries resolve to the latest version:**
  - `an_text_glyph`, `an_text_length`, `an_obj_bounds_at_cursor(text)` and
    `an_obj_at_cursor` on a letter all answer for the latest version whose
    `an_text_set` starts at or before the cursor.
  - So mid-crossfade they already see the new text.
  - `an_obj_at_cursor(text)` itself is the container's state (opacity, pos,
    ...); no version can change that.
- **Bounds on versions:**
  - Objects can't be freed one by one (rule 2: the arena), so old versions
    aren't freed.
  - **Render cost is bounded:** each version records its live interval, from
    the start of its fade-in to the end of its fade-out with no later track on
    it. The tessellator skips a version outside its interval with one
    comparison. Only versions alive at that frame cost anything: at most two
    in CROSSFADE, a few in LETTERS (the letters that glide are reused, not
    copied).
  - **Memory is bounded:** at most 256 versions per text, version 0 included.
    The 257th `an_text_set` on one text fails, loudly:
    - It's a script-time error naming the text: its name if it has one, else
      its creation string. For example:
      `anown: an_text_set on text "insert 42": 256 versions already (the limit)`.
    - It returns a failed anim: `an_anim_ok(a)` is false.
    - Playing a failed anim records nothing, and returns a handle at the
      cursor whose `ok` is false.
    - The text doesn't change. Nothing is dropped silently.
  - A version costs its letters' objects (about 300 bytes each, from the
    arena).

**Modes:**
- **CROSSFADE:**
  - The old version fades out and the new one fades in. Each version is one
    layer: the letters of a version batch and fade together, and rule 13
    doesn't apply since versions don't share edges by design.
  - **`stagger`** (an option, 0..1, default 0) controls the overlap. With
    total duration D, each fade lasts f = D / (1 + stagger), and the fade-in
    starts stagger × f after the fade-out.
    - stagger 0: both run over all of D (a full crossfade).
    - stagger 1: f = D/2, so the old text fades out and then the new fades in.
    - In between, they overlap partly.
- **LETTERS:**
  - Old and new strings are matched by longest common subsequence (LCS) of
    codepoints, as one string each (newlines and spaces included).
  - The table is the usual one: L[i][j] is the LCS length of the first i old
    letters and the first j new ones.
  - The match is read back from (n, m):
    - if old[i−1] == new[j−1], match them and step diagonally;
    - otherwise step up (drop old letter i−1) when L[i−1][j] ≥ L[i][j−1], else
      left (drop new letter j−1).
  - **Tie-break:** on equal lengths it drops the old letter, so with repeated
    letters the match goes to the latest copies. That's deterministic and needs
    no hashing.
  - Only codepoints that have glyph objects animate. Spaces take part in the
    match but have nothing to move.
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
- **CROSSFADE, stagger 0.5, D 0.3 s:**
  - Each fade is f = 0.2 s, and the fade-in starts at 0.1 s.
  - At 0.1 s: old 0.5, new 0 (linear).
  - At 0.2 s: old 0, new 0.5.
- **LETTERS, "insert 42" → "insert 17"** (mono font, centered):
  - The LCS is "insert " (L = 7).
  - The 6 letters i n s e r t are matched, each with zero movement: same
    length, same mono advance, so the same positions.
  - "4" and "2" fade out, starting at 0 and gap; "1" and "7" fade in, starting
    at 0 and gap.
  - At the end, the letter positions equal a fresh `an_text("insert 17")`.
- **Repeated letters, "abab" → "ba"**, by hand:
  - L[4][2] = 2.
  - At (4, 2): b ≠ a; L[3][2] = 2 ≥ L[4][1] = 1, so step up.
  - At (3, 2): a = a, so old 2 matches new 1.
  - At (2, 1): b = b, so old 1 matches new 0.
  - Kept: old letters 1 and 2, which glide to new slots 0 and 1. Old 0 and old
    3 fade out; nothing fades in.
- **The 257th set:** 255 sets succeed (256 versions); the next fails.
  - `an_anim_ok` is false, and the message names the text (stderr captured
    in the test).
  - Playing the failed anim leaves the cursor and the track count unchanged.
  - The text still shows the 256th version.
- **Repeated letters, "aa" → "a":** the old letter 1 is kept (the latest copy,
  by the tie-break), and old 0 fades out.

## 3. `an_axes_point` in scene space

```c
an_vec2 an_axes_point_at_cursor(const an_obj *axes, float x, float y); /* scene space, at the cursor */
an_vec2 an_axes_local(const an_obj *axes, float x, float y);           /* the axes' own space */
```
- `an_axes_point` is removed. The name says the answer is a snapshot at the
  cursor, like the other `_at_cursor` queries.
- **Scene space:** `an_axes_point_at_cursor` composes the axes' world
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

**What the offsets are measured in.** The riso and shadow offsets are
translations applied after the object's full transform for that frame, `w` in
the tessellator. That transform includes:
- parents, scale and rotation;
- the verb transform (slap, pop, squash);
- the camera's zoom and rotation (on the pan-only fast path the camera is a
  per-draw offset, but then its zoom is 1).

So the offsets are on-screen pixels, and the size is measured the same way.

**Size:**
- `size` = the **larger** side of the object's local bounding box (fixed at
  creation; for text, the em size, since letters of one text share it) × √|det|
  of that frame's `w`.
- So it uses the frame's own effective scale, verbs and camera zoom included.
- The larger side, not the shorter, so open strokes and thin lines (a
  horizontal line's box is 0 tall) are measured by their length, and are
  capped only if they are short.

**The cap** is continuous:
- riso' = riso × min(1, 0.10 × size / |riso|)
- shadow' = shadow × min(1, 0.20 × size / |shadow|)
- Here `shadow` is the style's shadow offset times chaos, before the ink width
  is added (the "beyond the ink" part; the ink-width term is unchanged).
- **A zero-length offset is returned unchanged** (no division: the min is
  taken only when |offset| > 0). Chaos 0, or a style offset of (0, 0), stays
  exactly (0, 0).
- The direction is kept. The cap applies after chaos. There is no step: as an
  object shrinks, its offsets shrink smoothly.
- Text keeps its existing size-based riso scaling (`size_chaos`), and the cap
  applies on top of it.

**Thresholds at the defaults:**

| Effect | Default offset | Length | Cap starts below |
|---|---|---|---|
| riso | (4, −3) | 5 | 50 px |
| shadow | (8, 8) | 11.31 | 56.6 px |

- Not touched: array cells (80 px), nodes (76 px with their outline), titles.
- 22 px plot points get a riso of 2.2 px and a shadow of 4.4 px.

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

**Tests** (the capped offsets are read from the emission style, an internal
function, and checked against hand values; plus pixels):
- **Just below and just above the thresholds** (chaos 1):

  | Square | Riso cap | Riso length | Shadow cap | Shadow length |
  |---|---|---|---|---|
  | 49 px | 4.9 (capped) | 4.9 | 9.8 (capped) | 9.8 |
  | 51 px | 5.1 (no cap) | 5.0 | 10.2 (capped) | 10.2 |
  | 57 px | 5.7 (no cap) | 5.0 | 11.4 (no cap) | 11.31 |
  | 200 px | — | 5.0 | — | 11.31 |

- **Pixels:** an 8 px square's riso (0.8 px) and shadow (1.6 px) edges, at
  chaos 1, against a hand-computed column of pixels, with effects other than
  riso/shadow off.
- **Slapped:** a 60 px square, slapped. At a frame where the sampled verb
  scale is s (read from the track), size = 60 s, and the riso is
  min(5, 6 s) by hand. So it's capped while s < 0.833 and uncapped at the
  end (s = 1).
- **Camera:** the same 60 px square under camera zoom 0.5 (size 30) is capped
  to riso 3.0 and shadow 6.0.
- **Zero length:** chaos 0 and an explicit riso offset of (0, 0) each give
  exactly (0, 0), with no NaN, on an 8 px square (which would be capped
  otherwise).
- **Line:** a 300 px horizontal line is uncapped. Its larger side is 300,
  although its box is 0 tall.
- **Cache:** `test_memo` gains a square scaling from 30 to 70 px, crossing
  both thresholds.

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
- **Added while implementing:**
  - `an_graph_opts.layout`: the initial layout opts. Without them, the first
    child of a graph is placed with the default opts before any
    `an_graph_layout_tree` call can set them.
  - `an_graph_layout_tree` stores opts even on an empty tree.
- **Friction (open):** `an_graph_child` makes the edge drawn. 11_bst wants it
  undrawn until after the glide, so it calls `an_obj_set_draw(edge, 0)` (found
  with `an_graph_edge_between`). An `an_edge_opts *` on `an_graph_child` would
  remove that.
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
- **Pop:** a node of r 30 popping in. At a frame mid-pop where the sampled
  verb scale is s (read from the track), the edge end is center + (30 s + 6)
  along the edge direction, by hand from s. At the pop's end it's center + 36.
- **Slap:** a node of r 30 slapped at a frame where its sampled verb scale is
  s (read from the track). The edge end is center + (30 s + gap) along the
  direction, by hand from s. At the slap's end (s = 1) it's back to 36.
- **Camera pan:** the same graph rendered at camera (960, 540) and
  (860, 540), on the pure-translation fast path, effects off. The panned frame
  equals the unpanned one shifted 100 px, pixel for pixel, in the region
  covering both nodes and the edge.

## Commits

One per item, in this order: 5, 1, 9, 6, 7, 3, 8, 2, then 4 last.

- **5 (the rename) comes first,** and every golden hash is unchanged: quicksort
  and bst both identical.
- **Every commit before 4 leaves quicksort identical.**
- **11_bst is rewritten with the new API** across items 1, 6, 3 and 2.
  - Where the new API does the same thing as the old code, its frames must
    match `bst_5d0ab00` exactly.
  - Every remaining difference is listed in that commit, with its frames and
    its cause. Expected:
    - **Item 1:** the edges' colors on the path now follow the graph's ACTIVE
      color. The old ones were the same pink, so this may be identical.
      Possibly also the edge z, if creation order changed the stacking.
    - **Item 2:** the caption's change animation (crossfade in place of the
      fade-out/fade-in pair).
- **Stops for review:** after the 11_bst rewrite and after item 4, where a
  golden file changes, the differences are reported before going on.
- **No implementation commit is pushed without asking.**
- **4 is last and alone.** After it, both golden files are re-recorded as
  `quicksort_caps.sha256` and `bst_caps.sha256`, keeping the earlier ones.
  That's stated as an intended look change, with the changed frames counted
  and the objects responsible named.

Then stop.
