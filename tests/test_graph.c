// Graphs (docs/graphs.md): edges resolved from their nodes each frame, cycle
// rejection, the cache with moving nodes, nodes, states and path highlights,
// layouts, axes and plots. Every expected value is worked out by hand in the
// comments.
//
// paper #F3EDE2 = (243, 237, 226); charcoal #16161A = (22, 22, 26)
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1280, H = 720 };

static ani_offscreen g_off;
static ani_renderer g_r;
static unsigned char *g_px;

static void render(an_scene *s, int64_t frame) {
    void *pool = ani_pool_push();
    CHECK(ani_renderer_prepare(&g_r, s, frame, W, H));
    ani_offscreen_begin(&g_off, AN_BLACK);
    ani_renderer_draw(&g_r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&g_off, g_px);
    ani_pool_pop(pool);
}

static const unsigned char *at(int x, int y) { return g_px + ((size_t)y * W + (size_t)x) * 4; }
static bool is(int x, int y, int r, int g, int b) {
    const unsigned char *p = at(x, y);
    const bool ok = p[0] == r && p[1] == g && p[2] == b;
    if (!ok) fprintf(stderr, "  (%d,%d) = %d %d %d, want %d %d %d\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}
#define PAPER(x, y) is(x, y, 243, 237, 226)
#define CHARCOAL(x, y) is(x, y, 22, 22, 26)

static an_scene *scene(void) {
    return an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 8,
                                             .style = { .grain_off = true, .vignette_off = true, .boil_off = true } });
}

static void resolve(const an_obj *e, an_scene *s, int64_t f, an_vec2 *p0, an_vec2 *p1) {
    an_vec2 c, d;
    CHECK(ani_edge_resolve(e, ani_frame_time(s, f), p0, p1, &c, &d));
}

// ---- edges ----------------------------------------------------------------------------------

// Circles r 30 at (100, 100) and (300, 100), gap 6: x 100 + 36 = 136 to 300 - 36 = 264.
// The second node then moves to (300, 300) over 1 s, linear (frames 0..60):
//   frame 30: it's at (300, 200); d = (200, 100), unit (0.894427, 0.447214):
//     (100, 100) + 36 u = (132.199, 116.100); (300, 200) - 36 u = (267.801, 183.900)
//   frame 60: d = (200, 200), unit (0.707107, 0.707107): 36 u = (25.456, 25.456):
//     (125.456, 125.456) and (274.544, 274.544)
// A rounded node (120 x 70) at (500, 100), approached from the left: its box edge
// at x 440, gap 6: 434.
static void test_edge_resolve(void) {
    an_scene *s = scene();
    an_obj *a = an_node(s, &(an_node_opts){ .pos = { 100, 100 }, .radius = 30 });
    an_obj *b = an_node(s, &(an_node_opts){ .pos = { 300, 100 }, .radius = 30 });
    an_obj *r = an_node(s, &(an_node_opts){ .shape = AN_NODE_ROUNDED, .pos = { 500, 100 } });
    an_obj *e = an_edge(s, &(an_edge_opts){ .from = a, .to = b });
    an_obj *e2 = an_edge(s, &(an_edge_opts){ .from = a, .to = r, .directed = true });
    an_play(s, an_move_to(b, (an_vec2){ 300, 300 }, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_vec2 p0, p1;
    resolve(e, s, 0, &p0, &p1);
    CHECK_NEAR(p0.x, 136, 1e-4);
    CHECK_NEAR(p1.x, 264, 1e-4);
    CHECK(p0.y == 100.0f && p1.y == 100.0f);
    resolve(e, s, 30, &p0, &p1);
    CHECK_NEAR(p0.x, 132.199, 1e-3);
    CHECK_NEAR(p0.y, 116.100, 1e-3);
    CHECK_NEAR(p1.x, 267.801, 1e-3);
    CHECK_NEAR(p1.y, 183.900, 1e-3);
    resolve(e, s, 60, &p0, &p1);
    CHECK_NEAR(p0.x, 125.456, 1e-3);
    CHECK_NEAR(p1.y, 274.544, 1e-3);
    resolve(e2, s, 0, &p0, &p1);
    CHECK_NEAR(p1.x, 434, 1e-4);
    CHECK(e2->u.edge.head && an_obj_kind(e2->u.edge.head) == AN_KIND_POLYGON);
    // no cycles: an endpoint that's an edge, or inside one; moving a node under its edge
    CHECK(an_edge(s, &(an_edge_opts){ .from = e, .to = a }) == NULL);
    an_obj *inside = an_rect(s, &(an_rect_opts){ .parent = e });
    CHECK(an_edge(s, &(an_edge_opts){ .from = inside, .to = a }) == NULL);
    CHECK(!an_obj_set_parent(a, e) && !an_obj_set_parent(b, e));
    CHECK(an_edge(s, &(an_edge_opts){ .from = a, .to = a }) == NULL);
    an_scene_destroy(s);
}

// In pixels, chaos 0: nodes as above but gap 20, edge width 6 (a band y 97..103).
// The node outline (r 30, width 5) inks x 127.5..132.5; the edge starts at
// 100 + 30 + 20 = 150 with a round cap of radius 3 (leftmost x 147):
// (200, 100) charcoal, (200, 104) paper, (140, 100) paper (inside the gap).
static void test_edge_pixels(void) {
    an_scene *s = scene();
    an_set_chaos(s, 0.0f);
    an_obj *a = an_node(s, &(an_node_opts){ .pos = { 100, 100 }, .radius = 30 });
    an_obj *b = an_node(s, &(an_node_opts){ .pos = { 300, 100 }, .radius = 30 });
    an_edge(s, &(an_edge_opts){ .from = a, .to = b, .stroke_width = 6, .gap = 20, .stroke = AN_HEX(0x16161A) });
    render(s, 0);
    CHECK(CHARCOAL(200, 100) && CHARCOAL(200, 97) && CHARCOAL(200, 102) && PAPER(200, 104) && PAPER(140, 100));
    an_scene_destroy(s);
}

// The cache with moving nodes: their edges' geometry changes every frame, and the
// resolved endpoints are in the key. Cache on and off give the same meshes and
// pixels, forward and reverse.
static void test_edge_cache(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 8, .style = { .shadow = true } });
    an_obj *n[4];
    for (int k = 0; k < 4; k++)
        n[k] = an_node(s, &(an_node_opts){ .pos = { 200.0f + 250.0f * (float)k, 300 }, .label = "x" });
    for (int k = 0; k < 3; k++) an_edge(s, &(an_edge_opts){ .from = n[k], .to = n[k + 1], .directed = true, .bend = 0.2f, .z = -1 });
    an_play_async(s, an_move_by(n[1], (an_vec2){ 0, 200 }, &(an_anim_opts){ .duration = 0.5f }));
    an_wait(s, 0.3f);
    an_play(s, an_move_by(n[3], (an_vec2){ -100, -150 }, &(an_anim_opts){ .duration = 0.5f }));
    an_wait(s, 0.3f);
    const int64_t L = an_scene_length(s);
    const size_t bytes = (size_t)W * H * 4;
    unsigned char *ref = malloc(bytes * (size_t)(L + 1));
    g_r.mesh.memo_off = true;
    for (int64_t f = 0; f <= L; f++) {
        render(s, f);
        memcpy(ref + bytes * (size_t)f, g_px, bytes);
    }
    g_r.mesh.memo_off = false;
    int bad = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int64_t i = 0; i <= L; i++) {
            const int64_t f = pass ? L - i : i;
            render(s, f);
            bad += memcmp(ref + bytes * (size_t)f, g_px, bytes) != 0;
        }
    CHECK(bad == 0);
    free(ref);
    an_scene_destroy(s);
}

// Edges follow verbs: a horizontal edge a (100, 100) -> b (300, 100), circles r 30,
// gap 6. Mid-pop (and mid-slap) on b, its verb scale is (sx, sy), read from the track;
// the edge runs along x, so its end is at 300 - (30 sx + 6) by hand, and the start
// stays at 136. At the verb's end (sx = 1) the end is back at 264.
static void verb_case(an_anim verb, const char *what) {
    an_scene *s = scene();
    an_obj *g = an_graph(s, NULL);
    an_obj *a = an_graph_node(g, &(an_node_opts){ .pos = { 100, 100 }, .radius = 30 });
    an_obj *b = an_graph_node(g, &(an_node_opts){ .pos = { 300, 100 }, .radius = 30 });
    an_obj *e = an_graph_edge(g, a, b, NULL);
    verb.obj = b;
    const an_handle h = an_play(s, verb);
    int tested = 0;
    for (int64_t f = 1; f < ani_ticks((float)h.end) / (1000000 / 60); f++) {
        const ani_value vs = ani_sample(ani_obj_prop(b, AN_PROP_VERB_SCALE), ani_frame_time(s, f));
        if (vs.v[0] == 1.0) continue;
        an_vec2 p0, p1;
        resolve(e, s, f, &p0, &p1);
        CHECK_NEAR(p1.x, 300.0 - (30.0 * vs.v[0] + 6.0), 1e-3);
        CHECK_NEAR(p1.y, 100.0, 1e-3);
        CHECK_NEAR(p0.x, 136.0, 1e-4);
        tested++;
    }
    if (tested < 5) fprintf(stderr, "  %s: only %d frames with a verb scale\n", what, tested);
    CHECK(tested >= 5);
    an_vec2 p0, p1;
    resolve(e, s, an_scene_length(s), &p0, &p1);
    CHECK_NEAR(p1.x, 264.0, 1e-4);
    an_scene_destroy(s);
}

static void test_edge_verbs(void) {
    verb_case(an_pop(NULL, NULL), "pop");
    verb_case(an_slap(NULL, NULL), "slap");
}

// The camera is applied after edges are resolved: a pan of 100 px (camera x 640 ->
// 540, pure translation, effects off) moves the edge and nodes together; the panned
// frame at x equals the unpanned frame at x - 100, pixel for pixel, over the region
// holding both nodes and the edge.
static void test_edge_camera_pan(void) {
    an_scene *s = scene();
    an_obj *g = an_graph(s, NULL);
    an_obj *a = an_graph_node(g, &(an_node_opts){ .pos = { 300, 300 }, .radius = 30, .label = "a" });
    an_obj *b = an_graph_node(g, &(an_node_opts){ .pos = { 600, 400 }, .radius = 30, .label = "b" });
    an_graph_edge(g, a, b, &(an_edge_opts){ .directed = true, .bend = 0.3f });
    an_wait(s, 0.1f);
    an_play(s, an_camera_move_to(s, (an_vec2){ 540, 360 }, &(an_anim_opts){ .duration = 0.2f }));
    const size_t bytes = (size_t)W * H * 4;
    unsigned char *before = malloc(bytes);
    render(s, 0);
    memcpy(before, g_px, bytes);
    render(s, an_scene_length(s));
    int differ = 0, inked = 0;
    for (int y = 200; y < 500; y++)
        for (int x = 350; x < 800; x++) {
            const unsigned char *p = g_px + ((size_t)y * W + (size_t)x) * 4, *q = before + ((size_t)y * W + (size_t)(x - 100)) * 4;
            differ += memcmp(p, q, 3) != 0;
            inked += q[0] < 100;
        }
    CHECK(differ == 0 && inked > 500);
    free(before);
    an_scene_destroy(s);
}

// ---- nodes and states ----------------------------------------------------------------------------

// States in a graph whose ACTIVE color is #FFD400: a path a -> b -> c (edges a-b and
// b-c, gap 0.2 s, duration 0.3 s) styles a, edge ab, b, edge bc, c, starting at 0, 0.2,
// 0.4, 0.6, 0.8 s; the handle ends at 0.8 + 0.3 = 1.1 s. At the end the nodes' fill and
// the edges' stroke are exactly #FFD400, and the edges are 5 x 1.5 = 7.5 wide.
// A halftone node has paper_under; a solid one doesn't.
static void test_states(void) {
    an_scene *s = scene();
    const an_color yel = AN_HEX(0xFFD400);
    an_obj *g = an_graph(s, &(an_graph_opts){ .state_colors = { [AN_STATE_ACTIVE] = yel } });
    an_obj *a = an_graph_node(g, &(an_node_opts){ .pos = { 100, 100 } });
    an_obj *b = an_graph_node(g, &(an_node_opts){ .pos = { 300, 100 }, .pattern = { .kind = AN_PATTERN_HALFTONE } });
    an_obj *c = an_graph_node(g, &(an_node_opts){ .pos = { 500, 100 } });
    an_obj *ab = an_graph_edge(g, a, b, NULL), *bc = an_graph_edge(g, b, c, NULL);
    CHECK(b->u.node.shape->paper_under && !a->u.node.shape->paper_under);
    const an_handle h = an_play(s, an_highlight_path((an_obj *[]){ a, b, c }, 3, AN_STATE_ACTIVE,
                                                     &(an_path_opts){ .gap = 0.2f }));
    CHECK(h.start == 0.0 && h.end == 1.1);
    an_obj *const order[5] = { a->u.node.shape, ab, b->u.node.shape, bc, c->u.node.shape };
    for (int k = 0; k < 5; k++) { // each step flips its state at k x 0.2 s exactly
        const ani_prop_ref r = ani_obj_prop(order[k], AN_PROP_STATE);
        const int64_t t = (int64_t)k * 200000;
        CHECK(ani_sample(r, ani_tick_time(t - 1)).v[0] == 0.0 && ani_sample(r, ani_tick_time(t)).v[0] == 1.0);
    }
    CHECK(an_node_state_at_cursor(c) == AN_STATE_ACTIVE && an_edge_state_at_cursor(bc) == AN_STATE_ACTIVE);
    const an_obj_state ns = an_obj_at_cursor(b->u.node.shape), es = an_obj_at_cursor(bc);
    CHECK(ns.fill.r == yel.r && ns.fill.g == yel.g && ns.fill.b == yel.b);
    CHECK(es.stroke.r == yel.r && es.stroke.g == yel.g && es.stroke.b == yel.b && es.stroke_width == 7.5f);
    // a reset all at once: every object flips at the cursor (1.1 s)
    an_play(s, an_highlight_path((an_obj *[]){ a, b, c }, 3, AN_STATE_BASE, &(an_path_opts){ .together = true }));
    for (int k = 0; k < 5; k++) {
        const ani_prop_ref r = ani_obj_prop(order[k], AN_PROP_STATE);
        CHECK(ani_sample(r, ani_tick_time(1099999)).v[0] == 1.0 && ani_sample(r, ani_tick_time(1100000)).v[0] == 0.0);
    }
    an_scene_destroy(s);
}

// Ownership: the edge list in creation order; edge_between both ways and NULL;
// a duplicate ordered pair returns the first edge; nodes of another graph are
// rejected; edges default to z -1 and nodes to z 0 (edges under nodes whatever the
// creation order); a path with a missing edge styles only the 2 nodes.
static void test_graph_ownership(void) {
    an_scene *s = scene();
    an_obj *g = an_graph(s, NULL), *h = an_graph(s, NULL);
    an_obj *a = an_graph_node(g, NULL), *b = an_graph_node(g, NULL), *c = an_graph_node(g, NULL);
    an_obj *x = an_graph_node(h, NULL);
    an_obj *ab = an_graph_edge(g, a, b, NULL), *cb = an_graph_edge(g, c, b, NULL);
    CHECK(g->u.graph.ne == 2 && g->u.graph.edges[0].edge == ab && g->u.graph.edges[1].edge == cb);
    CHECK(an_graph_edge_between(g, a, b) == ab && an_graph_edge_between(g, b, a) == ab);
    CHECK(an_graph_edge_between(g, b, c) == cb && an_graph_edge_between(g, a, c) == NULL);
    CHECK(an_graph_edge(g, a, b, NULL) == ab && g->u.graph.ne == 2);
    CHECK(an_graph_edge(g, a, x, NULL) == NULL);
    CHECK(ab->z == -1 && a->z == 0 && ab->parent == g && a->parent == g);
    // a, c have no edge: the path styles a and c only
    const an_anim p = an_highlight_path((an_obj *[]){ a, c }, 2, AN_STATE_ACTIVE, NULL);
    CHECK(p.swap_i == 2 && p.objs[0] == a && p.objs[1] == c);
    an_scene_destroy(s);
}

// ---- layouts -------------------------------------------------------------------------------------

#define POS(v, X, Y) CHECK((v).x == (float)(X) && (v).y == (float)(Y))

// The BST of 5 3 8 1 4 7 9 (index = insertion order). In-order: 1 3 4 5 7 8 9, ranks
// 0..6; the root (5) has rank 3. Spacing (80, 100): x = (rank - 3) x 80:
//   5 (0, 0); 3 (-160, 100); 8 (160, 100); 1 (-240, 200); 4 (-80, 200); 7 (80, 200); 9 (240, 200)
// A lone right child: 5 then 7: 7 at (80, 100).
// n-ary: 0 has children 1, 2, 3; 1 has 4, 5. Leaves in pre-order: 4 slot 0, 5 slot 1,
// 2 slot 2, 3 slot 3; 1 = (0 + 1) / 2 = 0.5; 0 = (0.5 + 3) / 2 = 1.75. Spacing 100:
//   0 (0, 0); 1 (-125, 100); 2 (25, 100); 3 (125, 100); 4 (-175, 200); 5 (-75, 200)
static void test_layout_tree(void) {
    const an_layout_node bst[7] = { { -1, 0 }, { 0, -1 }, { 0, 1 }, { 1, -1 }, { 1, 1 }, { 2, -1 }, { 2, 1 } };
    an_vec2 p[7];
    CHECK(an_layout_tree(bst, 7, &(an_layout_opts){ .spacing = { 80, 100 } }, p));
    POS(p[0], 0, 0); POS(p[1], -160, 100); POS(p[2], 160, 100); POS(p[3], -240, 200);
    POS(p[4], -80, 200); POS(p[5], 80, 200); POS(p[6], 240, 200);
    CHECK(an_layout_tree((an_layout_node[]){ { -1, 0 }, { 0, 1 } }, 2, &(an_layout_opts){ .spacing = { 80, 100 } }, p));
    POS(p[0], 0, 0); POS(p[1], 80, 100);
    const an_layout_node nary[6] = { { -1, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, { 1, 0 } };
    CHECK(an_layout_tree(nary, 6, NULL, p));
    POS(p[0], 0, 0); POS(p[1], -125, 100); POS(p[2], 25, 100); POS(p[3], 125, 100);
    POS(p[4], -175, 200); POS(p[5], -75, 200);
    // bad input: two roots; two left children; a cycle not reaching the root
    CHECK(!an_layout_tree((an_layout_node[]){ { -1, 0 }, { -1, 0 } }, 2, NULL, p));
    CHECK(!an_layout_tree((an_layout_node[]){ { -1, 0 }, { 0, -1 }, { 0, -1 } }, 3, NULL, p));
    CHECK(!an_layout_tree((an_layout_node[]){ { -1, 0 }, { 2, 1 }, { 1, 1 } }, 3, NULL, p));
}

// Layered: edges 0->3, 1->2, 2->4, 3->4, and 4->0 (closes a cycle: ignored).
// Layers: 0, 1 in 0; 2, 3 in 1; 4 in 2. Start in index order: [0 1], [2 3], [4].
// Sweep 1 (down): layer 1: bary(2) = pos(1) = 1, bary(3) = pos(0) = 0 -> [3 2];
//   layer 2: bary(4) = (pos 3 + pos 2) / 2 = 0.5 -> [4].
// Sweep 2 (up): layer 1: bary(3) = bary(2) = pos(4) = 0: tie, previous order -> [3 2];
//   layer 0: bary(0) = pos(3) = 0, bary(1) = pos(2) = 1 -> [0 1]. Nothing moves after.
// Spacing (100, 100), centered: 0 (-50, 0), 1 (50, 0), 3 (-50, 100), 2 (50, 100), 4 (0, 200).
static void test_layout_layered(void) {
    const int e[5][2] = { { 0, 3 }, { 1, 2 }, { 2, 4 }, { 3, 4 }, { 4, 0 } };
    an_vec2 p[5];
    CHECK(an_layout_layered(5, e, 5, NULL, p));
    POS(p[0], -50, 0); POS(p[1], 50, 0); POS(p[3], -50, 100); POS(p[2], 50, 100); POS(p[4], 0, 200);
}

// Origins: the same BST and DAG with origin (960, 300): every position above shifted
// by exactly (960, 300).
static void test_layout_origin(void) {
    const an_layout_node bst[7] = { { -1, 0 }, { 0, -1 }, { 0, 1 }, { 1, -1 }, { 1, 1 }, { 2, -1 }, { 2, 1 } };
    an_vec2 p[7];
    CHECK(an_layout_tree(bst, 7, &(an_layout_opts){ .spacing = { 80, 100 }, .origin = { 960, 300 } }, p));
    POS(p[0], 960, 300); POS(p[1], 800, 400); POS(p[2], 1120, 400); POS(p[3], 720, 500);
    POS(p[4], 880, 500); POS(p[5], 1040, 500); POS(p[6], 1200, 500);
    const int e[5][2] = { { 0, 3 }, { 1, 2 }, { 2, 4 }, { 3, 4 }, { 4, 0 } };
    CHECK(an_layout_layered(5, e, 5, &(an_layout_opts){ .origin = { 960, 300 } }, p));
    POS(p[0], 910, 300); POS(p[1], 1010, 300); POS(p[3], 910, 400); POS(p[2], 1010, 400); POS(p[4], 960, 500);
}

// an_layout_animate: two rects from (0, 0) and (100, 0) to (200, 100) and (300, 300),
// 1 s linear (60 fps): at 0.5 s they're at the midpoints (100, 50) and (200, 150).
// A retarget at 0.5 s of the first to (0, 0) over 1 s continues from (100, 50): at 1 s
// it's at (50, 25). With NULL opts the move is an instant set at the cursor.
static void test_layout_animate(void) {
    an_scene *s = scene();
    an_obj *r[2] = { an_rect(s, NULL), an_rect(s, &(an_rect_opts){ .pos = { 100, 0 } }) };
    const an_anim_opts lin = { .duration = 1, .ease = AN_EASE_LINEAR };
    an_play_async(s, an_layout_animate(r, (an_vec2[]){ { 200, 100 }, { 300, 300 } }, 2, &lin));
    an_wait(s, 0.5f);
    an_vec2 p = an_obj_at_cursor(r[0]).pos;
    POS(p, 100, 50);
    p = an_obj_at_cursor(r[1]).pos;
    POS(p, 200, 150);
    an_play_async(s, an_layout_animate(r, (an_vec2[]){ { 0, 0 } }, 1, &lin));
    an_wait(s, 0.5f);
    p = an_obj_at_cursor(r[0]).pos;
    POS(p, 50, 25);
    an_play(s, an_layout_animate(r, (an_vec2[]){ { 7, 9 } }, 1, NULL));
    p = an_obj_at_cursor(r[0]).pos;
    POS(p, 7, 9);
    an_scene_destroy(s);
}

// Trees in a graph: the BST 5 3 8 1 4 7 9 built with an_graph_child under layout
// spacing (80, 100), origin (960, 300); each node is created at its slot in the tree
// as it stands then (so 3, made when the tree is 5 and 3, sits at in-order rank 0 vs
// the root's 1: (880, 400)); after an_graph_layout_tree (instant) every node is at the
// pure layout's position plus the origin, as in test_layout_origin. Each child has
// its edge from the parent. A second root, or a second left child, is refused.
static void test_graph_tree(void) {
    an_scene *s = scene();
    an_obj *g = an_graph(s, &(an_graph_opts){ .layout = { .spacing = { 80, 100 }, .origin = { 960, 300 } } });
    an_obj *n5 = an_graph_child(g, NULL, 0, NULL);
    an_obj *n3 = an_graph_child(g, n5, -1, NULL);
    an_vec2 p = an_obj_at_cursor(n3).pos;
    POS(p, 880, 400);
    an_obj *n8 = an_graph_child(g, n5, 1, NULL);
    an_obj *n1 = an_graph_child(g, n3, -1, NULL), *n4 = an_graph_child(g, n3, 1, NULL);
    an_obj *n7 = an_graph_child(g, n8, -1, NULL), *n9 = an_graph_child(g, n8, 1, NULL);
    CHECK(an_graph_child(g, NULL, 0, NULL) == NULL && an_graph_child(g, n3, -1, NULL) == NULL);
    CHECK(an_graph_edge_between(g, n5, n3) && an_graph_edge_between(g, n8, n9) && !an_graph_edge_between(g, n3, n8));
    an_play(s, an_graph_layout_tree(g, NULL, NULL));
    an_obj *const all[7] = { n5, n3, n8, n1, n4, n7, n9 };
    const an_vec2 want[7] = { { 960, 300 }, { 800, 400 }, { 1120, 400 }, { 720, 500 }, { 880, 500 }, { 1040, 500 }, { 1200, 500 } };
    for (int i = 0; i < 7; i++) {
        p = an_obj_at_cursor(all[i]).pos;
        POS(p, want[i].x, want[i].y);
    }
    an_scene_destroy(s);
}

// ---- axes and plots -------------------------------------------------------------------------------

static float half(float x, void *u) { (void)u; return x * 0.5f; }
static float swing(float x, void *u) { (void)u; return 2.5f + 2.4f * sinf(2.0f * x); }
static float wave(float x, void *u) { (void)u; return 4.0f + 3.0f * sinf(x); }
static float hole(float x, void *u) { (void)u; return (x > 4.0f && x < 6.0f) ? NAN : 2.0f; }

// Axes 800 x 500, x 0..10 step 2, y 0..5 step 1: a point maps to (x * 80, 500 - y * 100).
//   (5, 2.5) -> (400, 250); (0, 0) -> (0, 500); (10, 5) -> (800, 0).
// Ticks: x at 0, 160, ..., 800 (6), y at 500, 400, ..., 0 (6): with the two axis lines,
// 14 strokes and 12 labels. Default steps: range 10 -> 10/6 = 1.67 -> 2; range 1 -> 0.2.
// The plot box inset for the curve: half of 5 + the boil 1.5 = 4: [4, 796] x [4, 496].
static void test_axes(void) {
    an_scene *s = scene();
    an_obj *ax = an_axes(s, &(an_axes_opts){ .pos = { 200, 100 }, .size = { 800, 500 }, .x_max = 10, .y_max = 5,
                                             .x_step = 2, .y_step = 1 });
    an_vec2 p = an_axes_point(ax, 5, 2.5f);
    POS(p, 400, 250);
    p = an_axes_point(ax, 0, 0);
    POS(p, 0, 500);
    p = an_axes_point(ax, 10, 5);
    POS(p, 800, 0);
    int strokes = 0, texts = 0, xt = 0, yt = 0;
    for (an_obj *c = ax->first_child; c; c = c->next_sibling) {
        if (c->kind == AN_KIND_TEXT) texts++;
        if (c->kind != AN_KIND_STROKE) continue;
        strokes++;
        const an_vec2 a = c->path->pts[0], b = c->path->pts[c->path->npts - 1];
        if (a.y == 500.0f && b.y == 512.0f && a.x == b.x) CHECK(a.x == 160.0f * (float)xt++);
        if (a.x == 0.0f && b.x == -12.0f && a.y == b.y) CHECK(a.y == 500.0f - 100.0f * (float)yt++);
    }
    CHECK(strokes == 14 && texts == 12 && xt == 6 && yt == 6);
    an_obj *d = an_axes(s, &(an_axes_opts){ .y_max = 1 }); // x 0..10 default, y 0..1: steps 2 and 0.2
    int dx = 0, dy = 0;
    for (an_obj *c = d->first_child; c; c = c->next_sibling) {
        if (c->kind != AN_KIND_STROKE) continue;
        const an_vec2 a = c->path->pts[0], b = c->path->pts[c->path->npts - 1];
        dx += a.y == 500.0f && b.y == 512.0f;
        dy += a.x == 0.0f && b.x == -12.0f;
    }
    CHECK(dx == 6 && dy == 6);

    // f(x) = x / 2: a straight line from (0, 500) to (800, 0), slope -0.625, so no
    // splits: 33 samples. Clipped to the inset box: it enters where y = 496 (x = 6.4,
    // since x = 4 still has y 497.5) and leaves where y = 4 (x = 793.6). One subpath.
    an_obj *lin = an_plot_function(ax, half, NULL, NULL);
    CHECK(lin && lin->path->nsubs == 1 && lin->path->npts == 33);
    CHECK_NEAR(lin->path->pts[0].x, 6.4, 1e-3);
    CHECK_NEAR(lin->path->pts[0].y, 496, 1e-3);
    CHECK_NEAR(lin->path->pts[32].x, 793.6, 1e-3);
    CHECK_NEAR(lin->path->pts[32].y, 4, 1e-3);
    // f(x) = 2.5 + 2.4 sin 2x (0.1..4.9, inside the box): curved enough to split (240 px
    // amplitude, 400 px period: about 11 px off the chord over a 25 px interval), so more
    // than 33 samples; every point is (x * 80, 500 - f(x) * 100) for its own x
    an_obj *par = an_plot_function(ax, swing, NULL, NULL);
    CHECK(par && par->path->npts > 33);
    int exact = 0;
    for (int i = 0; i < par->path->npts; i++) {
        const an_vec2 q = par->path->pts[i];
        if (q.x <= 4.0f || q.x >= 796.0f || q.y <= 4.0f || q.y >= 496.0f) continue;
        const float x = q.x / 80.0f;
        exact += fabsf(q.y - (500.0f - swing(x, NULL) * 100.0f)) < 1e-3f;
        CHECK_NEAR(q.y, 500.0f - swing(x, NULL) * 100.0f, 2e-3);
    }
    CHECK(exact > 20);
    // f(x) = 4 + 3 sin x leaves the top (f > 5 when sin x > 1/3: x in (0.34, 2.80) and
    // (6.62, 9.08)) and comes back: 3 subpaths, all inside the inset box
    an_obj *wv = an_plot_function(ax, wave, NULL, NULL);
    CHECK(wv && wv->path->nsubs == 3);
    for (int i = 0; wv && i < wv->path->npts; i++) {
        const an_vec2 q = wv->path->pts[i];
        CHECK(q.x >= 4.0f - 1e-3f && q.x <= 796.0f + 1e-3f && q.y >= 4.0f - 1e-3f && q.y <= 496.0f + 1e-3f);
    }
    // non-finite for 4 < x < 6: the curve breaks into 2 subpaths
    an_obj *ho = an_plot_function(ax, hole, NULL, NULL);
    CHECK(ho && ho->path->nsubs == 2);

    // bars at x 2, 4, 6 with values 3, 7, -1 on y 0..5: the first is 300 px tall,
    // centered at y 500 - 150 = 350 and x 160; the second is clipped to 5 (500 px,
    // center 250); the third to 0 (no height). Points: 7 and -1 are outside 0..5, so
    // one point, (2, 3) -> (160, 200).
    const float xs[3] = { 2, 4, 6 }, ys[3] = { 3, 7, -1 };
    an_obj *bars = an_plot_data(ax, xs, ys, 3, &(an_plot_opts){ .style = AN_PLOT_BARS });
    an_obj *b0 = an_plot_item(bars, 0), *b1 = an_plot_item(bars, 1), *b2 = an_plot_item(bars, 2);
    CHECK(b0 && b1 && b2 && !an_plot_item(bars, 3));
    CHECK(b0->u.rect.size.y == 300.0f && b0->pos.x == 160.0f && b0->pos.y == 350.0f);
    CHECK(b1->u.rect.size.y == 500.0f && b1->pos.y == 250.0f);
    CHECK(b2->u.rect.size.y < 0.01f && b2->pos.y == 500.0f);
    an_obj *pts = an_plot_data(ax, xs, ys, 3, NULL);
    CHECK(an_plot_item(pts, 0) && !an_plot_item(pts, 1));
    p = an_plot_item(pts, 0)->pos;
    POS(p, 160, 200);
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_graph: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    test_edge_resolve();
    test_edge_pixels();
    test_edge_cache();
    test_edge_verbs();
    test_edge_camera_pan();
    test_states();
    test_graph_ownership();
    test_layout_tree();
    test_layout_layered();
    test_layout_origin();
    test_layout_animate();
    test_graph_tree();
    test_axes();

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_graph");
}
