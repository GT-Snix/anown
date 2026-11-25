// The plane (docs/plane.md): the identity gate, seek order with the cache,
// mapping by hand, constant ink width, the singular matrix reached from the
// identity, clipping, anchoring and crushed vectors.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "plane_internal.h"
#include "render_internal.h"
#include "scene_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080 };

static ani_offscreen g_off;
static ani_renderer g_r;
static unsigned char *g_px;
static const size_t BYTES = (size_t)W * H * 4;

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

static const an_mat2 A = { 3, 1, 0, 2 };

// A plane with every kind of content: grid, axes, vectors (one crushed later),
// a line, an anchored dot and label. Full look (shadow on).
static an_scene *busy(float chaos, an_obj **plane) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 12,
                                                    .style = { .shadow = true, .grain_off = chaos == 0.0f } });
    an_set_chaos(s, chaos);
    an_obj *p = an_plane(s, &(an_plane_opts){ .axes = true });
    an_plane_vector(p, (an_vec2){ 1, 1 }, &(an_arrow_opts){ .stroke = AN_HEX(0x16161A) });
    an_plane_vector(p, (an_vec2){ 1, -1 }, &(an_arrow_opts){ .stroke = AN_HEX(0x00B4D8) });
    an_plane_line(p, (an_vec2){ 0, 0 }, (an_vec2){ 2, 1 }, &(an_line_opts){ .stroke_width = 2 });
    an_plane_dot(p, (an_vec2){ 2, 1 }, NULL);
    an_text(s, &(an_text_opts){ .parent = p, .text = "v", .pos = { 1, 1.3f }, .size = 40, .font = an_font_mono(s) });
    *plane = p;
    return s;
}

// The identity gate: at M = I, every byte equals the same scene with mapping
// bypassed (p -> (unit x, -unit y)), with every effect on. And with chaos 0 (no
// boil, no grain), I -> A -> I ends on the same bytes it started with.
static void test_identity(void) {
    an_obj *p;
    an_scene *s = busy(1.0f, &p);
    an_wait(s, 0.5f);
    unsigned char *ref = malloc(BYTES);
    int same = 0;
    for (int64_t f = 0; f <= 15; f += 5) {
        render(s, f);
        memcpy(ref, g_px, BYTES);
        ani_plane_map_off = true;
        render(s, f);
        ani_plane_map_off = false;
        same += memcmp(ref, g_px, BYTES) == 0;
    }
    CHECK(same == 4);
    an_scene_destroy(s);

    s = busy(0.0f, &p);
    an_play(s, an_plane_matrix_to(p, A, &(an_anim_opts){ .duration = 1 }));
    an_play(s, an_plane_matrix_to(p, AN_MAT2_IDENTITY, &(an_anim_opts){ .duration = 1 }));
    render(s, 0);
    memcpy(ref, g_px, BYTES);
    render(s, an_scene_length(s));
    CHECK(memcmp(ref, g_px, BYTES) == 0);
    render(s, 30); // and A itself is something else
    CHECK(memcmp(ref, g_px, BYTES) != 0);
    free(ref);
    an_scene_destroy(s);
}

// Seek order: I -> A -> B -> a rotation in three keyframes, rendered with the
// cache off (the reference), then on: forward, reverse and shuffled.
static void test_seek_order(void) {
    an_obj *p;
    an_scene *s = busy(1.0f, &p);
    an_play(s, an_plane_matrix_to(p, A, &(an_anim_opts){ .duration = 0.5f }));
    an_play(s, an_plane_matrix_to(p, (an_mat2){ -1, 0, 0, 2 }, &(an_anim_opts){ .duration = 0.5f }));
    for (int k = 1; k <= 3; k++) {
        const float r = (float)k * 0.5235988f;
        an_play(s, an_plane_matrix_to(p, (an_mat2){ cosf(r), -sinf(r), sinf(r), cosf(r) },
                                      &(an_anim_opts){ .duration = 0.2f, .ease = AN_EASE_LINEAR }));
    }
    const int64_t L = an_scene_length(s);
    unsigned char *ref = malloc(BYTES * (size_t)(L + 1));
    g_r.mesh.memo_off = true;
    for (int64_t f = 0; f <= L; f++) {
        render(s, f);
        memcpy(ref + BYTES * (size_t)f, g_px, BYTES);
    }
    g_r.mesh.memo_off = false;
    int bad = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int64_t i = 0; i <= L; i++) {
            const int64_t f = pass == 0 ? i : pass == 1 ? L - i : (i * 17) % (L + 1);
            render(s, f);
            bad += memcmp(ref + BYTES * (size_t)f, g_px, BYTES) != 0;
        }
    CHECK(bad == 0);
    free(ref);
    an_scene_destroy(s);
}

// Mapping by hand (1080p: unit 100, origin (960, 540), y up): v = (1, 1) under A
// is (4, 2) -> (1360, 340); halfway (linear) M = [[2, 0.5], [0, 1.5]], (2.5, 1.5) ->
// (1210, 390). The anchored dot at (2, 1) -> A: (7, 2) -> (1660, 340), its size unchanged.
static void test_mapping(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H });
    an_obj *p = an_plane(s, NULL);
    an_obj *dot = an_plane_dot(p, (an_vec2){ 2, 1 }, &(an_circle_opts){ .radius = 10 });
    an_play_async(s, an_plane_matrix_to(p, A, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_wait(s, 0.5f);
    an_vec2 q = an_plane_point_at_cursor(p, (an_vec2){ 1, 1 });
    CHECK_NEAR(q.x, 1210, 1e-3);
    CHECK_NEAR(q.y, 390, 1e-3);
    an_wait(s, 0.5f);
    q = an_plane_point_at_cursor(p, (an_vec2){ 1, 1 });
    CHECK_NEAR(q.x, 1360, 1e-3);
    CHECK_NEAR(q.y, 340, 1e-3);
    const an_bounds b = an_obj_bounds_at_cursor(dot);
    CHECK_NEAR((b.min.x + b.max.x) * 0.5, 1660, 1e-3);
    CHECK_NEAR((b.min.y + b.max.y) * 0.5, 340, 1e-3);
    CHECK_NEAR(b.max.x - b.min.x, 20, 1e-3);
    an_mat2 m = an_plane_matrix_at_cursor(p);
    CHECK(m.a == 3 && m.b == 1 && m.c == 0 && m.d == 2);
    an_scene_destroy(s);
}

// Ink width on screen: the grid line y = 1 is at y 440 under I and at 340 under A
// (it maps to y = 2); with chaos 0, its column of inked pixels at x 1300 has the
// same count under both (the matrix doesn't scale the ink).
static int inked_rows(int x, int y0, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; y++) n += g_px[((size_t)y * W + (size_t)x) * 4] < 232;
    return n;
}

static void test_ink_width(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .style = { .grain_off = true, .vignette_off = true } });
    an_set_chaos(s, 0.0f);
    an_obj *p = an_plane(s, &(an_plane_opts){ .stroke_width = 4 });
    an_play(s, an_plane_matrix_to(p, A, NULL));
    render(s, 0);
    const int at_i = inked_rows(1310, 430, 450);
    render(s, an_scene_length(s));
    const int at_a = inked_rows(1310, 330, 350);
    if (at_i != at_a || at_i < 3) fprintf(stderr, "  ink rows: %d at I, %d at A\n", at_i, at_a);
    CHECK(at_i == at_a && at_i >= 3);
    an_scene_destroy(s);
}

// The singular matrix C = [[1, 1], [1, 1]] reached from the identity over 1 s: the
// determinant 1 - e^2 reaches 0 only on the last frame. No NaN or infinity in any
// vertex there; the grid's line set on the last frame (the fallback range) holds
// every line of the frame before (no jump); every grid point lands on y = x
// (plane-local (u, -u)). A vector along (1, -1) is crushed to 0: finite, its head
// scaled to 0, its direction its own: (0.7071, 0.7071) on screen.
static void test_singular(void) {
    an_obj *p;
    an_scene *s = busy(1.0f, &p);
    an_play(s, an_plane_matrix_to(p, (an_mat2){ 1, 1, 1, 1 }, &(an_anim_opts){ .duration = 1 }));
    const int64_t L = an_scene_length(s);
    render(s, L);
    int bad = 0;
    for (int i = 0; i < g_r.mesh.count; i++) {
        const ani_vertex *v = &g_r.mesh.v[i];
        bad += !isfinite(v->x) || !isfinite(v->y) || !isfinite(v->r) || !isfinite(v->a);
    }
    CHECK(bad == 0 && g_r.mesh.count > 0);
    const an_obj *grid = NULL;
    for (const an_obj *c = p->first_child; c; c = c->next_sibling) if (c->kind == AN_KIND_PLANE_GRID) grid = c;
    CHECK(grid != NULL);
    const an_vec2 lo = { -1000, -600 }, hi = { 1000, 600 };
    static ani_grid_seg a[ANI_PLANE_MAX_LINES + 2], b[ANI_PLANE_MAX_LINES + 2];
    const ani_map before = ani_plane_map_at(p, ani_frame_time(s, L - 1)), last = ani_plane_map_at(p, ani_frame_time(s, L));
    const int na = ani_plane_grid(grid, &before, lo, hi, a, ANI_PLANE_MAX_LINES + 2);
    const int nb = ani_plane_grid(grid, &last, lo, hi, b, ANI_PLANE_MAX_LINES + 2);
    int missing = 0;
    for (int i = 0; i < na; i++) {
        bool found = false;
        for (int j = 0; j < nb && !found; j++) found = a[i].axis == b[j].axis && a[i].k == b[j].k;
        missing += !found;
    }
    CHECK(na > 0 && missing == 0);
    int off_line = 0;
    for (int j = 0; j < nb; j++) off_line += fabsf(b[j].a.x + b[j].a.y) > 1e-3f || fabsf(b[j].b.x + b[j].b.y) > 1e-3f;
    CHECK(off_line == 0);
    // the crushed vector
    an_obj *vec = NULL;
    for (an_obj *c = p->first_child; c; c = c->next_sibling)
        if (c->kind == AN_KIND_PLANE_VECTOR && c->pos.y < 0) vec = c;
    an_vec2 tip, dir;
    float head;
    ani_plane_vector_geom(vec, &last, ani_frame_time(s, L), &tip, &dir, &head);
    CHECK(tip.x == 0.0f && tip.y == 0.0f && head == 0.0f);
    CHECK_NEAR(dir.x, 0.70710678, 1e-6);
    CHECK_NEAR(dir.y, 0.70710678, 1e-6);
    an_scene_destroy(s);
}

// Clipping: under A, the box (+-965, +-545 px = +-9.65, +-5.45 units) has the preimage
// corners y = Y/2, x = (X - y)/3: x in [-4.125, 4.125], y in [-2.725, 2.725], so lines
// x = -4..4 (9) and y = -2..2 (5): 14. Every segment lies inside the box and ends on
// its edge (no line end is visible).
static void test_clipping(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H });
    an_obj *p = an_plane(s, NULL);
    an_plane_set_matrix(p, A);
    const ani_map m = ani_plane_map_at(p, ani_tick_time(s->cursor));
    const an_vec2 lo = { -965, -545 }, hi = { 965, 545 };
    static ani_grid_seg seg[ANI_PLANE_MAX_LINES + 2];
    const int n = ani_plane_grid(p->first_child, &m, lo, hi, seg, ANI_PLANE_MAX_LINES + 2);
    int inside = 0, ends_on_edge = 0;
    for (int i = 0; i < n; i++) {
        const an_vec2 e[2] = { seg[i].a, seg[i].b };
        for (int k = 0; k < 2; k++) {
            inside += e[k].x >= lo.x - 1e-3f && e[k].x <= hi.x + 1e-3f && e[k].y >= lo.y - 1e-3f && e[k].y <= hi.y + 1e-3f;
            ends_on_edge += fabsf(e[k].x - lo.x) < 1e-2f || fabsf(e[k].x - hi.x) < 1e-2f || fabsf(e[k].y - lo.y) < 1e-2f ||
                            fabsf(e[k].y - hi.y) < 1e-2f;
        }
    }
    CHECK(n == 14 && inside == 2 * n && ends_on_edge == 2 * n);
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_plane: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(BYTES);
    test_identity();
    test_seek_order();
    test_mapping();
    test_ink_width();
    test_singular();
    test_clipping();
    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_plane");
}
