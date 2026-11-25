// Script-level guarantees: randomness keyed by tree position (adding an object
// inside one subtree changes nothing outside it), frame dumps independent of the
// thread count, and an_obj_at_cursor reading exactly the tracks recorded so far.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { W = 1280, H = 720 };

static ani_offscreen g_off;
static ani_renderer g_r;

static void render(an_scene *s, int64_t frame, unsigned char *px) {
    void *pool = ani_pool_push();
    CHECK(ani_renderer_prepare(&g_r, s, frame, W, H));
    ani_offscreen_begin(&g_off, AN_BLACK);
    ani_renderer_draw(&g_r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&g_off, px);
    ani_pool_pop(pool);
}

// The full look (boil, riso, shadows, grain). Two callouts, a rect, a text and
// an array; variant 1 adds one more child inside the first callout. Objects
// keyed by tree position: everything outside that callout's subtree renders the
// same, pixel for pixel. (Under the old creation-counter keys every object made
// after the extra child boiled differently.)
static an_scene *keyed_scene(int extra, an_obj **c1_out) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .seed = 31, .style = { .shadow = true } });
    an_obj *c1 = an_callout(s, &(an_callout_opts){ .text = "one", .pos = { 200, 140 }, .target = { 380, 260 }, .bend = 0.3f });
    if (extra) an_rect(s, &(an_rect_opts){ .parent = c1, .pos = { 0, 60 }, .size = { 50, 30 }, .stroke_width = 4 });
    an_rect(s, &(an_rect_opts){ .pos = { 800, 200 }, .size = { 200, 120 }, .fill = AN_HEX(0x00C2FF), .stroke_width = 6 });
    an_text(s, &(an_text_opts){ .text = "ABC", .pos = { 700, 460 }, .size = 110, .fill = AN_HEX(0xFF2E88), .stroke_width = 5 });
    static const int v[4] = { 3, 1, 4, 1 };
    an_array(s, &(an_array_opts){ .pos = { 640, 620 }, .values = v, .count = 4, .cell = { 70, 70 } });
    an_callout(s, &(an_callout_opts){ .text = "two", .pos = { 1100, 560 }, .target = { 1000, 330 }, .bend = -0.3f });
    an_wait(s, 1.0f);
    *c1_out = c1;
    return s;
}

static void test_subtree_keys(void) {
    const size_t bytes = (size_t)W * H * 4;
    unsigned char *a = malloc(bytes), *b = malloc(bytes);
    an_obj *c1a, *c1b;
    an_scene *sa = keyed_scene(0, &c1a), *sb = keyed_scene(1, &c1b);
    an_seek(sb, 0);
    an_bounds bb = an_obj_bounds_at_cursor(c1b); // the callout with its extra child
    const int x0 = (int)bb.min.x - 40, y0 = (int)bb.min.y - 40, x1 = (int)bb.max.x + 40, y1 = (int)bb.max.y + 40;
    int differ = 0, compared = 0, inside_diff = 0;
    for (int64_t f = 0; f <= 50; f += 25) {
        render(sa, f, a);
        render(sb, f, b);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const bool same = memcmp(a + ((size_t)y * W + x) * 4, b + ((size_t)y * W + x) * 4, 3) == 0;
                if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
                    inside_diff += !same;
                    continue;
                }
                compared++;
                differ += !same;
            }
        }
    }
    if (differ) fprintf(stderr, "  %d of %d pixels outside the callout differ\n", differ, compared);
    CHECK(differ == 0 && compared > 1000000);
    CHECK(inside_diff > 0); // the extra child itself is visible
    an_scene_destroy(sa);
    an_scene_destroy(sb);
    free(a);
    free(b);
}

// Frame dumps: the same scene dumped with 1 encoder thread and with 8 gives
// byte-identical PNG files.
static void test_dump_threads(void) {
    an_obj *c;
    an_scene *s = keyed_scene(0, &c);
    mkdir("out", 0755);
    mkdir("out/test", 0755);
    mkdir("out/test/dump1", 0755);
    mkdir("out/test/dump8", 0755);
    const int64_t n1 = an_scene_save_frames(s, &(an_frames_desc){ .dir = "out/test/dump1", .every = 6, .threads = 1 });
    const int64_t n8 = an_scene_save_frames(s, &(an_frames_desc){ .dir = "out/test/dump8", .every = 6, .threads = 8 });
    CHECK(n1 == 11 && n8 == 11); // frames 0, 6, ..., 60
    int same = 0;
    for (int64_t f = 0; f <= 60; f += 6) {
        char p1[256], p8[256];
        snprintf(p1, sizeof p1, "out/test/dump1/frame_%04lld.png", (long long)f);
        snprintf(p8, sizeof p8, "out/test/dump8/frame_%04lld.png", (long long)f);
        FILE *a = fopen(p1, "rb"), *b = fopen(p8, "rb");
        bool eq = a && b;
        while (eq) {
            const int x = fgetc(a), y = fgetc(b);
            if (x != y) eq = false;
            if (x == EOF || y == EOF) break;
        }
        if (a) fclose(a);
        if (b) fclose(b);
        same += eq;
    }
    CHECK(same == 11);
    an_scene_destroy(s);
}

// an_obj_at_cursor mid-way through an async animation, and only the tracks
// recorded so far: a fade 1 -> 0 over 1 s (in-out) played async at 0; at a cursor
// of 0.25 s the eased progress is 4 x 0.25^3 = 0.0625, so opacity 0.9375. A track
// recorded afterwards (a fade to 0.2 starting at 0.25 s) doesn't change the value
// already read, and a query before recording it ignores it: the state at 0.25 s
// is then exactly the new track's start, the old value continuing (0.9375), and
// it diverges only later (at 0.5 s: in-out 0.5 of the way from 0.9375 to 0.2 =
// 0.56875).
static void test_cursor_query(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *o = an_rect(s, NULL);
    an_play_async(s, an_fade_to(o, 0.0f, &(an_anim_opts){ .duration = 1 }));
    an_wait(s, 0.25f);
    const an_obj_state before = an_obj_at_cursor(o);
    CHECK_NEAR(before.opacity, 0.9375, 1e-6);
    an_play_async(s, an_fade_to(o, 0.2f, &(an_anim_opts){ .duration = 0.5f }));
    CHECK_NEAR(before.opacity, 0.9375, 1e-6);           // a value, not a view
    CHECK_NEAR(an_obj_at_cursor(o).opacity, 0.9375, 1e-6); // the new track starts from it
    an_wait(s, 0.25f);
    CHECK_NEAR(an_obj_at_cursor(o).opacity, 0.9375 + (0.2 - 0.9375) * 0.5, 1e-6);
    an_scene_destroy(s);
}

int main(void) {
    // first: frame dumps set up (and shut down) their own Metal context, like
    // an_scene_export and an_scene_save_png
    test_dump_threads();
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_script: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    test_subtree_keys();
    test_cursor_query();
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_script");
}
