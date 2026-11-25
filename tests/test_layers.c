// Layers (docs/layers.md, phase 5A): isolated layers with stencil coverage and
// premultiplied compositing, checked on real Metal output (4x MSAA, readback).
// Expected colors are worked out by hand from the paper and paint colors.
//
// paper    #F3EDE2 = (243, 237, 226)
// charcoal #16161A = ( 22,  22,  26)
// pink     #FF48B0 = (255,  72, 176)
// blue     #0078BF = (  0, 120, 191)
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 200 };

static ani_offscreen g_off;
static ani_renderer g_r;
static unsigned char *g_px;

static void render(an_scene *s, int64_t frame, bool force_isolate) {
    void *pool = ani_pool_push();
    g_r.mesh.force_isolate = force_isolate;
    CHECK(ani_renderer_prepare(&g_r, s, frame, W, H));
    ani_offscreen_begin(&g_off, AN_BLACK);
    ani_renderer_draw(&g_r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&g_off, g_px);
    ani_pool_pop(pool);
    g_r.mesh.force_isolate = false;
}

static const unsigned char *at(const unsigned char *px, int x, int y) { return px + ((size_t)y * W + (size_t)x) * 4; }

// |pixel - expected| <= tol per channel; expected given as doubles (x.5 allowed)
static bool near_rgb(const unsigned char *p, double r, double g, double b, double tol) {
    return fabs(p[0] - r) <= tol && fabs(p[1] - g) <= tol && fabs(p[2] - b) <= tol;
}

static bool is_paper(const unsigned char *p) { return near_rgb(p, 243, 237, 226, 2); }

static an_scene *new_scene(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60 });
    an_set_chaos(s, 0); // pixel tests run without effects
    return s;
}

// A scribble that overlaps itself many times: reversals and a crossing.
static an_obj *scribble(an_scene *s, an_color ink) {
    return an_stroke(s, &(an_stroke_opts){
        .segs = (an_seg[]){ AN_MOVE(40, 60), AN_LINE(280, 70), AN_LINE(50, 80), AN_LINE(270, 95),
                            AN_LINE(60, 110), AN_CUBIC(300, 200, 0, 0, 280, 140) },
        .seg_count = 6, .stroke = ink, .stroke_width = 14,
        .brush = { .overshoot = 0.6f },
    });
}

// Distance from (px, py) to the stroke's flattened centerline (its path points,
// in scene space since the stroke sits at the origin). Plain geometry, not the
// renderer: it decides which pixels are fully inside the ribbon.
static double centerline_dist(const an_obj *o, double px, double py) {
    const ani_path *p = o->path;
    double best = 1e30;
    for (int i = 0; i + 1 < p->npts; i++) {
        const double ax = p->pts[i].x, ay = p->pts[i].y, bx = p->pts[i + 1].x, by = p->pts[i + 1].y;
        const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        const double ex = ax + dx * t - px, ey = ay + dy * t - py;
        best = fmin(best, sqrt(ex * ex + ey * ey));
    }
    return best;
}

// Every pixel whose center is at least 1.5 px inside the ribbon (half width 7)
// must be the expected color: one blend of ink over paper, however many
// triangles overlap there. Edge pixels are partially covered and excluded.
static int check_uniform(const char *name, const an_obj *o, double r, double g, double b) {
    int interior = 0, bad = 0;
    for (int y = 1; y < H - 1; y++) {
        for (int x = 1; x < W - 1; x++) {
            if (centerline_dist(o, x + 0.5, y + 0.5) > 7.0 - 1.5) continue;
            interior++;
            const unsigned char *p = at(g_px, x, y);
            if (!near_rgb(p, r, g, b, 1.0)) {
                if (bad < 3) fprintf(stderr, "  %s: (%d,%d) = %d %d %d, want %.1f %.1f %.1f\n", name, x, y, p[0], p[1], p[2], r, g, b);
                bad++;
            }
        }
    }
    printf("  %s: %d interior pixels, %d off\n", name, interior, bad);
    CHECK(interior > 1000);
    return bad;
}

static void test_overlap_opacity(void) {
    // opaque charcoal ink, object at opacity 0.5: 0.5 ink + 0.5 paper
    an_scene *s = new_scene();
    an_obj *o = scribble(s, AN_HEX(0x16161A));
    an_obj_set_opacity(o, 0.5f);
    render(s, 0, false);
    CHECK(g_r.mesh.nlayers == 1 && g_r.layers_rendered == 1);
    CHECK(check_uniform("overlap, opacity 0.5", o, (22 + 243) / 2.0, (22 + 237) / 2.0, (26 + 226) / 2.0) == 0);
    an_scene_destroy(s);
}

static void test_overlap_translucent_ink(void) {
    // object at opacity 1, ink color alpha 0.5: isolated for stencil coverage
    an_scene *s = new_scene();
    an_obj *o = scribble(s, (an_color){ 22 / 255.0f, 22 / 255.0f, 26 / 255.0f, 0.5f });
    render(s, 0, false);
    CHECK(g_r.mesh.nlayers == 1);
    CHECK(check_uniform("overlap, ink alpha 0.5", o, (22 + 243) / 2.0, (22 + 237) / 2.0, (26 + 226) / 2.0) == 0);
    an_scene_destroy(s);
}

// Two overlapping opaque rects in a group at 0.5: the group fades as one unit,
// so the top rect looks the same over the bottom one as over paper.
//   A (pink): x 60..140, y 70..130     B (blue, on top): x 100..180, y 80..140
static void test_group_opacity(void) {
    an_scene *s = new_scene();
    an_obj *g = an_group(s, NULL);
    an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 100, 100 }, .size = { 80, 60 }, .fill = AN_HEX(0xFF48B0) });
    an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 140, 110 }, .size = { 80, 60 }, .fill = AN_HEX(0x0078BF) });
    an_obj_set_opacity(g, 0.5f);
    render(s, 0, false);
    const double bR = (0 + 243) / 2.0, bG = (120 + 237) / 2.0, bB = (191 + 226) / 2.0;   // 121.5 178.5 208.5
    const double pR = (255 + 243) / 2.0, pG = (72 + 237) / 2.0, pB = (176 + 226) / 2.0;  // 249 154.5 201
    CHECK(near_rgb(at(g_px, 120, 100), bR, bG, bB, 1)); // overlap: B over A, but faded as a unit
    CHECK(near_rgb(at(g_px, 160, 120), bR, bG, bB, 1)); // B over paper: the same color
    CHECK(near_rgb(at(g_px, 80, 90), pR, pG, pB, 1));   // A only
    CHECK(is_paper(at(g_px, 30, 30)));
    // the layer's rectangle: content x 60..180, y 70..140, plus 2 px
    CHECK(g_r.rects[0].x == 58 && g_r.rects[0].y == 68 && g_r.rects[0].w == 124 && g_r.rects[0].h == 74);
    an_scene_destroy(s);
}

// Two translucent fills (alpha 0.5) overlapping inside one group layer (group
// opacity 0.5). The second must draw over the first: that needs the stencil reset
// under it. By hand, with A = pink, B = blue, P = paper:
//   layer at the overlap: 0.5B + 0.25A, alpha 0.75; composited at 0.5:
//   0.25B + 0.125A + 0.625P = (183.75, 187.125, 211.0)
//   B only: 0.25B + 0.75P = (182.25, 207.75, 217.25);  A only: (245.5, 195.75, 213.5)
static void test_translucent_in_shared_layer(void) {
    an_scene *s = new_scene();
    an_obj *g = an_group(s, NULL);
    an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 100, 100 }, .size = { 80, 60 },
                                .fill = { 1.0f, 72 / 255.0f, 176 / 255.0f, 0.5f } });
    an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 140, 110 }, .size = { 80, 60 },
                                .fill = { 0.0f, 120 / 255.0f, 191 / 255.0f, 0.5f } });
    an_obj_set_opacity(g, 0.5f);
    render(s, 0, false);
    CHECK(g_r.mesh.nlayers == 1);
    CHECK(near_rgb(at(g_px, 120, 100), 183.75, 187.125, 211.0, 1));
    CHECK(near_rgb(at(g_px, 160, 120), 182.25, 207.75, 217.25, 1));
    CHECK(near_rgb(at(g_px, 80, 90), 245.5, 195.75, 213.5, 1));
    an_scene_destroy(s);
}

// Nested: a rect at 0.5 inside a group at 0.5 = 0.25 blue + 0.75 paper.
static void test_nested(void) {
    an_scene *s = new_scene();
    an_obj *g = an_group(s, NULL);
    an_obj *r = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 160, 100 }, .size = { 100, 80 }, .fill = AN_HEX(0x0078BF) });
    an_obj_set_opacity(g, 0.5f);
    an_obj_set_opacity(r, 0.5f);
    render(s, 0, false);
    CHECK(g_r.mesh.nlayers == 2 && g_r.layers_rendered == 2);
    CHECK(near_rgb(at(g_px, 160, 100), 0.25 * 0 + 0.75 * 243, 0.25 * 120 + 0.75 * 237, 0.25 * 191 + 0.75 * 226, 1));
    an_scene_destroy(s);
}

// Forced isolation of everything vs the direct path, at opacity 1. Same image up
// to 8-bit rounding, except "conflation" pixels: where two objects' edges split a
// pixel's samples, resolve-then-composite differs from per-sample blending by up
// to 1/4 of the color range. E.g. A on samples {1,2}, B on {3,4}: direct =
// 0.5A + 0.5B; composited = 0.5B + 0.25A + 0.25bg; difference 0.25(A - bg) <= 64.
// So: 99.9% of pixels within 1, every pixel within 64 + 1 rounding.
static void test_forced_vs_direct(void) {
    an_scene *s = new_scene();
    an_rect(s, &(an_rect_opts){ .pos = { 90, 90 }, .size = { 110, 80 }, .fill = AN_HEX(0xFF48B0),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 6, .brush = { .taper_in = 20, .overshoot = 0.6f } });
    an_circle(s, &(an_circle_opts){ .pos = { 170, 110 }, .radius = 55, .fill = AN_HEX(0x0078BF), .stroke = AN_HEX(0x16161A), .stroke_width = 5 });
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 240, 60 }, .rotation = 0.3f });
    an_rect(s, &(an_rect_opts){ .parent = g, .size = { 60, 60 }, .fill = AN_HEX(0xFFE800) });
    an_arrow(s, &(an_arrow_opts){ .parent = g, .from = { -40, 60 }, .to = { 40, 100 }, .stroke = AN_HEX(0x16161A), .stroke_width = 4 });
    scribble(s, AN_HEX(0x16161A));
    unsigned char *direct = malloc((size_t)W * H * 4);
    render(s, 0, false);
    CHECK(g_r.mesh.nlayers == 0);
    memcpy(direct, g_px, (size_t)W * H * 4);
    render(s, 0, true);
    CHECK(g_r.mesh.nlayers >= 6 && g_r.layers_rendered == g_r.mesh.nlayers);
    int maxd = 0, over1 = 0;
    for (int i = 0; i < W * H; i++) {
        for (int c = 0; c < 3; c++) {
            const int d = abs((int)direct[i * 4 + c] - (int)g_px[i * 4 + c]);
            if (d > maxd) maxd = d;
            if (d > 1) {
                over1++;
                break;
            }
        }
    }
    printf("  forced isolation vs direct: max channel diff %d, %d of %d pixels differ by more than 1 (%.3f%%)\n",
           maxd, over1, W * H, 100.0 * over1 / (W * H));
    CHECK(maxd <= 65);
    CHECK(over1 <= W * H / 1000); // 99.9% within 1
    free(direct);
    an_scene_destroy(s);
}

// Over budget: layers fall back to drawing directly; the frame still renders.
static void test_budget_fallback(void) {
    an_scene *s = new_scene();
    an_obj *o = scribble(s, AN_HEX(0x16161A));
    an_obj_set_opacity(o, 0.5f);
    const size_t saved = g_r.budget;
    const size_t used = g_r.pool_bytes;
    g_r.budget = 0; // nothing fits, even after freeing the unused cached targets
    // a different size class than anything allocated so far forces a new allocation
    an_obj *big = an_rect(s, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 300, 190 }, .fill = AN_HEX(0x0078BF) });
    an_obj_set_opacity(big, 0.5f);
    fprintf(stderr, "  (an over-budget warning is expected below)\n");
    render(s, 0, false);
    CHECK(g_r.layers_fallback >= 1);
    CHECK(!is_paper(at(g_px, 160, 100))); // still drawn
    CHECK(g_r.pool_bytes <= used);
    g_r.budget = saved;
    an_scene_destroy(s);
}

// Many size classes over a long render (the eigen video ran out of sokol's image
// pool): 100 frames, each with a translucent rect of a new size class. The cache
// stays within ANI_MAX_SCRATCHES / ANI_MAX_RESOLVES live targets (unused ones are
// freed), and no layer ever falls back.
static void test_pool_bounded(void) {
    int worst_s = 0, worst_r = 0, fallbacks = 0;
    for (int k = 0; k < 100; k++) {
        an_scene *s = new_scene();
        an_obj *o = an_rect(s, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 20.0f + 2.9f * (float)k, 12.0f + 1.7f * (float)k },
                                                .fill = AN_HEX(0x0078BF) });
        an_obj_set_opacity(o, 0.5f);
        render(s, 0, false);
        fallbacks += g_r.layers_fallback;
        int ls = 0, lr = 0;
        for (int i = 0; i < g_r.nscratch; i++) ls += g_r.scratches[i].w != 0;
        for (int i = 0; i < g_r.nresolve; i++) lr += g_r.resolves[i].w != 0;
        worst_s = ls > worst_s ? ls : worst_s;
        worst_r = lr > worst_r ? lr : worst_r;
        an_scene_destroy(s);
    }
    printf("  100 size classes: at most %d scratch and %d resolve targets live\n", worst_s, worst_r);
    CHECK(fallbacks == 0 && worst_s <= ANI_MAX_SCRATCHES && worst_r <= ANI_MAX_RESOLVES);
}

// The pool is reused: rendering the same frame again allocates nothing.
static void test_pool_stable(void) {
    an_scene *s = new_scene();
    for (int i = 0; i < 20; i++) {
        an_obj *o = an_circle(s, &(an_circle_opts){ .pos = { 20.0f + 14 * i, 100 }, .radius = 18, .fill = AN_HEX(0xFF48B0) });
        an_obj_set_opacity(o, 0.5f);
    }
    render(s, 0, false);
    const size_t bytes = g_r.pool_bytes;
    const int nres = g_r.nresolve;
    for (int i = 0; i < 4; i++) render(s, 0, false);
    CHECK(g_r.pool_bytes == bytes && g_r.nresolve == nres);
    CHECK(g_r.layers_rendered == 20);
    printf("  20 isolated circles: pool %zu KB, %d resolve textures, %d scratch classes\n", bytes >> 10, nres, g_r.nscratch);
    an_scene_destroy(s);
}

// Three-level nested fade with overlapping inner objects:
//   G1 (0.5) > G2 (0.5) > A pink (0.5) and B blue (0.5), B on top, overlapping.
// By hand (premultiplied, P = paper):
//   G2 layer at the overlap: A in at 0.5 -> (0.5A, 0.5); B over at 0.5 ->
//     (0.5B + 0.25A, 0.75)
//   G1 layer: x0.5 -> (0.25B + 0.125A, 0.375)
//   main: x0.5 -> 0.125B + 0.0625A + (1 - 0.1875)P
//     = (213.375, 212.0625, 218.5)
//   B only: 0.125B + 0.875P = (212.625, 222.375, 221.625)
//   A only: 0.125A + 0.875P = (244.5, 216.375, 219.75)
//   A: x 60..140, y 70..130    B: x 100..180, y 80..140
static void test_three_level_nested(void) {
    an_scene *s = new_scene();
    an_obj *g1 = an_group(s, NULL);
    an_obj *g2 = an_group(s, &(an_group_opts){ .parent = g1 });
    an_obj *a = an_rect(s, &(an_rect_opts){ .parent = g2, .pos = { 100, 100 }, .size = { 80, 60 }, .fill = AN_HEX(0xFF48B0) });
    an_obj *b = an_rect(s, &(an_rect_opts){ .parent = g2, .pos = { 140, 110 }, .size = { 80, 60 }, .fill = AN_HEX(0x0078BF) });
    an_obj_set_opacity(g1, 0.5f);
    an_obj_set_opacity(g2, 0.5f);
    an_obj_set_opacity(a, 0.5f);
    an_obj_set_opacity(b, 0.5f);
    render(s, 0, false);
    CHECK(g_r.mesh.nlayers == 4 && g_r.layers_rendered == 4);
    CHECK(near_rgb(at(g_px, 120, 100), 213.375, 212.0625, 218.5, 1));
    CHECK(near_rgb(at(g_px, 160, 120), 212.625, 222.375, 221.625, 1));
    CHECK(near_rgb(at(g_px, 80, 90), 244.5, 216.375, 219.75, 1));
    an_scene_destroy(s);
}

// Fade sweep of two touching shapes sharing a diagonal edge, from opacity 1.0
// down to 0.98 in 0.005 steps, written as PNGs (full frame plus an 8x zoom of
// the shared edge) in out/test/ for checking by eye.
//
// Two variants: each shape fading on its own (two layers), and both in one
// group that fades (one layer). Pink A = (255,72,176), blue B = (0,120,191),
// paper G = 237. Paper bleeding into the seam shows as green above the A/B mix.
//   - opacity 1 (direct path): a seam pixel is a mix of A and B only, so
//     G <= max(72, 120) = 120 (+1 rounding)
//   - separate layers (conflation): the worst case is B over (A over paper)
//     with half coverage each: G <= 0.25*237 + 0.75*120 = 149.25 (+1)
//   - one group layer: per-sample inside the layer, then x0.98 over paper:
//     G <= 0.98*120 + 0.02*237 = 122.3 (+1)
#include "stb_image_write.h"
#include <sys/stat.h>

static int seam_max_g(an_vec2 c, float rot) {
    int maxg = 0;
    const float cs = cosf(-rot), sn = sinf(-rot);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float dx = (float)x + 0.5f - c.x, dy = (float)y + 0.5f - c.y;
            const float lx = dx * cs - dy * sn, ly = dx * sn + dy * cs; // into the group's frame
            if (fabsf(lx) < 0.75f && fabsf(ly) < 45.0f) {
                const int g = at(g_px, x, y)[1];
                if (g > maxg) maxg = g;
            }
        }
    }
    return maxg;
}

static void save_png(const char *path) {
    stbi_write_png(path, W, H, 4, g_px, W * 4);
}

static void save_zoom(const char *path, int cx, int cy) {
    enum { R = 20, Z = 8 }; // 40x40 crop, 8x nearest-neighbor
    static unsigned char out[(2 * R * Z) * (2 * R * Z) * 4];
    for (int y = 0; y < 2 * R * Z; y++) {
        for (int x = 0; x < 2 * R * Z; x++) {
            const unsigned char *p = at(g_px, cx - R + x / Z, cy - R + y / Z);
            memcpy(&out[((size_t)y * 2 * R * Z + (size_t)x) * 4], p, 4);
        }
    }
    stbi_write_png(path, 2 * R * Z, 2 * R * Z, 4, out, 2 * R * Z * 4);
}

static void test_fade_sweep(void) {
    mkdir("out", 0755);
    mkdir("out/test", 0755);
    const an_vec2 c = { 160, 100 };
    const float rot = 0.3f;
    int sep_max = 0, grp_max = 0, direct_max = 0;
    for (int variant = 0; variant < 2; variant++) {
        an_scene *s = new_scene();
        an_obj *g = an_group(s, &(an_group_opts){ .pos = c, .rotation = rot });
        an_obj *a = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { -40, 0 }, .size = { 80, 100 }, .fill = AN_HEX(0xFF48B0) });
        an_obj *b = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 40, 0 }, .size = { 80, 100 }, .fill = AN_HEX(0x0078BF) });
        // frame k (0..4) has opacity 1 - 0.005 k
        const an_anim_opts lin = { .duration = 4.0f / 60.0f, .ease = AN_EASE_LINEAR };
        if (variant == 0) {
            an_par(s, 2, (an_anim[]){ an_fade_to(a, 0.98f, &lin), an_fade_to(b, 0.98f, &lin) });
        } else {
            an_play(s, an_fade_to(g, 0.98f, &lin));
        }
        for (int k = 0; k <= 4; k++) {
            render(s, k, false);
            const int mg = seam_max_g(c, rot);
            char path[128];
            snprintf(path, sizeof path, "out/test/fade_sweep_%s_%.3f.png", variant ? "group" : "separate", 1.0 - 0.005 * k);
            save_png(path);
            snprintf(path, sizeof path, "out/test/fade_sweep_%s_%.3f_zoom.png", variant ? "group" : "separate", 1.0 - 0.005 * k);
            save_zoom(path, (int)c.x, (int)c.y);
            printf("  fade sweep, %s, opacity %.3f: %d layers, seam max G %d\n", variant ? "group   " : "separate",
                   1.0 - 0.005 * k, g_r.mesh.nlayers, mg);
            if (k == 0) {
                direct_max = mg > direct_max ? mg : direct_max;
                CHECK(g_r.mesh.nlayers == 0);
            } else if (variant == 0) {
                sep_max = mg > sep_max ? mg : sep_max;
            } else {
                grp_max = mg > grp_max ? mg : grp_max;
            }
        }
        an_scene_destroy(s);
    }
    CHECK(direct_max <= 121);  // no paper in the seam at opacity 1
    CHECK(sep_max <= 150);     // conflation bound for separate layers
    CHECK(grp_max <= 124);     // one group layer: no seam beyond the 2% fade
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_layers: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    test_overlap_opacity();
    test_overlap_translucent_ink();
    test_group_opacity();
    test_nested();
    test_translucent_in_shared_layer();
    test_three_level_nested();
    test_fade_sweep();
    test_forced_vs_direct();
    test_pool_stable();
    test_budget_fallback();
    test_pool_bounded();

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_layers");
}
