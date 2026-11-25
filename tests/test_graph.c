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

// ---- nodes and states ----------------------------------------------------------------------------

// States: a node set ACTIVE ends exactly at the palette's accent b (#FF2E88); an
// edge set ACTIVE at b with width 5 x 1.5 = 7.5. A path highlight of node, edge,
// node with gap 0.2 s starts the third object's state at 0.4 s: just before, it's
// BASE; at 0.4 s, ACTIVE. A halftone node has paper_under; a solid one doesn't.
static void test_states(void) {
    an_scene *s = scene();
    an_obj *a = an_node(s, &(an_node_opts){ .pos = { 100, 100 } });
    an_obj *b = an_node(s, &(an_node_opts){ .pos = { 300, 100 }, .pattern = { .kind = AN_PATTERN_HALFTONE } });
    an_obj *e = an_edge(s, &(an_edge_opts){ .from = a, .to = b });
    CHECK(b->u.node.shape->paper_under && !a->u.node.shape->paper_under);
    const an_handle h = an_play(s, an_highlight_path((an_obj *[]){ a, e, b }, 3, AN_CELL_ACTIVE, 0.2f, NULL));
    CHECK(h.start == 0.0 && h.end == 0.7); // the last object's 0.3 s track starts at 0.4
    const ani_prop_ref sb = ani_obj_prop(b->u.node.shape, AN_PROP_CELL_STATE);
    CHECK(ani_sample(sb, ani_tick_time(399999)).v[0] == 0.0 && ani_sample(sb, ani_tick_time(400000)).v[0] == 1.0);
    CHECK(an_node_state_at_cursor(b) == AN_CELL_ACTIVE);
    const an_color mag = AN_HEX(0xFF2E88);
    const an_obj_state ns = an_obj_at_cursor(a->u.node.shape), es = an_obj_at_cursor(e);
    CHECK(ns.fill.r == mag.r && ns.fill.g == mag.g && ns.fill.b == mag.b);
    CHECK(es.stroke.r == mag.r && es.stroke.g == mag.g && es.stroke_width == 7.5f);
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
    test_states();

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_graph");
}
