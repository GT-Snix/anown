// 12_eigen: "Eigenvectors: the directions that don't turn" (docs/eigen_script.md).
// One plane for the whole video; one function per chapter. Plane objects have
// fixed unique names with a chapter prefix ("ch0/ring/15"); scene-level objects
// live in a fixed-name group per chapter ("ch0"). So each chapter's random
// streams don't depend on the others.
//
//   ./build/examples/12_eigen [--chapter N] [--frames DIR] [--every N] [--export FILE.mp4]
//   --chapter N: render only chapter N's frames of the full timeline (its first
//   frame is the state chapter N-1 ends in; the pixels match the full render)
#include "anown.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NCH = 7, FPS = 30, RING = 24 };
// chapter durations, seconds (retime to the voiceover here)
static const float DUR[NCH] = { 30, 60, 90, 120, 90, 70, 30 };
static const bool BUILT[NCH] = { true, true, true, false, false, false, false };

static const float PI = 3.14159265358979f;
#define MAGENTA AN_HEX(0xE6007E)
#define CYAN AN_HEX(0x00B4D8)
#define CHARCOAL AN_HEX(0x16161A)
#define ORANGE AN_HEX(0xFF6A00)

static const an_mat2 A = { 3, 1, 0, 2 }; // eigenvectors (1, 0) [3] and (1, -1) [2]
static const an_mat2 IDENTITY = { 1, 0, 0, 1 };

static an_scene *S;
static an_obj *P; // the plane
static double ch_start[NCH], ch_end[NCH];
static an_obj *title; // made in chapter 0, used again in chapter 6

// ---- helpers a user ends up writing ----------------------------------------------------

// a name from a format (the library copies names); 8 rotating buffers
static const char *nm(const char *fmt, ...) {
    static char buf[8][64];
    static int k;
    char *b = buf[k++ % 8];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, 64, fmt, ap);
    va_end(ap);
    return b;
}

static an_vec2 mul(an_mat2 m, an_vec2 v) { return (an_vec2){ m.a * v.x + m.b * v.y, m.c * v.x + m.d * v.y }; }

// does M keep v on its line? (cross product 0, relative)
static bool stays(an_mat2 m, an_vec2 v) {
    const an_vec2 w = mul(m, v);
    const float cross = v.x * w.y - v.y * w.x;
    return fabsf(cross) <= 1e-6f * hypotf(v.x, v.y) * hypotf(w.x, w.y);
}

// an arrow from the origin: riso and shadow off for the whole arrow
static an_obj *arrow(const char *name, an_vec2 v, an_color c, float width, bool undrawn) {
    return an_plane_vector(P, v, &(an_arrow_opts){ .name = name, .stroke = c, .stroke_width = width, .undrawn = undrawn,
                                                   .riso = AN_OFF, .shadow = AN_OFF });
}

static void recolor(an_obj *o, an_color c, float dur) {
    an_play_async(S, an_stroke_to(o, c, &(an_anim_opts){ .duration = dur }));
    an_play_async(S, an_fill_to(o, c, &(an_anim_opts){ .duration = dur })); // the head
}

static an_obj *ring[RING];
// the 24-arrow ring, drawn on; returns how many arrows M keeps on their line
static void make_ring(const char *prefix, an_color c) {
    for (int k = 0; k < RING; k++) {
        const float a = (float)k * 15.0f * PI / 180.0f;
        ring[k] = arrow(nm("%s/ring/%d", prefix, k * 15), (an_vec2){ cosf(a), sinf(a) }, c, 5, true);
        an_play_async(S, an_draw(ring[k], &(an_anim_opts){ .duration = 0.4f }));
        an_wait(S, 0.04f);
    }
    an_wait(S, 0.4f);
}

static int ring_count_staying(an_mat2 m) {
    int n = 0;
    for (int k = 0; k < RING; k++) {
        const float a = (float)k * 15.0f * PI / 180.0f;
        n += stays(m, (an_vec2){ cosf(a), sinf(a) });
    }
    return n;
}

// the ring arrows M keeps on their lines turn magenta (along (1, 0)) or cyan
static void color_staying(an_mat2 m) {
    for (int k = 0; k < RING; k++) {
        const float a = (float)k * 15.0f * PI / 180.0f;
        const an_vec2 v = { cosf(a), sinf(a) };
        if (!stays(m, v)) continue;
        recolor(ring[k], fabsf(v.y) < 1e-6f ? MAGENTA : CYAN, 0.5f);
    }
    an_wait(S, 0.6f);
}

// fade a set of objects together and wait for it
static void fade_all(an_obj *const *objs, int n, float to, float dur) {
    an_handle h = { 0 };
    for (int k = 0; k < n; k++)
        if (objs[k]) h = an_play_async(S, an_fade_to(objs[k], to, &(an_anim_opts){ .duration = dur }));
    an_wait_until(h);
}

// a pale version of a color (toward the paper)
static an_color pale(an_color c, float t) {
    const an_color p = AN_HEX(0xF3EDE2);
    return (an_color){ c.r + (p.r - c.r) * t, c.g + (p.g - c.g) * t, c.b + (p.b - c.b) * t, 1 };
}

// a "pop" for a plane arrow: its v grows from 0 with a spring
static void pop_arrow(an_obj *a) {
    an_obj_set_scale(a, (an_vec2){ 0, 0 });
    an_play(S, an_scale_to(a, (an_vec2){ 1, 1 }, &(an_anim_opts){ .ease = an_spring(0.45f, 0.45f) }));
}

// a label anchored in the plane (moves with M, stays upright)
static an_obj *plane_label(const char *name, const char *text, an_vec2 at, an_color c, float size) {
    return an_text(S, &(an_text_opts){ .name = name, .parent = P, .text = text, .pos = at, .size = size,
                                       .font = an_font_mono(S), .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                       .fill = c, .riso = AN_OFF, .shadow = AN_OFF });
}

static an_obj *chapter_group(int ch) { return an_group(S, &(an_group_opts){ .name = nm("ch%d", ch) }); }

// hold until the chapter's duration is used up
static void end_chapter(int ch) {
    const double end = ch_start[ch] + DUR[ch], now = an_scene_time(S);
    if (now > end + 1e-9) fprintf(stderr, "chapter %d runs %.2f s over its %.0f s\n", ch, now - end, (double)DUR[ch]);
    else an_wait(S, (float)(end - now));
    ch_end[ch] = an_scene_time(S);
}

// ---- chapter 0: the hook ------------------------------------------------------------------

static void chapter0(void) {
    an_obj *g = chapter_group(0);
    make_ring("ch0", CHARCOAL);
    title = an_text(S, &(an_text_opts){ .parent = g, .text = "EIGENVECTORS", .pos = { 960, 540 }, .size = 170,
                                        .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE, .fill = MAGENTA,
                                        .stroke = CHARCOAL, .stroke_width = 8 });
    for (int k = 0; k < an_text_length(title); k++) {
        an_play_async(S, an_slap(an_text_glyph(title, k),
                                 &(an_verb_opts){ .enter = AN_ON, .impact_shake = k == an_text_length(title) - 1 ? 18.0f : 0.0f }));
        an_wait(S, 0.07f);
    }
    an_wait(S, 2.0f);
    // into the top-left corner (the text is centered on its pos)
    an_play_async(S, an_scale_to(title, (an_vec2){ 0.3f, 0.3f }, &(an_anim_opts){ .duration = 0.8f }));
    an_play(S, an_move_to(title, (an_vec2){ 250, 70 }, &(an_anim_opts){ .duration = 0.8f }));
    an_wait(S, 1.0f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 3 }));
    const int n = ring_count_staying(A);
    printf("chapter 0: %d of %d arrows stay on their line\n", n, RING);
    color_staying(A);
    an_wait(S, 3.0f);
    end_chapter(0);
}

// ---- chapter 1: vectors and directions --------------------------------------------------

enum { DOTS_X = 9, DOTS_Y = 5 };
static an_obj *c1_keep[8 + (2 * DOTS_X + 1) * (2 * DOTS_Y + 1)];
static int c1_n;

static void chapter1(void) {
    chapter_group(1); // (no scene-level objects in this chapter)
    // back to the identity; the ring goes
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all(ring, RING, 0, 0.8f);
    an_wait(S, 1.0f);
    // the axes
    an_obj *ax = an_plane_line(P, (an_vec2){ 0, 0 }, (an_vec2){ 1, 0 }, &(an_line_opts){ .name = "ch1/axis/x", .z = -5,
                                .stroke = CHARCOAL, .stroke_width = 4, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_obj *ay = an_plane_line(P, (an_vec2){ 0, 0 }, (an_vec2){ 0, 1 }, &(an_line_opts){ .name = "ch1/axis/y", .z = -5,
                                .stroke = CHARCOAL, .stroke_width = 4, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_play_async(S, an_draw(ax, &(an_anim_opts){ .duration = 1.2f }));
    an_play(S, an_draw(ay, &(an_anim_opts){ .duration = 1.2f }));
    an_wait(S, 1.0f);
    // v = (2, 1) pops in, labeled
    an_obj *v = arrow("ch1/v", (an_vec2){ 2, 1 }, CHARCOAL, 6, false);
    pop_arrow(v);
    an_obj *vl = plane_label("ch1/v/label", "v", (an_vec2){ 2.15f, 1.35f }, CHARCOAL, 44);
    an_obj_set_opacity(vl, 0);
    an_play(S, an_fade_to(vl, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_wait(S, 2.0f);
    // its line of directions
    an_obj *line = an_plane_line(P, (an_vec2){ 0, 0 }, (an_vec2){ 2, 1 }, &(an_line_opts){ .name = "ch1/v/line", .z = -4,
                                  .stroke = pale(CHARCOAL, 0.55f), .stroke_width = 2, .hidden = true, .shadow = AN_OFF });
    an_play(S, an_fade_to(line, 1, &(an_anim_opts){ .duration = 1.2f }));
    an_wait(S, 3.0f);
    // a probe sweeps once around the origin (rotation is clockwise: -2 pi turns it counter-clockwise)
    an_obj *probe = arrow("ch1/probe", (an_vec2){ 1.5f, 0 }, ORANGE, 6, false);
    pop_arrow(probe);
    an_play(S, an_rotate_to(probe, -2.0f * PI, &(an_anim_opts){ .duration = 8, .ease = AN_EASE_LINEAR }));
    an_play(S, an_fade_to(probe, 0, &(an_anim_opts){ .duration = 0.5f }));
    an_wait(S, 2.0f);
    // every point is the tip of some arrow: dots at the grid points...
    c1_n = 0;
    c1_keep[c1_n++] = v;
    c1_keep[c1_n++] = vl;
    c1_keep[c1_n++] = line;
    an_obj *dots[(2 * DOTS_X + 1) * (2 * DOTS_Y + 1)];
    int nd = 0;
    for (int y = DOTS_Y; y >= -DOTS_Y; y--)
        for (int x = -DOTS_X; x <= DOTS_X; x++) {
            an_obj *d = an_plane_dot(P, (an_vec2){ (float)x, (float)y }, &(an_circle_opts){
                                         .name = nm("ch1/dot/%d/%d", x, y), .radius = 5, .fill = CHARCOAL, .hidden = true,
                                         .riso = AN_OFF, .shadow = AN_OFF });
            dots[nd++] = d;
            c1_keep[c1_n++] = d;
        }
    an_anim fades[(2 * DOTS_X + 1) * (2 * DOTS_Y + 1)];
    for (int k = 0; k < nd; k++) fades[k] = an_fade_to(dots[k], 1, &(an_anim_opts){ .duration = 0.3f });
    an_stagger(S, nd, fades, 0.012f);
    an_wait(S, 1.5f);
    // ...and arrows draw to a few of them
    static const an_vec2 to[4] = { { 3, 2 }, { -2, 1 }, { -4, -3 }, { 1, -4 } };
    for (int k = 0; k < 4; k++) {
        an_obj *a = arrow(nm("ch1/to/%d", k), to[k], CHARCOAL, 5, true);
        an_play(S, an_draw(a, &(an_anim_opts){ .duration = 0.7f }));
        an_wait(S, 0.6f);
        c1_keep[c1_n++] = a;
    }
    end_chapter(1);
}

// ---- chapter 2: a transformation moves everything ---------------------------------------

// the plane point p in scene px at the identity (origin at the center, 1 unit = 100 px, y up)
static an_vec2 scene_at_identity(an_vec2 p) { return (an_vec2){ 960 + 100 * p.x, 540 - 100 * p.y }; }

// one vector's trip under A, against the line it started on (a thin pale scene line:
// it stays where it was while the plane moves)
static void trip(an_obj *g, const char *name, an_vec2 v) {
    const an_vec2 o = scene_at_identity((an_vec2){ 0, 0 }), d = { v.x / hypotf(v.x, v.y), -v.y / hypotf(v.x, v.y) };
    an_obj *line = an_line(S, &(an_line_opts){ .parent = g, .from = { o.x - d.x * 1300, o.y - d.y * 1300 },
                                               .to = { o.x + d.x * 1300, o.y + d.y * 1300 }, .stroke = pale(CHARCOAL, 0.55f),
                                               .stroke_width = 2, .hidden = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_obj *a = arrow(name, v, CHARCOAL, 6, false);
    an_play_async(S, an_fade_to(line, 1, &(an_anim_opts){ .duration = 0.8f }));
    pop_arrow(a);
    an_wait(S, 1.5f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 3 }));
    const an_vec2 w = mul(A, v);
    printf("chapter 2: (%g, %g) -> (%g, %g), %s its line\n", v.x, v.y, w.x, w.y, stays(A, v) ? "on" : "off");
    an_wait(S, 3.0f);
    an_play(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all((an_obj *[]){ a, line }, 2, 0, 0.6f);
    an_wait(S, 1.0f);
}

static void chapter2(void) {
    an_obj *g = chapter_group(2);
    // only the axes and the grid stay
    fade_all(c1_keep, c1_n, 0, 1.0f);
    an_wait(S, 1.0f);
    // the basis arrows under A: (1, 0) -> (3, 0), (0, 1) -> (1, 2)
    an_obj *e1 = arrow("ch2/e1", (an_vec2){ 1, 0 }, CHARCOAL, 6, true), *e2 = arrow("ch2/e2", (an_vec2){ 0, 1 }, CHARCOAL, 6, true);
    an_play_async(S, an_draw(e1, &(an_anim_opts){ .duration = 0.7f }));
    an_play(S, an_draw(e2, &(an_anim_opts){ .duration = 0.7f }));
    an_wait(S, 1.5f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 3 }));
    an_wait(S, 3.0f);
    an_play(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    an_wait(S, 1.0f);
    fade_all((an_obj *[]){ e1, e2 }, 2, 0, 0.6f);
    an_wait(S, 0.5f);
    // two vectors that leave their line
    trip(g, "ch2/v11", (an_vec2){ 1, 1 });
    trip(g, "ch2/v01", (an_vec2){ 0, 1 });
    // all 24, slower, and the count
    make_ring("ch2", CHARCOAL);
    an_wait(S, 1.0f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 6 }));
    const int left = RING - ring_count_staying(A);
    printf("chapter 2: %d of %d left their line\n", left, RING);
    an_obj *count = an_text(S, &(an_text_opts){ .parent = g, .text = nm("%d of %d left their line", left, RING),
                                                .pos = { 960, 985 }, .size = 64, .align = AN_ALIGN_CENTER,
                                                .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL, .hidden = true,
                                                .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S, an_fade_to(count, 1, &(an_anim_opts){ .duration = 0.6f }));
    end_chapter(2);
}

// ---- main -----------------------------------------------------------------------------------

int main(int argc, char **argv) {
    int chapter = -1, every = 30;
    const char *dir = NULL, *mp4 = NULL;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (strcmp(argv[k], "--chapter") == 0) chapter = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--frames") == 0) dir = argv[k + 1];
        else if (strcmp(argv[k], "--every") == 0) every = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--export") == 0) mp4 = argv[k + 1];
    }
    S = an_scene_create(&(an_scene_desc){ .width = 1920, .height = 1080, .fps = FPS, .seed = 12,
                                          .style = { .shadow = true, .grain_mode = AN_GRAIN_STATIC } });
    if (!S) return 1;
    P = an_plane(S, &(an_plane_opts){ .name = "plane" }); // 1 unit = 100 px at 1080p, origin at the center

    void (*const chapters[NCH])(void) = { chapter0, chapter1, chapter2, NULL, NULL, NULL, NULL };
    for (int c = 0; c < NCH; c++) {
        if (!BUILT[c]) continue;
        ch_start[c] = an_scene_time(S);
        chapters[c]();
        printf("chapter %d: %.2f .. %.2f s\n", c, ch_start[c], ch_end[c]);
    }
    printf("timeline %.2f s, frames 0..%lld\n", an_scene_time(S), (long long)an_scene_length(S));

    int rc = 0;
    if (chapter >= 0 && (chapter >= NCH || !BUILT[chapter])) {
        fprintf(stderr, "chapter %d isn't built yet\n", chapter);
        return 1;
    }
    if (dir) {
        const int64_t first = chapter >= 0 ? (int64_t)llround(ch_start[chapter] * FPS) : 0;
        const int64_t last = chapter >= 0 ? (int64_t)llround(ch_end[chapter] * FPS) : an_scene_length(S);
        const int64_t n = an_scene_save_frames(S, &(an_frames_desc){ .dir = dir, .every = every, .first = first, .last = last,
                                                                     .prefix = chapter >= 0 ? nm("ch%d", chapter) : "eigen" });
        if (n >= 0) printf("wrote %lld PNG frames (%lld..%lld, every %d) to %s\n", (long long)n, (long long)first,
                           (long long)last, every, dir);
        rc = n >= 0 ? 0 : 1;
    }
    if (mp4 && rc == 0) {
        const int64_t n = an_scene_export(S, &(an_export_desc){ .path = mp4 });
        rc = n > 0 ? 0 : 1;
    }
    if (!dir && !mp4) an_scene_preview(S);
    an_scene_destroy(S);
    return rc;
}
