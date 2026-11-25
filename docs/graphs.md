# Graphs, trees and plots (9B): design

Status: proposal (2026-11-25). Scope: edges between nodes, deterministic layouts
(tidy tree, layered), node shapes with labels, highlight states for nodes, edges
and paths, axes with a function plot and a data plot. Then `examples/11_bst`.

## 1. Edges

```c
typedef struct an_edge_opts {
    AN_OBJ_FIELDS          /* stroke / stroke_width: the line (default ink, 5) */
    an_obj *from, *to;     /* nodes (any objects that aren't edges) */
    bool directed;         /* an arrowhead at `to` */
    float bend;            /* -1..1: curvature (0 straight), as for callouts */
    float gap;             /* stop short of each node's outline; default 6 */
} an_edge_opts;
an_obj *an_edge(an_scene *s, const an_edge_opts *opts);
```

**Endpoints are resolved after sampling, every frame.**
- An edge's geometry isn't fixed at creation like other paths (rule 10).
  At each frame, the tessellator takes both nodes' world transforms at that
  frame (sampled with `ani_obj_world_at`), clips the line to each node's
  outline plus `gap`, and builds the path in the mesh's scratch memory.
- That path is a straight line, or a quadratic for a bend, flattened at the
  scene's tolerance. It's stroked through `ani_stroke_emit` as usual, so
  brush, boil, riso and shadow all apply.
- Nothing is written to the edge object, so rule 10's "rendering never writes
  to objects" holds. Only "flattened once at creation" is relaxed, for edges
  only, and that is stated in rule 10.
- **The node's outline:** circles clip at their radius on screen. Rounded
  rects (and any other shape) clip at their box, along the line through the
  center.
- **Draw-on:** `an_draw(edge)` is by arc length along the resolved path; the
  arrowhead pops in at 1 (like polygons).

**No cycles, rejected at script time.**
- An edge's endpoints must not depend on the edge. `an_edge` returns NULL (with
  a message) if `from` or `to` is an edge, is the edge itself, or is a
  descendant of an edge.
- Nodes' positions come from their own tracks, which never read edges, so
  that's the only way to make a cycle.
- `an_obj_set_parent` refuses (returns false) to move a node under an edge that
  uses it.

**Cache key.** The key gets the resolved geometry inputs: both endpoints' world
transforms at the frame, and their outline sizes. A node that moves makes its
edges miss; a still graph keeps hitting.

**Arrowheads use `an_polygon`.**
- A directed edge gets a child polygon (its part `head`): a triangle with its
  tip at the origin, pointing along +x.
- When the edge is drawn, its children are visited with the resolved tip frame
  (translate to the tip, rotate to the end tangent) as their parent transform.
  So the head is an ordinary filled polygon, with boil, riso, shadow and fill
  timing, that moves with the edge.

## 2. Deterministic layouts (script time)

Layouts are pure functions from a structure to positions. They return
positions, and the script applies them, as base positions (setters at
creation) or as moves (`an_move_to` in an `an_par`), so a layout can animate.

```c
typedef struct an_tree_node { int parent; int side; } an_tree_node; /* parent -1: root; side
                                    -1 left, +1 right, 0 n-ary order (index order) */
void an_layout_tree(const an_tree_node *nodes, int n, an_vec2 spacing, an_vec2 *out);
void an_layout_layered(int n, const int (*edges)[2], int m, an_vec2 spacing, an_vec2 *out);
```

**Tidy tree:**
- *Binary trees* (all sides given): x = the node's in-order rank × spacing.x,
  y = depth × spacing.y. In-order ranks never collide, left subtrees sit left of
  their parent, and a lone right child sits to the right, as a BST needs.
- *n-ary trees* (side 0): leaves left to right in pre-order, one slot each, and
  every parent centered over its first and last child.
- Both are recentered so the root is at x = 0, y = 0.
- **Determinism:** integer ranks and slots, then one multiply. No iteration,
  no floats accumulated in an order that could change.

**Layered** (for DAGs; cycles broken by ignoring back edges in index order):
- **Layers:** each node's layer is its longest path from a source.
- **Order within a layer:** start from index order, then **8 fixed sweeps** of
  the barycenter heuristic (down, up, down, ...). Ties are broken by the
  previous order, then by index.
- **Placement:** evenly spaced in each layer, centered on x = 0;
  y = layer × spacing.y.
- **Determinism:**
  - The iteration count is fixed. The order of operations is index order, and
    the starting order is index order: deterministic by construction, so no
    seed is needed.
  - Barycenters are means over neighbors summed in index order. Sums are double,
    sequential, no parallel reductions.
  - If a randomized start is wanted later, it takes an `an_rng *` explicitly
    (the scene's stream), never a global.

A force layout isn't in 9B. Layered covers DAGs, and the tidy tree covers trees.

## 3. Nodes

```c
typedef enum an_node_shape { AN_NODE_CIRCLE = 0, AN_NODE_ROUNDED } an_node_shape;
typedef struct an_node_opts {
    AN_OBJ_FIELDS          /* fill / stroke / pattern / paper_under: the shape's */
    an_node_shape shape;
    float radius;          /* circle; default 40 */
    an_vec2 size;          /* rounded rect; default {120, 70}; corner radius = 0.25 x the shorter side */
    const char *label;     /* centered; mono at 0.5 x radius (circle) or 0.4 x height */
    an_font *font;
    an_color state_colors[4]; /* as for arrays */
} an_node_opts;
an_obj *an_node(an_scene *s, const an_node_opts *opts);
```

- **Structure:** a node is a group, `AN_KIND_NODE`, holding its shape (part
  `shape`: a circle, or a rounded rect built from four quarter arcs) and its
  label (part `label`). Edges clip at the shape.
- **Halftone nodes default to `.paper_under`:** if the pattern isn't solid,
  `paper_under` AUTO is on. Edges are drawn behind nodes (create them first, or
  give them a lower z), and the paper keeps them from showing through the dot
  gaps.

## 4. Highlight states

Nodes and edges get the array's four named states (BASE, ACTIVE, DIMMED, DONE).
`an_cell_state` is reused; the name is now a misnomer (see friction).
- `an_node_set_state(node, state, opts)` records a fill track on the shape and a
  step on its state property.
- `an_edge_set_state(edge, state, opts)` records stroke color and width tracks
  (active and done edges 1.5x wider).
- `an_node_state_at_cursor(node)`.
- `an_highlight_path(objs, n, state, gap, opts)` is one anim that sets each of
  `objs` (nodes and edges in path order, e.g. node, edge, node, ...) to `state`,
  `gap` seconds apart. Played, it returns a handle spanning all of them.

## 5. Axes and plots

```c
typedef struct an_axes_opts {
    AN_OBJ_FIELDS          /* pos: the plot box's top-left; stroke: ink */
    an_vec2 size;          /* the plot box; default {800, 500} */
    float x_min, x_max, y_min, y_max;
    float x_step, y_step;  /* tick spacing in data units; 0: about 6 ticks */
    const char *x_label, *y_label;
    float label_size;      /* tick label em size; default 26 */
} an_axes_opts;
an_obj *an_axes(an_scene *s, const an_axes_opts *opts);
an_vec2 an_axes_point(const an_obj *axes, float x, float y); /* data -> the axes' space */

typedef float (*an_fn)(float x, void *user);
an_obj *an_plot_function(an_obj *axes, an_fn f, void *user, const an_plot_opts *opts);
an_obj *an_plot_data(an_obj *axes, const float *xs, const float *ys, int n, const an_plot_opts *opts);
```

**Axes** (`AN_KIND_AXES`, a group):
- The x axis is along the bottom of the box and the y axis along the left: two
  strokes with the default brush, a little overshoot and taper, so they look
  hand-inked.
- Ticks: short strokes every step from the range's first multiple.
- Tick labels: mono, right of nothing, centered under or beside the ticks.
- Axis labels: at the ends.
- Data maps to axes space linearly: x_min..x_max → 0..w, y_min..y_max → h..0.

**`an_plot_function`:**
- **Adaptive sampling at script time:** start with 32 equal intervals, then
  split an interval when the midpoint's distance to the chord exceeds 0.25 px
  (in axes space), up to depth 8. Non-finite values break the curve into
  separate subpaths.
- The result is a fixed path (rule 10), stroked like any stroke.
  `an_draw(plot)` draws it on by arc length.

**Clipping** works on geometry at creation; neither layers nor the stencil is
used:
- The sampled polyline is clipped to the plot box with Liang–Barsky per segment.
  Points outside are cut at the boundary, and the curve splits into subpaths
  where it leaves and re-enters.
- The box is inset by half the stroke width plus the style's boil amplitude, so
  the drawn ink stays inside the box even while it boils.
- **Why not the stencil:** all 8 bits are taken (0x80 silhouette, 0x70 marks,
  0x0F winding). A clip bit would mean shrinking the winding counter to 3
  bits (exact only to |w| ≤ 7) or a second depth-stencil pass.
- **Why not a layer:** a layer is a rectangle on screen, so it could clip only
  axis-aligned boxes, and it would cost an isolated layer per plot.
- Geometry clipping is exact, free at render time, rotates and scales with the
  axes, and composes with everything.
- **The limit:** what's clipped is fixed at creation. Animating the axes' range
  would need a re-plot (a new object), not a moving window. That's stated.

**`an_plot_data`:** `.style` BARS (rects from y = 0, or from y_min if 0 is out of
range) or POINTS (circles of radius 6), children of the axes at their data
positions. Points outside the range are skipped (with a message). Bars are
clipped to the box.

## Tests (hand-computed)

- **Edges:**
  - Circles r 30 at (100, 100) and (300, 100), gap 6: the line runs x 136..264.
  - After the second node moves to (300, 300) (a played move, sampled at a
    frame in the middle and at the end), it runs along the new direction,
    clipped by hand.
  - Rounded rect nodes clip at their box.
  - The arrowhead's tip is at the end point.
  - An edge to an edge, or a node parented under its own edge, is rejected.
  - test_memo gains a scene with moving nodes and edges.
- **Tree layout:** a 7-key BST inserted as 5 3 8 1 4 7 9 (in-order ranks
  0..6), spacing (80, 100). Positions by hand: 5 at (0, 0), 3 at (−160, 100),
  8 at (160, 100), 1 at (−240, 200), and so on. An n-ary tree checked by hand
  too.
- **Layered:** a 5-node DAG; layers and positions by hand after the sweeps.
- **Nodes:** the rounded rect's corner arc points; the label centered; a
  halftone node has `paper_under` on, and a dot gap over an edge shows paper.
- **States:** node fill and edge stroke exact at the end; a path highlight's
  per-object start times are `gap` apart.
- **Axes:**
  - `an_axes_point` by hand;
  - tick count and positions for 0..10 step 2;
  - every plot point (x, f(x)) mapped exactly at its sample x;
  - for a function leaving the range, every path point inside the inset box,
    and a split into two subpaths where it re-enters;
  - draw-on at progress 0.5 covers half the arc length;
  - bars' heights by hand.

## `examples/11_bst`

- **Insertion:** 12 keys from the scene's RNG are inserted into a BST, one at a
  time:
  - the comparison path from the root lights up node by node (ACTIVE), with
    the edges between;
  - the new node pops in and its edge draws on;
  - the whole tree glides to its new tidy layout;
  - the states reset.
- **Plot:** tree height vs n for this run (data points), with log2(n + 1)
  (the best case) drawn on as a function.
- **Command:** `make example-bst` exports unattended. The script is written as
  a user would write it, and every friction point is listed.

## Commits

1. edges (endpoint resolution, cycle check, arrowheads, cache key);
2. nodes and states (including path highlights);
3. layouts;
4. axes and plots (clipping);
5. the example and the friction list.
