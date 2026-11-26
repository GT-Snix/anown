// The space (docs/space.md), milestone A: projection by hand, the plane gate,
// near-plane cutting (also under camera shake), orbit sweeps, seek order with
// the cache, move3 and the matrix helpers.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "plane_internal.h"
#include "render_internal.h"
#include "scene_internal.h"
#include "space_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080 };
static const float PI = 3.14159265358979f;

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

static int nonfinite_vertices(void) {
    int bad = 0;
    for (int i = 0; i < g_r.mesh.count; i++) {
        const ani_vertex *v = &g_r.mesh.v[i];
        bad += !isfinite(v->x) || !isfinite(v->y) || !isfinite(v->r) || !isfinite(v->a);
    }
    return bad;
}

static an_scene *scene(void) { return an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 12 }); }

// Projection (1080p: unit 100, the target at (960, 540), D = 10):
// 1. default camera, (1, 0, 1): depth 9, s = 10/9 -> (960 + 111.11, 540)
// 2. tilt pi/2, yaw 0 (front view, z up), (1, 0, 1): depth 10 -> (1060, 440)
// 3. tilt pi/2, yaw pi/2 (camera at (10, 0, 0)): (0, 1, 0) -> (1060, 540);
//    (1, 0, 0) -> (960, 540) at depth 9
// 4. M = diag(2, 1, 1) at the default camera: (1, 0, 0) -> (1160, 540)
// 5. a dolly 10 -> 40 is log-space: halfway (linear ease) the distance is 20
static void test_projection(void) {
    an_scene *s = scene();
    an_obj *sp = an_space(s, NULL);
    an_vec2 q = an_space_point_at_cursor(sp, (an_vec3){ 1, 0, 1 });
    CHECK_NEAR(q.x, 960 + 1000.0 / 9.0, 1e-3);
    CHECK_NEAR(q.y, 540, 1e-3);
    CHECK_NEAR(an_space_depth_at_cursor(sp, (an_vec3){ 1, 0, 1 }), 9, 1e-6);
    an_space_set_orbit(sp, (an_orbit){ 0, PI / 2, 10 });
    q = an_space_point_at_cursor(sp, (an_vec3){ 1, 0, 1 });
    CHECK_NEAR(q.x, 1060, 1e-3);
    CHECK_NEAR(q.y, 440, 1e-3);
    an_space_set_orbit(sp, (an_orbit){ PI / 2, PI / 2, 10 });
    q = an_space_point_at_cursor(sp, (an_vec3){ 0, 1, 0 });
    CHECK_NEAR(q.x, 1060, 1e-3);
    CHECK_NEAR(q.y, 540, 1e-3);
    q = an_space_point_at_cursor(sp, (an_vec3){ 1, 0, 0 });
    CHECK_NEAR(q.x, 960, 1e-3);
    CHECK_NEAR(q.y, 540, 1e-3);
    CHECK_NEAR(an_space_depth_at_cursor(sp, (an_vec3){ 1, 0, 0 }), 9, 1e-5);
    an_space_set_orbit(sp, (an_orbit){ 0, 0, 10 });
    an_space_set_matrix(sp, an_mat3_scale((an_vec3){ 2, 1, 1 }));
    q = an_space_point_at_cursor(sp, (an_vec3){ 1, 0, 0 });
    CHECK_NEAR(q.x, 1160, 1e-3);
    CHECK_NEAR(q.y, 540, 1e-3);
    an_play_async(s, an_space_orbit_to(sp, (an_orbit){ 0, 0, 40 }, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_wait(s, 0.5f);
    CHECK_NEAR(an_space_orbit_at_cursor(sp).distance, 20, 1e-3);
    an_scene_destroy(s);
}

// The plane gate: at the default camera with M = I, a space holding vectors, a line,
// a dot and a label at z = 0 renders the same bytes as an_plane with the same
// content (no grid), full look. The grids agree segment by segment within 1e-3 px.
static void gate_content(an_scene *s, an_obj *p, bool space) {
    const an_arrow_opts va = { .stroke = AN_HEX(0x16161A) }, vb = { .stroke = AN_HEX(0x00B4D8) };
    const an_line_opts lo = { .stroke_width = 2 };
    if (space) {
        an_space_vector(p, (an_vec3){ 1, 1, 0 }, &va);
        an_space_vector(p, (an_vec3){ 1, -1, 0 }, &vb);
        an_space_line(p, (an_vec3){ 0, 0, 0 }, (an_vec3){ 2, 1, 0 }, &lo);
        an_space_dot(p, (an_vec3){ 2, 1, 0 }, NULL);
    } else {
        an_plane_vector(p, (an_vec2){ 1, 1 }, &va);
        an_plane_vector(p, (an_vec2){ 1, -1 }, &vb);
        an_plane_line(p, (an_vec2){ 0, 0 }, (an_vec2){ 2, 1 }, &lo);
        an_plane_dot(p, (an_vec2){ 2, 1 }, NULL);
    }
    an_text(s, &(an_text_opts){ .parent = p, .text = "v", .pos = { 1, 1.3f }, .size = 40, .font = an_font_mono(s) });
    an_wait(s, 0.5f);
}

static an_scene *gate_scene(bool space, an_obj **out) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 12, .style = { .shadow = true } });
    an_obj *p = space ? an_space(s, &(an_space_opts){ .grid = AN_SPACE_GRID_NONE })
                      : an_plane(s, &(an_plane_opts){ .grid_step = -1 });
    gate_content(s, p, space);
    *out = p;
    return s;
}

static void test_plane_gate(void) {
    an_obj *pl, *sp;
    an_scene *a = gate_scene(false, &pl), *b = gate_scene(true, &sp);
    unsigned char *ref = malloc(BYTES);
    int same = 0;
    for (int64_t f = 0; f <= 15; f += 5) {
        render(a, f);
        memcpy(ref, g_px, BYTES);
        render(b, f);
        same += memcmp(ref, g_px, BYTES) == 0;
    }
    CHECK(same == 4);
    free(ref);
    an_scene_destroy(a);
    an_scene_destroy(b);

    // the grids
    an_scene *s = scene();
    an_obj *p = an_plane(s, NULL), *q = an_space(s, NULL);
    const ani_time at = ani_tick_time(s->cursor);
    const ani_map m = ani_plane_map_at(p, at);
    const ani_view v = ani_space_view_at(q, at);
    const an_vec2 lo = { -965, -545 }, hi = { 965, 545 };
    static ani_grid_seg ga[ANI_PLANE_MAX_LINES + 2], gb[ANI_PLANE_MAX_LINES + 2];
    const int na = ani_plane_grid(p->first_child, &m, lo, hi, ga, ANI_PLANE_MAX_LINES + 2);
    const int nb = ani_space_grid(q->first_child, &v, lo, hi, gb, ANI_PLANE_MAX_LINES + 2);
    int matched = 0;
    double worst = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++) {
            if (ga[i].axis != gb[j].axis || ga[i].k != gb[j].k) continue;
            matched++;
            // the plane's lines run in +x / -y, the space's from -e to +e: same direction
            const double d = fmax(fmax(fabs(ga[i].a.x - gb[j].a.x), fabs(ga[i].a.y - gb[j].a.y)),
                                  fmax(fabs(ga[i].b.x - gb[j].b.x), fabs(ga[i].b.y - gb[j].b.y)));
            const double d2 = fmax(fmax(fabs(ga[i].a.x - gb[j].b.x), fabs(ga[i].a.y - gb[j].b.y)),
                                   fmax(fabs(ga[i].b.x - gb[j].a.x), fabs(ga[i].b.y - gb[j].a.y)));
            worst = fmax(worst, fmin(d, d2));
        }
    if (worst > 1e-3) fprintf(stderr, "  grid: worst endpoint difference %g px\n", worst);
    CHECK(na == 30 && nb == na && matched == na && worst <= 1e-3); // x = -9..9 (19), y = -5..5 (11)
    an_scene_destroy(s);
}

// Near plane (default camera, D = 10, near 0.5):
// - the line through (0, 0, 9.8) along (1, 0, 1): its points (t, 0, 9.8 + t) have depth
//   0.2 - t, visible for t <= -0.3, at x = 1000 t / (0.2 - t) px: from -600 (the cut)
//   toward its vanishing point at -1000, clipped at the box's left edge (-990)
// - a line through the camera itself, (0, 0, 10) along (0, 0, 1): a dot at the center
// - the vector (1, 0, 20): its tip is behind the camera; cut where depth = 0.5, at
//   t = 0.475 -> 1000 x 0.475 / 0.5 = 950 px; no head
static void test_near_plane(void) {
    an_scene *s = scene();
    an_obj *sp = an_space(s, &(an_space_opts){ .axes = true });
    an_obj *l1 = an_space_line(sp, (an_vec3){ 0, 0, 9.8f }, (an_vec3){ 1, 0, 1 }, NULL);
    an_obj *l2 = an_space_line(sp, (an_vec3){ 0, 0, 10 }, (an_vec3){ 0, 0, 1 }, NULL);
    an_obj *vec = an_space_vector(sp, (an_vec3){ 1, 0, 20 }, NULL);
    an_space_vector(sp, (an_vec3){ 0, 0, 30 }, NULL); // straight at the eye, through it
    an_space_path(sp, (an_vec3[]){ { -2, 0, 0 }, { 0, 0, 15 }, { 2, 0, 0 }, { 0, 2, 12 } }, 4, true, NULL);
    an_play(s, an_shake(s, 30, &(an_anim_opts){ .duration = 1 }));
    const ani_view v = ani_space_view_at(sp, ani_tick_time(0));
    const an_vec2 lo = { -990, -600 }, hi = { 990, 600 };
    an_vec2 a, b;
    CHECK(ani_space_line_seg(l1, &v, lo, hi, &a, &b));
    CHECK_NEAR(fmax(a.x, b.x), -600, 1e-3);
    CHECK_NEAR(fmin(a.x, b.x), -990, 1e-3);
    CHECK(a.y == 0.0f && b.y == 0.0f);
    CHECK(ani_space_line_seg(l2, &v, lo, hi, &a, &b) && fabsf(a.x) < 1e-3f && fabsf(a.y) < 1e-3f);
    an_vec2 tail, tip, dir;
    float head;
    bool has_tip;
    CHECK(ani_space_vector_geom(vec, &v, ani_tick_time(0), &tail, &tip, &dir, &head, &has_tip));
    CHECK(!has_tip && head == 0.0f && tail.x == 0.0f && tail.y == 0.0f);
    CHECK_NEAR(tip.x, 950, 1e-3);
    // every frame of the shake: finite vertices, and the line's clipped segment inside the
    // shaken frame's box
    const int64_t L = an_scene_length(s);
    int bad = 0, outside = 0, empty = 0;
    for (int64_t f = 0; f <= L; f++) {
        render(s, f);
        bad += nonfinite_vertices();
        empty += g_r.mesh.count == 0;
        const an_vec2 sh = ani_shake_at(s, ani_frame_time(s, f));
        const an_vec2 slo = { -960 + sh.x, -540 + sh.y }, shi = { 960 + sh.x, 540 + sh.y };
        if (ani_space_line_seg(l1, &v, slo, shi, &a, &b))
            outside += !(a.x >= slo.x - 1e-3f && a.x <= shi.x + 1e-3f && b.x >= slo.x - 1e-3f && b.x <= shi.x + 1e-3f &&
                         isfinite(a.x) && isfinite(b.x));
    }
    CHECK(bad == 0 && outside == 0 && empty == 0);
    an_scene_destroy(s);
}

// Orbit sweeps: tilt 0 -> pi/2 at yaw 0.3, then yaw 0 -> 4 pi at tilt 0 (straight
// overhead, where yaw is the view's roll). No NaN or infinity at any frame.
static an_scene *busy(an_obj **out) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 12, .style = { .shadow = true } });
    an_obj *sp = an_space(s, &(an_space_opts){ .axes = true });
    an_space_vector(sp, (an_vec3){ 2, 1, 0 }, &(an_arrow_opts){ .stroke = AN_HEX(0xE6007E) });
    an_space_vector(sp, (an_vec3){ -1, 2, 1 }, &(an_arrow_opts){ .stroke = AN_HEX(0x00B4D8) });
    an_obj *up = an_space_vector(sp, (an_vec3){ 0, 0, 2.5f }, &(an_arrow_opts){ .stroke = AN_HEX(0xFF6A00) });
    an_space_path(sp, (an_vec3[]){ { 1, 1, 0 }, { 1, 1, 1 }, { -1, 1, 1 }, { -1, 1, 0 } }, 4, true, NULL);
    an_space_dot(sp, (an_vec3){ 2, 1, 0 }, NULL);
    an_text(s, &(an_text_opts){ .parent = sp, .text = "z", .pos = { 0, 0.3f }, .size = 40, .font = an_font_mono(s) });
    *out = up;
    return s;
}

static void test_orbit_sweeps(void) {
    an_obj *up;
    an_scene *s = busy(&up);
    an_obj *sp = up->parent;
    an_space_set_orbit(sp, (an_orbit){ 0.3f, 0, 10 });
    an_play(s, an_space_orbit_to(sp, (an_orbit){ 0.3f, PI / 2, 10 }, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_play(s, an_space_orbit_to(sp, (an_orbit){ 0, 0, 10 }, &(an_anim_opts){ .duration = 0.5f }));
    an_play(s, an_space_orbit_to(sp, (an_orbit){ 4 * PI, 0, 10 }, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    const int64_t L = an_scene_length(s);
    int bad = 0, empty = 0;
    for (int64_t f = 0; f <= L; f++) {
        render(s, f);
        bad += nonfinite_vertices();
        empty += g_r.mesh.count == 0;
    }
    CHECK(bad == 0 && empty == 0);
    an_scene_destroy(s);
}

// Seek order: orbit, dolly, a matrix change, a target move and a move3, rendered with
// the cache off (the reference), then on: forward, reverse and shuffled.
static void test_seek_order(void) {
    an_obj *up;
    an_scene *s = busy(&up);
    an_obj *sp = up->parent;
    an_play(s, an_space_orbit_to(sp, (an_orbit){ 0.6f, 1.0f, 14 }, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_space_matrix_to(sp, (an_mat3){ { 3, 1, 0, 0, 2, 0, 0, 0, 1 } }, &(an_anim_opts){ .duration = 0.3f }));
    an_play_async(s, an_space_target_to(sp, (an_vec3){ 1, 0, 0.5f }, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_move3_to(up, (an_vec3){ 1, -1, 2 }, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_space_orbit_to(sp, (an_orbit){ -0.8f, 0.4f, 8 }, &(an_anim_opts){ .duration = 0.2f }));
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
    if (bad) fprintf(stderr, "  seek order: %d of %lld frames differ\n", bad, (long long)(3 * (L + 1)));
    CHECK(bad == 0);
    free(ref);
    an_scene_destroy(s);
}

// move3: pos and pos_z with one timing; halfway (linear) at the midpoint. The matrix
// helpers: a rotation keeps its axis exactly; angles reduce modulo 2 pi.
static void test_move3_and_helpers(void) {
    an_scene *s = scene();
    an_obj *sp = an_space(s, NULL);
    an_obj *v = an_space_vector(sp, (an_vec3){ 1, 0, 0 }, NULL);
    const an_handle h = an_play_async(s, an_move3_to(v, (an_vec3){ 3, 2, 4 }, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    CHECK_NEAR(h.end - h.start, 1.0, 1e-9);
    an_wait(s, 0.5f);
    double p[3];
    ani_space_pos3(v, ani_tick_time(s->cursor), p);
    CHECK_NEAR(p[0], 2, 1e-6);
    CHECK_NEAR(p[1], 1, 1e-6);
    CHECK_NEAR(p[2], 2, 1e-6);
    const an_mat3 r = an_mat3_rotation((an_vec3){ 0, 0, 1 }, 0.7f);
    CHECK(r.m[2] == 0.0f && r.m[5] == 0.0f && r.m[8] == 1.0f && r.m[6] == 0.0f && r.m[7] == 0.0f);
    const an_mat3 r1 = an_mat3_rotation((an_vec3){ 1, 1, 1 }, 0.3f), r2 = an_mat3_rotation((an_vec3){ 1, 1, 1 }, 0.3f + 40 * PI);
    double worst = 0;
    for (int k = 0; k < 9; k++) worst = fmax(worst, fabs(r1.m[k] - r2.m[k]));
    CHECK(worst < 2e-5); // 0.3 + 40 pi in float is 0.3 to about 1e-5
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_space: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(BYTES);
    test_projection();
    test_plane_gate();
    test_near_plane();
    test_orbit_sweeps();
    test_seek_order();
    test_move3_and_helpers();
    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_space");
}
