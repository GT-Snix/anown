// Camera (docs/camera.md): zoom vs the riso offset, max_zoom, text snapping and
// the size factor under zoom, boil coarsening, the translation fast path.
// Real Metal output (4x MSAA, readback); every expected value is worked out by
// hand in the comments.
//
// paper #F3EDE2 = (243, 237, 226); blue #0078BF = (0, 120, 191);
// charcoal #16161A = (22, 22, 26); shadow #3255A4 = (50, 85, 164)
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 960, H = 540 };

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
#define CHARCOAL(x, y) is(x, y, 22, 22, 26)

static an_scene *scene(bool shadow) {
    return an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 4,
                                             .style = { .grain_off = true, .vignette_off = true, .boil_off = true,
                                                        .shadow = shadow } });
}

// A 60x40 rect at the scene's center (480, 270), blue fill, charcoal ink of width
// 4, chaos 1 (riso offset (+4, -3)), no shadow, camera at (480, 270).
//   zoom 1: fill x 450..510 -> +4 = 454..514, y 250..290 -> -3 = 247..287;
//           left ink band x 448..452: columns 452, 453 are the riso sliver (paper),
//           454 is fill
//   zoom 2: everything about the center doubles: fill x 420..540, y 230..310; the ink
//           band is 8 wide (416..424 on the left, 536..544 on the right, 226..234
//           on top). The riso offset stays (+4, -3) on screen: fill x 424..544,
//           y 227..307. So the sliver is gone (the ink's inner edge is 424 = the
//           fill's left edge): columns 424 and 427 are fill (a doubled offset of 8
//           would leave 424..427 paper), row 225 at x 480 is paper (a doubled -6
//           would put fill there, above the ink's outer edge at 226), and the outer
//           edges are 416 and 544.
static void test_zoom_riso(void) {
    for (int zoom = 1; zoom <= 2; zoom++) {
        an_scene *s = scene(false);
        an_rect(s, &(an_rect_opts){ .pos = { 480, 270 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF),
                                    .stroke = AN_HEX(0x16161A), .stroke_width = 4 });
        an_camera_set_zoom(s, (float)zoom);
        render(s, 0);
        if (zoom == 1) {
            CHECK(CHARCOAL(449, 270) && PAPER(452, 270) && PAPER(453, 270) && BLUE(454, 270) && PAPER(447, 270));
        } else {
            CHECK(BLUE(424, 270) && BLUE(427, 270) && CHARCOAL(416, 270) && CHARCOAL(423, 270) && PAPER(415, 270));
            CHECK(CHARCOAL(543, 270) && PAPER(544, 270));
            CHECK(PAPER(480, 225) && CHARCOAL(480, 226) && BLUE(480, 300) && BLUE(480, 305) && CHARCOAL(480, 306) && CHARCOAL(480, 313) && PAPER(480, 314));
        }
        an_scene_destroy(s);
    }
}

// max_zoom (default 4): a rect at scale 1.5 under camera zoom 3 is at 4.5x on
// screen and warns; at scale 1.3 it's 3.9x and doesn't.
static void test_max_zoom(void) {
    for (int over = 0; over < 2; over++) {
        an_scene *s = scene(false);
        an_rect(s, &(an_rect_opts){ .pos = { 480, 270 }, .size = { 20, 20 }, .scale = { over ? 1.5f : 1.3f, over ? 1.5f : 1.3f } });
        an_camera_set_zoom(s, 3.0f);
        g_r.warned_zoom = false;
        render(s, 0);
        CHECK_NEAR(g_r.mesh.max_screen_scale, over ? 4.5 : 3.9, 1e-3);
        CHECK(g_r.warned_zoom == (over == 1));
        an_scene_destroy(s);
    }
    g_r.warned_zoom = false;
}

// Zooming into code text (30 px mono, 3 letters "abc", no box; scene shadows on,
// boil on at the default 1.5 px, so the size factor's cap applies):
// the camera zooms 1 -> 3 over frames 0..60, holds to 90, back to 1 at 150.
//   chaos: at zoom 3 the stem is 2.7 x 3 = 8.1 px: min(clamp(5.1 / 5) = 1,
//          0.3 x 8.1 / 3 = 0.81) = 0.81, so chaos rises from 0 (frame 0) to 0.81
//   shadow: code text has its shadow off, so no shadow pixel ever appears
//   snapping: on at frame 0 (zoom exactly 1), off on every frame where the zoom
//          isn't 1, on again from frame 150 (the track ends at exactly 1)
static void test_zoom_code(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 4,
                                                    .style = { .grain_off = true, .vignette_off = true, .shadow = true } });
    an_obj *c = an_code(s, &(an_code_opts){ .pos = { 400, 240 }, .code = "abc", .no_box = true });
    an_play(s, an_camera_zoom_to(s, 3.0f, &(an_anim_opts){ .duration = 1.0f }));
    an_wait(s, 0.5f);
    an_play(s, an_camera_zoom_to(s, 1.0f, &(an_anim_opts){ .duration = 1.0f }));
    an_wait(s, 0.2f);
    an_obj *a = an_text_glyph(an_code_text(c, 1), 0);
    int shadow_px = 0, flips = 0, prev = -1;
    for (int64_t f = 0; f <= an_scene_length(s); f++) {
        render(s, f);
        const int snapped = g_r.mesh.snapped_letters > 0;
        if (prev >= 0 && snapped != prev) flips++;
        prev = snapped;
        if (f == 0 || f >= 150) CHECK(g_r.mesh.snapped_letters == 3);
        if (f > 0 && f < 150) CHECK(g_r.mesh.snapped_letters == 0);
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) shadow_px += at(x, y)[0] == 50 && at(x, y)[1] == 85 && at(x, y)[2] == 164;
    }
    CHECK(shadow_px == 0 && flips == 2); // off once, back on once
    an_seek(s, 75);
    CHECK_NEAR(an_obj_chaos(a), 0.81, 1e-4);
    an_seek(s, 0);
    CHECK(an_obj_chaos(a) == 0.0f);
    an_scene_destroy(s);
}

// Boil coarsening: spacing 15 local px (default), scale s on screen; the smallest
// power of two m with 15 m s >= 6: zoom 1 -> 15; 0.4 -> 15 (exactly 6 px); 0.3 ->
// 4.5 px, so 30; 0.1 -> 1.5 px: 60. During a zoom-out it changes only at boil
// steps: frames with the same (f x 12) / 60 have the same spacing.
static void test_boil_coarse(void) {
    const float zooms[4] = { 1.0f, 0.4f, 0.3f, 0.1f }, want[4] = { 15, 15, 30, 60 };
    for (int i = 0; i < 4; i++) {
        an_scene *s = scene(false);
        an_obj *r = an_rect(s, &(an_rect_opts){ .pos = { 480, 270 } });
        an_camera_set_zoom(s, zooms[i]);
        CHECK(ani_boil_spacing(s, r, ani_frame_time(s, 0)) == want[i]);
        an_scene_destroy(s);
    }
    an_scene *s = scene(false);
    an_obj *r = an_rect(s, &(an_rect_opts){ .pos = { 480, 270 } });
    an_play(s, an_camera_zoom_to(s, 0.05f, &(an_anim_opts){ .duration = 2.0f, .ease = AN_EASE_LINEAR }));
    int changes = 0, bad = 0;
    float prev = 0;
    for (int64_t f = 0; f <= 120; f++) {
        const float sp = ani_boil_spacing(s, r, ani_frame_time(s, f));
        if (f > 0 && sp != prev) {
            changes++;
            bad += (f * 12) % 60 != 0; // must be the first frame of a boil step
        }
        prev = sp;
    }
    CHECK(changes >= 3 && bad == 0); // 15 -> 30 -> 60 -> 120 over the zoom-out
    an_scene_destroy(s);
}

// Shake: intensity 12 px, 0.5 s, triggered at 0.2 s (frame 12 at 60 fps); the
// camera's base position stays (480, 270).
//   before: frames 0..11 -> shake exactly {0, 0}
//   from the end on: t >= 0.7 s, frame 42 and later -> exactly {0, 0}
//   frame 21: 0.15 s after the trigger: u = 0.3, trauma 0.7, amplitude 12 x 0.49
//     = 5.88; noise at k = 0.15 x 15 = 2.25: samples 2 and 3, f = 0.25, smoothstep
//     w = 0.25^2 (3 - 0.5) = 0.15625; x uses stream 0x5EA4E000, y 0x5EA4E001
//   the same values whatever order the frames are sampled in
static void test_shake(void) {
    an_scene *s = scene(false);
    an_rect(s, &(an_rect_opts){ .pos = { 480, 270 }, .size = { 200, 120 }, .fill = AN_HEX(0x0078BF) });
    an_wait(s, 0.2f);
    an_play(s, an_shake(s, 12.0f, &(an_anim_opts){ .duration = 0.5f }));
    an_wait(s, 0.3f);
    const int64_t L = an_scene_length(s);
    CHECK(L == 60);
    an_vec2 fwd[61], rev[61], shuf[61];
    for (int64_t f = 0; f <= L; f++) { an_seek(s, f); fwd[f] = an_camera_get(s).shake; }
    for (int64_t f = L; f >= 0; f--) { an_seek(s, f); rev[f] = an_camera_get(s).shake; }
    an_rng rng = an_rng_make(5, 5);
    int order[61];
    for (int i = 0; i <= 60; i++) order[i] = i;
    for (int i = 60; i > 0; i--) { const int j = (int)an_rng_below(&rng, (uint32_t)i + 1); const int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int i = 0; i <= 60; i++) { an_seek(s, order[i]); shuf[order[i]] = an_camera_get(s).shake; }
    CHECK(memcmp(fwd, rev, sizeof fwd) == 0 && memcmp(fwd, shuf, sizeof fwd) == 0);
    int rest_before = 0, rest_after = 0, moving = 0;
    for (int f = 0; f <= 60; f++) {
        const bool zero = fwd[f].x == 0.0f && fwd[f].y == 0.0f;
        if (f < 12) rest_before += zero;
        else if (f >= 42) rest_after += zero;
        else moving += !zero;
        an_seek(s, f);
        CHECK(an_camera_get(s).pos.x == 480.0f && an_camera_get(s).pos.y == 270.0f); // the base never moves
    }
    CHECK(rest_before == 12 && rest_after == 19 && moving == 30);
    // frame 21 by hand
    const double w = 0.15625;
    double n[2];
    for (int c = 0; c < 2; c++) {
        const double h0 = an_hash_float(4, 0x5EA4E000ull + (uint64_t)c, 2), h1 = an_hash_float(4, 0x5EA4E000ull + (uint64_t)c, 3);
        n[c] = (h0 + (h1 - h0) * w) * 2.0 - 1.0;
    }
    CHECK_NEAR(fwd[21].x, 5.88 * n[0], 1e-5);
    CHECK_NEAR(fwd[21].y, 5.88 * n[1], 1e-5);
    // in pixels: at rest (frame 60) the frame is byte-identical to frame 0
    const size_t bytes = (size_t)W * H * 4;
    unsigned char *f0 = malloc(bytes);
    render(s, 0);
    memcpy(f0, g_px, bytes);
    render(s, 60);
    CHECK(memcmp(f0, g_px, bytes) == 0);
    render(s, 21);
    CHECK(memcmp(f0, g_px, bytes) != 0); // and it does move mid-shake
    free(f0);
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_camera: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    test_zoom_riso();
    test_max_zoom();
    test_zoom_code();
    test_boil_coarse();
    test_shake();

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_camera");
}
