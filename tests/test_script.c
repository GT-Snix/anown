// Script-level guarantees: randomness keyed by tree position (adding an object
// inside one subtree changes nothing outside it), frame dumps independent of the
// thread count, and an_obj_at_cursor reading exactly the tracks recorded so far.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

// ---- an_text_set (docs/api-fixes.md, item 2) -------------------------------------------------

static an_obj *mono(an_scene *s, const char *str) {
    return an_text(s, &(an_text_opts){ .text = str, .pos = { 640, 300 }, .size = 60, .font = an_font_mono(s),
                                       .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE });
}

static double opacity_at(an_obj *o, double sec) {
    return ani_sample(ani_obj_prop(o, AN_PROP_OPACITY), ani_tick_time((int64_t)llround(sec * 1e6))).v[0];
}

// CROSSFADE, linear, D 0.3 s: at 0.15 s both versions are at 0.5. Stagger 0.5: each
// fade is 0.3 / 1.5 = 0.2 s and the fade-in starts at 0.1 s: at 0.1 s old 0.5 new 0;
// at 0.2 s old 0 new 0.5. Queries: before the change plays, the old text; from its
// start (mid-crossfade), the new one. After it, the letters sit exactly where a fresh
// text of the new string puts them, and the bounds are the fresh text's.
static void test_text_crossfade(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *t = mono(s, "insert 42");
    const an_anim_opts lin = { .duration = 0.3f, .ease = AN_EASE_LINEAR };
    an_anim a = an_text_set(t, "height: 3", &(an_text_set_opts){ .anim = lin });
    CHECK(an_anim_ok(a) && an_text_length(t) == 8); // not played yet: "insert42" (8 letters)
    an_obj *v0 = t->u.text.versions[0], *v1 = t->u.text.versions[1];
    const an_handle h = an_play_async(s, a);
    CHECK(h.ok && an_text_length(t) == 8 && an_text_glyph(t, 0)->parent == v1); // "height:3"
    CHECK_NEAR(opacity_at(v0, 0.15), 0.5, 1e-6);
    CHECK_NEAR(opacity_at(v1, 0.15), 0.5, 1e-6);
    an_wait_until(h);
    an_play(s, an_text_set(t, "n = 12", &(an_text_set_opts){ .anim = lin, .stagger = 0.5f }));
    an_obj *v2 = t->u.text.versions[2];
    CHECK_NEAR(opacity_at(v1, 0.3 + 0.1), 0.5, 1e-6);
    CHECK_NEAR(opacity_at(v2, 0.3 + 0.1), 0.0, 1e-6);
    CHECK_NEAR(opacity_at(v1, 0.3 + 0.2), 0.0, 1e-6);
    CHECK_NEAR(opacity_at(v2, 0.3 + 0.2), 0.5, 1e-6);
    // where a fresh text of the new string puts its letters, and the same bounds
    an_obj *fresh = mono(s, "n = 12");
    CHECK(an_text_length(t) == an_text_length(fresh));
    for (int k = 0; k < an_text_length(t); k++) {
        const an_vec2 p = an_text_glyph(t, k)->pos, q = an_text_glyph(fresh, k)->pos;
        CHECK(p.x == q.x && p.y == q.y);
    }
    const an_bounds b = an_obj_bounds_at_cursor(t), c = an_obj_bounds_at_cursor(fresh);
    CHECK(b.min.x == c.min.x && b.min.y == c.min.y && b.max.x == c.max.x && b.max.y == c.max.y);
    // the old versions are never seen again after their fades
    CHECK(!ani_text_version_live(v0, ani_tick_time(300001)) && !ani_text_version_live(v1, ani_tick_time(500001)));
    CHECK(ani_text_version_live(v2, ani_tick_time(10000000)));
    an_scene_destroy(s);
}

// LETTERS, "insert 42" -> "insert 17" (mono, centered; linear 0.3 s, gap 0.03 s): the
// LCS is "insert " (7); the letters i n s e r t (0..5) are matched with zero movement
// (same length, same advance); "4", "2" fade out at 0 and 0.03 s, "1", "7" fade in at
// 0 and 0.03 s. At 0.03 s: old "4" 1 - 0.03/0.3 = 0.9, old "2" 1, new "1" 0.1, new "7" 0.
// Matched letters take over their old letter's random stream.
static void test_text_letters(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *t = mono(s, "insert 42");
    an_play(s, an_text_set(t, "insert 17", &(an_text_set_opts){ .mode = AN_TEXT_LETTERS,
                                                                .anim = { .duration = 0.3f, .ease = AN_EASE_LINEAR } }));
    an_obj *v0 = t->u.text.versions[0], *v1 = t->u.text.versions[1];
    for (int k = 0; k < 6; k++) {
        CHECK(v1->u.tver.match[k] == k);
        an_obj *o = v0->u.tver.glyphs[k], *n = v1->u.tver.glyphs[k];
        CHECK(o->pos.x == n->pos.x && o->pos.y == n->pos.y && o->rng_key == n->rng_key);
    }
    CHECK(v1->u.tver.match[6] == -1 && v1->u.tver.match[7] == -1);
    CHECK_NEAR(opacity_at(v0->u.tver.glyphs[6], 0.03), 0.9, 1e-6);
    CHECK_NEAR(opacity_at(v0->u.tver.glyphs[7], 0.03), 1.0, 1e-6);
    CHECK_NEAR(opacity_at(v1->u.tver.glyphs[6], 0.03), 0.1, 1e-6);
    CHECK_NEAR(opacity_at(v1->u.tver.glyphs[7], 0.03), 0.0, 1e-6);
    // matched letters hand over in place at the start
    CHECK(opacity_at(v0->u.tver.glyphs[0], 0.0) == 0.0 && opacity_at(v1->u.tver.glyphs[0], 0.0) == 1.0);
    an_obj *fresh = mono(s, "insert 17");
    for (int k = 0; k < 8; k++) {
        const an_vec2 p = an_obj_at_cursor(an_text_glyph(t, k)).pos, q = an_text_glyph(fresh, k)->pos;
        CHECK(p.x == q.x && p.y == q.y);
    }
    an_scene_destroy(s);
}

// Repeated letters, by hand (docs/api-fixes.md): "abab" -> "ba" keeps old letters 1
// and 2 (new 0 <- old 1, new 1 <- old 2); "aa" -> "a" keeps old letter 1.
static void test_text_repeats(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *t = mono(s, "abab");
    an_play(s, an_text_set(t, "ba", &(an_text_set_opts){ .mode = AN_TEXT_LETTERS }));
    const an_obj *v = t->u.text.versions[1];
    CHECK(v->u.tver.match[0] == 1 && v->u.tver.match[1] == 2);
    an_obj *u = mono(s, "aa");
    an_play(s, an_text_set(u, "a", &(an_text_set_opts){ .mode = AN_TEXT_LETTERS }));
    CHECK(u->u.text.versions[1]->u.tver.match[0] == 1);
    an_scene_destroy(s);
}

// The limit: 255 sets give 256 versions; the 257th fails with a message naming the
// text, an_anim_ok false, and playing it records nothing (cursor and versions
// unchanged, handle not ok); the text still shows the 256th version.
static void test_text_limit(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *t = an_text(s, &(an_text_opts){ .text = "count 0", .name = "counter", .font = an_font_mono(s) });
    char buf[32];
    int ok = 0;
    for (int k = 1; k <= 255; k++) {
        snprintf(buf, sizeof buf, "count %d", k);
        const an_anim a = an_text_set(t, buf, &(an_text_set_opts){ .anim = { .duration = 0.05f } });
        ok += an_anim_ok(a) && an_play(s, a).ok;
    }
    CHECK(ok == 255 && t->u.text.nver == 256);
    // capture stderr for the failing call
    fflush(stderr);
    const int saved = dup(2);
    FILE *cap = tmpfile();
    dup2(fileno(cap), 2);
    const an_anim bad = an_text_set(t, "count 256", NULL);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    char msg[256] = { 0 };
    rewind(cap);
    const size_t got = fread(msg, 1, sizeof msg - 1, cap);
    fclose(cap);
    CHECK(got > 0 && strstr(msg, "an_text_set on text \"counter\"") && strstr(msg, "256 versions"));
    CHECK(!an_anim_ok(bad));
    const double before = an_scene_time(s);
    const an_handle h = an_play(s, bad);
    CHECK(!h.ok && h.start == before && h.end == before && an_scene_time(s) == before && t->u.text.nver == 256);
    CHECK(an_text_length(t) == 8); // "count255"
    an_scene_destroy(s);
}

// Missing glyphs (docs/plane.md): Archivo Black has no U+03BB (checked in its cmap);
// three texts using it give one message naming the character and the font.
// JetBrains Mono has it: no message.
static void test_missing_glyph(void) {
    an_scene *s = an_scene_create(NULL);
    fflush(stderr);
    const int saved = dup(2);
    FILE *cap = tmpfile();
    dup2(fileno(cap), 2);
    an_text(s, &(an_text_opts){ .text = "\xCE\xBB" });
    an_text(s, &(an_text_opts){ .text = "a \xCE\xBB \xCE\xBB" });
    an_text(s, &(an_text_opts){ .text = "\xCE\xBB = 3", .font = an_font_mono(s) });
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    char msg[512] = { 0 };
    rewind(cap);
    const size_t got = fread(msg, 1, sizeof msg - 1, cap);
    fclose(cap);
    int lines = 0;
    for (size_t i = 0; i < got; i++) lines += msg[i] == '\n';
    CHECK(lines == 1 && strstr(msg, "U+03BB") && strstr(msg, "Archivo"));
    if (lines != 1) fprintf(stderr, "%s", msg);
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
    test_text_crossfade();
    test_text_letters();
    test_text_repeats();
    test_text_limit();
    test_missing_glyph();
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_script");
}
