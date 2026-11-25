// Halftone and hatch fills (docs/layers.md, 5C): real Metal output, every
// expected pixel worked out by hand from the documented pattern definition.
// Scenes run at chaos 0 (no riso, boil, grain) unless a test says otherwise.
//
// paper #F3EDE2 = (243, 237, 226); blue #0078BF = (0, 120, 191)
//
// Base rect: center (160, 100), 200 x 120 -> local x -100..100, y -60..60,
// screen x 60..260, y 40..160. Halftone cell 20, density 0.5:
//   dot centers where local = 20k + 10: screen x = 70, 90, ..., 250; y = 50, 70, ..., 150
//   radius = sqrt(0.5 / pi) cells = 0.398942 x 20 = 7.979 px
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 200 };

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
#define BLUE(x, y) is(x, y, 0, 120, 191)

static an_scene *calm(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H });
    an_set_chaos(s, 0);
    return s;
}

// Mean ink coverage over a pixel rectangle: (paper - R) / (paper - blue), red channel.
static double coverage(int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) sum += (243.0 - at(x, y)[0]) / 243.0;
    return sum / ((double)(x1 - x0) * (y1 - y0));
}

static void test_halftone(void) {
    an_scene *s = calm();
    an_rect(s, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 200, 120 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
    render(s, 0);
    // dot center (70, 50): pixel center (70.5, 50.5) is 0.71 px away -> ink
    CHECK(BLUE(70, 50));
    // pixel (76, 50): center 6.52 px from (70, 50) < 7.98 - 0.5 -> ink
    CHECK(BLUE(76, 50));
    // pixel (79, 50): 9.01 px away > 7.98 + 0.5 -> paper
    CHECK(PAPER(79, 50));
    // cell corner (80, 60): ~14 px from every center -> paper
    CHECK(PAPER(80, 60));
    CHECK(BLUE(250, 150) && PAPER(260, 160));
    // coverage over the 10 x 6 whole cells: pi r^2 = density = 0.5
    const double c = coverage(60, 40, 260, 160);
    printf("  halftone density 0.5: measured coverage %.4f\n", c);
    CHECK(fabs(c - 0.5) < 0.01);
    an_scene_destroy(s);

    an_scene *t = calm();
    an_rect(t, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 200, 120 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.2f } });
    render(t, 0);
    const double c2 = coverage(60, 40, 260, 160);
    printf("  halftone density 0.2: measured coverage %.4f\n", c2);
    CHECK(fabs(c2 - 0.2) < 0.01);
    an_scene_destroy(t);
}

// Angle 45 degrees. Pattern coordinates p = R(-45) * local / 20; dot centers
// where p = (k + 0.5, j + 0.5). For (k, j) = (0, 0): local = 20 * R(45) * (0.5, 0.5)
// = (0, 14.142) -> screen (160, 114.14). For (-1, -1): (160, 85.86).
//   pixel (160, 114): local (0.5, 14.5) -> p = (0.5304, 0.4950), 0.031 cells from center -> ink
//   pixel (160, 85):  local (0.5, -14.5) -> p = (-0.4950, -0.5304) -> ink
//   pixel (160, 100): local (0.5, 0.5) -> p = (0.0354, 0) -> 0.683 cells from a center -> paper
static void test_halftone_angle(void) {
    an_scene *s = calm();
    an_rect(s, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 200, 120 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f, .angle = 0.78539816f } });
    render(s, 0);
    CHECK(BLUE(160, 114) && BLUE(160, 85) && PAPER(160, 100));
    an_scene_destroy(s);
}

// Anchored in object space: moving the object moves the pattern with it (same
// pixels, shifted), and scaling it scales the dots.
static void test_anchoring(void) {
    an_scene *a = calm();
    an_rect(a, &(an_rect_opts){ .pos = { 120, 100 }, .size = { 160, 120 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.4f, .angle = 0.3f } });
    render(a, 0);
    unsigned char *ref = malloc((size_t)W * H * 4);
    memcpy(ref, g_px, (size_t)W * H * 4);
    an_scene *b = calm();
    an_rect(b, &(an_rect_opts){ .pos = { 157, 100 }, .size = { 160, 120 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.4f, .angle = 0.3f } });
    render(b, 0);
    // The pattern must move with the object: every pixel that is solid ink or
    // solid paper in one render is exactly the same in the other. Anti-aliased
    // dot rims may differ slightly: the GPU interpolates pattern coordinates in
    // float relative to screen position (~0.015 px here), so rims at a different
    // screen position round a few levels differently. "Solid" is decided by hand
    // geometry (more than 1 px inside or outside a dot), not by the render.
    // Bound on rims: 12 levels.
    int solid_bad = 0, rim = 0, maxd = 0;
    for (int y = 42; y < 158; y++) {
        for (int x = 42; x < 198; x++) { // interior of the first rect (x 40..200)
            const unsigned char *p = ref + ((size_t)y * W + (size_t)x) * 4, *q = at(x + 37, y);
            // hand geometry: pattern coords of this pixel center in the first rect
            // (center 120, 100; angle 0.3; cell 20), distance to the nearest dot center
            const double lx = x + 0.5 - 120.0, ly = y + 0.5 - 100.0;
            const double pu = (cos(0.3) * lx + sin(0.3) * ly) / 20.0, pv = (-sin(0.3) * lx + cos(0.3) * ly) / 20.0;
            const double fu = pu - floor(pu) - 0.5, fv = pv - floor(pv) - 0.5;
            const double dist_px = sqrt(fu * fu + fv * fv) * 20.0, r_px = sqrt(0.4 / 3.14159265) * 20.0;
            const bool deep_ink = dist_px < r_px - 1.0, deep_paper = dist_px > r_px + 1.0;
            if (deep_ink && !(p[0] == 0 && q[0] == 0 && p[2] == 191 && q[2] == 191)) solid_bad++;
            if (deep_paper && !(p[0] == 243 && q[0] == 243 && p[1] == 237 && q[1] == 237)) solid_bad++;
            const bool p_solid = false;
            int d = 0;
            for (int k = 0; k < 3; k++) d = abs(p[k] - q[k]) > d ? abs(p[k] - q[k]) : d;
            if (d > maxd) maxd = d;
            (void)p_solid;
            if (d > 1) rim++;
        }
    }
    printf("  moved by +37 px: solid pixels that changed %d; rim pixels differing by > 1: %d; max difference %d levels\n",
           solid_bad, rim, maxd);
    CHECK(solid_bad == 0 && maxd <= 12);
    free(ref);
    an_scene_destroy(a);
    an_scene_destroy(b);

    // scale 2: rect 100 x 60 local -> 200 x 120 on screen; local centers 20k + 10
    // -> screen x = 160 + 2 (20k + 10) = 100, 140, 180, 220; y = 80, 120; radius 15.96 px
    an_scene *c = calm();
    an_rect(c, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 100, 60 }, .scale = { 2, 2 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
    render(c, 0);
    CHECK(BLUE(180, 120));       // center
    CHECK(BLUE(194, 120));       // 14.5 px away < 15.96 - 1
    CHECK(PAPER(197, 120));      // 17.5 px away > 15.96 + 1
    CHECK(BLUE(140, 80) && PAPER(160, 100));
    an_scene_destroy(c);
}

// Hatch, angle 0, spacing 20, density 0.25: lines where local y = 20k + 10, half
// width 0.125 x 20 = 2.5 px -> ink rows y in [107.5 + 20k, 112.5 + 20k].
//   row 110 (center 110.5): 0.5 px from the line center -> ink
//   row 108: 1.5 px -> ink;  row 105: 4.5 px -> paper;  row 114: 4.5 px -> paper
static void test_hatch(void) {
    an_scene *s = calm();
    an_rect(s, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 200, 120 }, .fill = AN_HEX(0x0078BF),
                                .pattern = { .kind = AN_PATTERN_HATCH, .size = 20, .density = 0.25f } });
    render(s, 0);
    CHECK(BLUE(150, 110) && BLUE(150, 108) && PAPER(150, 105) && PAPER(150, 114));
    CHECK(BLUE(150, 50) && PAPER(150, 45)); // k = -3: rows 47.5..52.5
    const double c = coverage(60, 40, 260, 160);
    printf("  hatch density 0.25: measured coverage %.4f\n", c);
    CHECK(fabs(c - 0.25) < 0.01);
    an_scene_destroy(s);
}

// The pattern is printed on the fill plate, so it moves with the riso offset.
// Chaos 1, ink width 2, offset (+4, -3): dot center (70, 50) moves to (74, 47).
//   pixel (80, 47): 6.52 px from (74, 47) -> ink;  10.8 px from (70, 50)
//   pixel (63, 50): 11.07 px from (74, 47) -> paper; 6.52 px from (70, 50)
static void test_pattern_with_riso(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H,
        .style = { .grain_off = true, .vignette_off = true, .boil_off = true } });
    an_rect(s, &(an_rect_opts){ .pos = { 160, 100 }, .size = { 200, 120 }, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 2,
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
    render(s, 0);
    CHECK(BLUE(80, 47) && PAPER(63, 50));
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_pattern: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));
    test_halftone();
    test_halftone_angle();
    test_anchoring();
    test_hatch();
    test_pattern_with_riso();
    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_pattern");
}
