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
static const bool BUILT[NCH] = { true, true, true, true, true, true, true };

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

// fade a set of objects together and wait for it. Each object fading on its own is
// its own layer, and a frame can hold only so many (sokol's image pool): past 20,
// they fade in a quick ripple (0.3 s each, at most about 20 at once).
static void fade_all(an_obj *const *objs, int n, float to, float dur) {
    const bool ripple = n > 20;
    const float d = ripple ? 0.3f : dur, gap = ripple ? 0.3f / 20.0f : 0.0f;
    an_handle h = { 0 };
    for (int k = 0; k < n; k++) {
        if (!objs[k]) continue;
        h = an_play_async(S, an_fade_to(objs[k], to, &(an_anim_opts){ .duration = d }));
        if (ripple) an_wait(S, gap);
    }
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

// hold until `t` seconds into the chapter (sub-sections of a chapter)
static void hold_until(int ch, float t) {
    const double end = ch_start[ch] + t, now = an_scene_time(S);
    if (now > end + 1e-9) fprintf(stderr, "chapter %d: section ending at %.0f s runs %.2f s over\n", ch, (double)t, now - end);
    else an_wait(S, (float)(end - now));
}

// a callout (riso and shadow off) pointing at a scene point, drawn on
static an_obj *note(an_obj *g, const char *text, an_vec2 at, an_vec2 target, float bend) {
    an_obj *c = an_callout(S, &(an_callout_opts){ .parent = g, .text = text, .size = 42, .pos = at, .target = target,
                                                   .bend = bend, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_play(S, an_draw(c, &(an_anim_opts){ .duration = 0.9f }));
    return c;
}

static an_mat2 rotation(float deg) {
    const float r = deg * PI / 180.0f;
    return (an_mat2){ cosf(r), -sinf(r), sinf(r), cosf(r) };
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
                                        .stroke = CHARCOAL, .stroke_width = 8, .shadow = AN_ON });
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

static an_obj *c2_count;

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
    c2_count = an_text(S, &(an_text_opts){ .parent = g, .text = nm("%d of %d left their line", left, RING),
                                                .pos = { 960, 985 }, .size = 64, .align = AN_ALIGN_CENTER,
                                                .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL, .hidden = true,
                                                .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S, an_fade_to(c2_count, 1, &(an_anim_opts){ .duration = 0.6f }));
    end_chapter(2);
}

// ---- chapter 3: eigenvectors and eigenvalues ----------------------------------------------

static an_obj *c3_lines[2]; // the magenta and cyan span lines (kept for chapter 6)
static an_obj *c3_keep[16];  // everything else chapter 3 leaves on screen
static int c3_n;

// the angle between v and A v, in degrees (from the cross and dot products)
static float angle_v_av(an_vec2 v) {
    const an_vec2 w = mul(A, v);
    return fabsf(atan2f(v.x * w.y - v.y * w.x, v.x * w.x + v.y * w.y)) * 180.0f / PI;
}

static an_vec2 unit_at(float deg) { return (an_vec2){ cosf(deg * PI / 180.0f), sinf(deg * PI / 180.0f) }; }

static void chapter3(void) {
    an_obj *g = chapter_group(3);
    an_obj *fade[RING + 1];
    memcpy(fade, ring, sizeof ring);
    fade[RING] = c2_count;
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all(fade, RING + 1, 0, 1.0f);
    an_wait(S, 1.0f);

    // a) the probe: v sweeps from -30 to 150 degrees in steps of 2.5 (9 degrees a second);
    // a lighter arrow shows A v; the readout shows the angle between them
    enum { STEPS = 72 };
    const float deg0 = -30.0f, step = 2.5f, dt = step / 9.0f;
    an_obj *probe = arrow("ch3/probe", unit_at(deg0), ORANGE, 6, false);
    an_obj *av = arrow("ch3/av", mul(A, unit_at(deg0)), pale(ORANGE, 0.5f), 5, false);
    an_obj *avl = plane_label("ch3/av/label", "Av", mul(A, unit_at(deg0)), pale(ORANGE, 0.3f), 40);
    an_obj *readout = an_text(S, &(an_text_opts){ .parent = g, .text = nm("angle(v, Av) = %5.1f\xC2\xB0", angle_v_av(unit_at(deg0))),
                                                  .pos = { 1880, 70 }, .size = 46, .font = an_font_mono(S),
                                                  .align = AN_ALIGN_RIGHT, .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL,
                                                  .riso = AN_OFF, .shadow = AN_OFF, .hidden = true });
    pop_arrow(probe);
    an_play_async(S, an_fade_to(readout, 1, &(an_anim_opts){ .duration = 0.4f }));
    pop_arrow(av);
    an_wait(S, 1.0f);
    an_obj *rings[2] = { NULL, NULL };
    int hits = 0;
    const an_anim_opts lin = { .duration = dt, .ease = AN_EASE_LINEAR };
    for (int k = 1; k <= STEPS; k++) {
        const float deg = deg0 + step * (float)k;
        const an_vec2 v = unit_at(deg), w = mul(A, v);
        an_play_async(S, an_move_to(probe, v, &lin));
        an_play_async(S, an_move_to(av, w, &lin));
        an_play_async(S, an_move_to(avl, (an_vec2){ w.x * 1.12f, w.y * 1.12f + 0.25f }, &lin));
        an_play_async(S, an_text_set(readout, nm("angle(v, Av) = %5.1f\xC2\xB0", angle_v_av(v)),
                                     &(an_text_set_opts){ .anim = { .duration = 0.1f } }));
        an_wait(S, dt);
        if (!stays(A, v) || hits == 2) continue;
        // a hit: a ring on the arrow, the whole line in its color, and a 2 s hold
        const an_color c = fabsf(v.y) < 1e-6f ? MAGENTA : CYAN;
        printf("chapter 3: v at %.1f degrees stays on its line (A v = %.4g v)\n", deg, hypotf(w.x, w.y));
        rings[hits] = an_plane_dot(P, v, &(an_circle_opts){ .name = nm("ch3/ring/%d", hits), .radius = 30, .no_fill = true,
                                                             .stroke = c, .stroke_width = 6, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S, an_slap(rings[hits], &(an_verb_opts){ .enter = AN_ON }));
        c3_lines[hits] = an_plane_line(P, (an_vec2){ 0, 0 }, v, &(an_line_opts){ .name = nm("ch3/span/%d", hits), .z = -3,
                                       .stroke = c, .stroke_width = 4, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S, an_draw(c3_lines[hits], &(an_anim_opts){ .duration = 1.2f }));
        an_wait(S, 2.0f);
        hits++;
    }
    fade_all((an_obj *[]){ probe, av, avl, readout, rings[0], rings[1] }, 6, 0, 0.6f);
    an_wait(S, 1.0f);

    // b) along each line, A only stretches: points on them slide along them
    static const an_vec2 on[8] = { { 1, 0 }, { 2, 0 }, { -1, 0 }, { -2, 0 }, { 1, -1 }, { 2, -2 }, { -1, 1 }, { -2, 2 } };
    an_obj *dots[8];
    for (int k = 0; k < 8; k++)
        dots[k] = an_plane_dot(P, on[k], &(an_circle_opts){ .name = nm("ch3/dot/%d", k), .radius = 9, .hidden = true,
                                                             .fill = k < 4 ? MAGENTA : CYAN, .riso = AN_OFF, .shadow = AN_OFF });
    fade_all(dots, 8, 1, 0.5f);
    an_wait(S, 1.5f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 3 }));
    an_wait(S, 0.5f);
    // ticks at the images of (1, 0) and (1, -1), with the stretch
    an_obj *marks[4];
    for (int k = 0; k < 2; k++) {
        const an_vec2 u = k ? (an_vec2){ 1, -1 } : (an_vec2){ 1, 0 };
        // the plane is at A now: the point_at_cursor of u is already its image A u
        const an_vec2 q = an_plane_point_at_cursor(P, u), o = an_plane_point_at_cursor(P, (an_vec2){ 0, 0 });
        const float lam = hypotf(mul(A, u).x, mul(A, u).y) / hypotf(u.x, u.y);
        an_vec2 n = { -(q.y - o.y), q.x - o.x }; // normal to the line, on screen
        const float nl = hypotf(n.x, n.y);
        n = (an_vec2){ n.x / nl, n.y / nl };
        marks[2 * k] = an_line(S, &(an_line_opts){ .parent = g, .from = { q.x - n.x * 22, q.y - n.y * 22 },
                                                   .to = { q.x + n.x * 22, q.y + n.y * 22 }, .stroke = k ? CYAN : MAGENTA,
                                                   .stroke_width = 5, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        marks[2 * k + 1] = an_text(S, &(an_text_opts){ .parent = g, .text = nm("\xC3\x97%g", lam),
                                                        .pos = { q.x - n.x * 60, q.y - n.y * 60 }, .size = 52,
                                                        .font = an_font_mono(S), .align = AN_ALIGN_CENTER,
                                                        .anchor = AN_ANCHOR_MIDDLE, .fill = k ? CYAN : MAGENTA, .hidden = true,
                                                        .riso = AN_OFF, .shadow = AN_OFF });
        an_play(S, an_draw(marks[2 * k], &(an_anim_opts){ .duration = 0.4f }));
        an_play(S, an_fade_to(marks[2 * k + 1], 1, &(an_anim_opts){ .duration = 0.4f }));
        an_wait(S, 0.8f);
    }
    an_wait(S, 3.0f);

    // c) the equation
    an_obj *eq = an_text(S, &(an_text_opts){ .parent = g, .text = "A v = \xCE\xBB v", .pos = { 960, 175 }, .size = 120,
                                             .font = an_font_mono(S), .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                             .fill = CHARCOAL, .riso = AN_OFF, .shadow = AN_OFF });
    for (int k = 0; k < an_text_length(eq); k++) {
        an_play_async(S, an_slap(an_text_glyph(eq, k), &(an_verb_opts){ .enter = AN_ON }));
        an_wait(S, 0.08f);
    }
    an_wait(S, 1.5f);
    // letters (no spaces): A v = lambda v -> the lambda is 3, the last v is 4
    const an_bounds lb = an_obj_bounds_at_cursor(an_text_glyph(eq, 3)), vb = an_obj_bounds_at_cursor(an_text_glyph(eq, 4));
    an_obj *same = an_callout(S, &(an_callout_opts){ .parent = g, .text = "same direction", .size = 40,
                                                       .pos = { 1450, 330 }, .target = { (vb.min.x + vb.max.x) * 0.5f, vb.max.y + 8 },
                                                       .bend = 0.3f, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_obj *scaled = an_callout(S, &(an_callout_opts){ .parent = g, .text = "just scaled", .size = 40,
                                                         .pos = { 560, 330 }, .target = { (lb.min.x + lb.max.x) * 0.5f, lb.max.y + 8 },
                                                         .bend = -0.3f, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_play(S, an_draw(same, &(an_anim_opts){ .duration = 0.8f }));
    an_wait(S, 1.0f);
    an_play(S, an_draw(scaled, &(an_anim_opts){ .duration = 0.8f }));
    an_wait(S, 3.0f);
    an_obj *l1 = an_text(S, &(an_text_opts){ .parent = g, .text = nm("\xCE\xBB\xE2\x82\x81 = %g", 3.0), .pos = { 120, 880 },
                                             .size = 64, .font = an_font_mono(S), .anchor = AN_ANCHOR_MIDDLE, .fill = MAGENTA,
                                             .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *l2 = an_text(S, &(an_text_opts){ .parent = g, .text = nm("\xCE\xBB\xE2\x82\x82 = %g", 2.0), .pos = { 120, 970 },
                                             .size = 64, .font = an_font_mono(S), .anchor = AN_ANCHOR_MIDDLE, .fill = CYAN,
                                             .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S, an_fade_to(l1, 1, &(an_anim_opts){ .duration = 0.5f }));
    an_play(S, an_fade_to(l2, 1, &(an_anim_opts){ .duration = 0.5f }));
    an_wait(S, 3.0f);

    // d) the name
    fade_all((an_obj *[]){ same, scaled }, 2, 0, 0.5f);
    const an_bounds tb = an_obj_bounds_at_cursor(title);
    an_obj *name = an_callout(S, &(an_callout_opts){ .parent = g, .text = "eigen = German for \"own\", or \"characteristic\"",
                                                       .size = 40, .pos = { 760, 330 },
                                                       .target = { (tb.min.x + tb.max.x) * 0.5f, tb.max.y + 6 }, .bend = -0.3f,
                                                       .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    an_play(S, an_draw(name, &(an_anim_opts){ .duration = 1.0f }));
    c3_n = 0;
    for (int k = 0; k < 8; k++) c3_keep[c3_n++] = dots[k];
    for (int k = 0; k < 4; k++) c3_keep[c3_n++] = marks[k];
    c3_keep[c3_n++] = eq;
    c3_keep[c3_n++] = l1;
    c3_keep[c3_n++] = l2;
    c3_keep[c3_n++] = name;
    end_chapter(3);
}

// ---- chapter 4: edge cases ---------------------------------------------------------------

static an_obj *c4_left[RING + 4];
// what the chapter before 6 leaves on screen (set by that chapter)
static an_obj **prev_left;
static int prev_n;

static void chapter4(void) {
    an_obj *g = chapter_group(4);
    an_obj *fade[18];
    int nf = 0;
    for (int k = 0; k < c3_n; k++) fade[nf++] = c3_keep[k];
    fade[nf++] = c3_lines[0];
    fade[nf++] = c3_lines[1];
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all(fade, nf, 0, 1.0f);
    an_wait(S, 1.0f);

    // a) negative: B flips the x-axis arrow but keeps it on its line
    const an_mat2 B = { -1, 0, 0, 2 };
    an_obj *x = arrow("ch4/a/x", (an_vec2){ 1.5f, 0 }, CHARCOAL, 6, true), *y = arrow("ch4/a/y", (an_vec2){ 0, 1.5f }, CHARCOAL, 6, true);
    an_play_async(S, an_draw(x, &(an_anim_opts){ .duration = 0.6f }));
    an_play(S, an_draw(y, &(an_anim_opts){ .duration = 0.6f }));
    an_wait(S, 1.5f);
    an_play(S, an_plane_matrix_to(P, B, &(an_anim_opts){ .duration = 3 }));
    printf("chapter 4a: B keeps (1, 0) on its line: %s; lambda = %g\n", stays(B, (an_vec2){ 1, 0 }) ? "yes" : "no",
           (double)mul(B, (an_vec2){ 1, 0 }).x);
    recolor(x, ORANGE, 0.5f);
    an_wait(S, 0.6f);
    const an_vec2 tip = an_plane_point_at_cursor(P, (an_vec2){ 1.5f, 0 });
    an_obj *ca = note(g, "lambda = -1: still on the line, pointing backwards", (an_vec2){ 640, 330 },
                      (an_vec2){ tip.x - 10, tip.y - 14 }, -0.3f);
    hold_until(4, 28.0f);
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all((an_obj *[]){ x, y, ca }, 3, 0, 1.0f);
    hold_until(4, 30.0f);

    // b) zero: C crushes the plane onto y = x; the direction (1, -1) goes to nothing
    const an_mat2 C = { 1, 1, 1, 1 };
    an_obj *u = arrow("ch4/b/u", (an_vec2){ 1, 1 }, CHARCOAL, 6, true), *w = arrow("ch4/b/w", (an_vec2){ 1, -1 }, CHARCOAL, 6, true);
    an_play_async(S, an_draw(u, &(an_anim_opts){ .duration = 0.6f }));
    an_play(S, an_draw(w, &(an_anim_opts){ .duration = 0.6f }));
    an_wait(S, 1.5f);
    an_play(S, an_plane_matrix_to(P, C, &(an_anim_opts){ .duration = 3 }));
    const an_vec2 cw = mul(C, (an_vec2){ 1, -1 });
    printf("chapter 4b: C (1, -1) = (%g, %g); C (1, 1) = 2 (1, 1)\n", (double)cw.x, (double)cw.y);
    recolor(u, ORANGE, 0.5f);
    an_wait(S, 0.6f);
    const an_vec2 o = an_plane_point_at_cursor(P, (an_vec2){ 0, 0 });
    an_obj *cb = note(g, "lambda = 0: this direction is crushed to nothing", (an_vec2){ 1300, 860 },
                      (an_vec2){ o.x + 12, o.y + 14 }, 0.3f);
    hold_until(4, 58.0f);
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all((an_obj *[]){ u, w, cb }, 3, 0, 1.0f);
    hold_until(4, 60.0f);

    // c) none: a quarter turn, in three keyframes so lengths don't shrink much
    make_ring("ch4", CHARCOAL);
    an_wait(S, 0.5f);
    // three of them in orange, each with the line it starts on (pale, fixed in the scene)
    static const int marked[3] = { 0, 4, 9 }; // 0, 60 and 135 degrees
    an_obj *lines[3];
    for (int k = 0; k < 3; k++) {
        const float a = (float)marked[k] * 15.0f * PI / 180.0f;
        const an_vec2 o = scene_at_identity((an_vec2){ 0, 0 }), d = { cosf(a), -sinf(a) };
        lines[k] = an_line(S, &(an_line_opts){ .parent = g, .from = { o.x - d.x * 1300, o.y - d.y * 1300 },
                                               .to = { o.x + d.x * 1300, o.y + d.y * 1300 }, .stroke = pale(ORANGE, 0.5f),
                                               .stroke_width = 2, .hidden = true, .shadow = AN_OFF, .riso = AN_OFF });
        an_play_async(S, an_fade_to(lines[k], 1, &(an_anim_opts){ .duration = 0.6f }));
        recolor(ring[marked[k]], ORANGE, 0.5f);
    }
    an_wait(S, 1.5f);
    for (int k = 1; k <= 3; k++)
        an_play(S, an_plane_matrix_to(P, rotation(30.0f * (float)k), &(an_anim_opts){ .duration = 1.2f, .ease = AN_EASE_LINEAR }));
    printf("chapter 4c: a quarter turn keeps %d of %d arrows on their line\n", ring_count_staying(rotation(90)), RING);
    an_wait(S, 0.8f);
    c4_left[RING] = note(g, "no arrow stays on its line: no real eigenvectors", (an_vec2){ 960, 900 },
                         an_plane_point_at_cursor(P, (an_vec2){ 0.2f, -1.05f }), 0.25f);
    memcpy(c4_left, ring, sizeof ring);
    for (int k = 0; k < 3; k++) c4_left[RING + 1 + k] = lines[k];
    prev_left = c4_left;
    prev_n = RING + 4;
    end_chapter(4);
}

// ---- chapter 5: why it matters --------------------------------------------------------------

static float deg_of(an_vec2 v) { return atan2f(v.y, v.x) * 180.0f / PI; }

// move unit arrows (radius r) along the circle from a0[i] to a1[i] degrees (the short
// way), all together, in `keys` linear steps over `dur` seconds
static void arcs(an_obj *const *objs, const float *a0, const float *a1, int n, float r, float dur, int keys) {
    const float dt = dur / (float)keys;
    for (int j = 1; j <= keys; j++) {
        for (int i = 0; i < n; i++) {
            float d = a1[i] - a0[i];
            while (d > 180.0f) d -= 360.0f;
            while (d <= -180.0f) d += 360.0f;
            if (d == 0.0f) continue;
            const float a = (a0[i] + d * (float)j / (float)keys) * PI / 180.0f;
            an_play_async(S, an_move_to(objs[i], (an_vec2){ r * cosf(a), r * sinf(a) },
                                        &(an_anim_opts){ .duration = dt, .ease = AN_EASE_LINEAR }));
        }
        an_wait(S, dt);
    }
}

static an_obj *c5_left[RING + 3];

static void chapter5(void) {
    an_obj *g = chapter_group(5);
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all(prev_left, prev_n, 0, 1.0f);
    an_wait(S, 1.0f);
    an_obj *span[2];
    for (int k = 0; k < 2; k++) {
        span[k] = an_plane_line(P, (an_vec2){ 0, 0 }, k ? (an_vec2){ 1, -1 } : (an_vec2){ 1, 0 },
                                &(an_line_opts){ .name = nm("ch5/span/%d", k), .z = -3, .stroke = k ? CYAN : MAGENTA,
                                                 .stroke_width = 4, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S, an_draw(span[k], &(an_anim_opts){ .duration = 1.2f }));
    }
    an_wait(S, 1.5f);

    // one arrow, A applied again and again, rescaled to unit length after each step
    const float R = 2.0f; // drawn at length 2 so the angle is easy to see
    an_vec2 v = { 0, 1 };
    an_obj *a = arrow("ch5/v", (an_vec2){ 0, R }, ORANGE, 6, false);
    pop_arrow(a);
    an_obj *readout = an_text(S, &(an_text_opts){ .parent = g, .text = nm("angle = %5.1f\xC2\xB0", deg_of(v)),
                                                  .pos = { 1880, 70 }, .size = 52, .font = an_font_mono(S),
                                                  .align = AN_ALIGN_RIGHT, .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL,
                                                  .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *fit = an_text(S, &(an_text_opts){ .parent = g, .text = "after each step: rescaled to fit", .pos = { 960, 1010 },
                                              .size = 46, .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                              .fill = CHARCOAL, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_fade_to(readout, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_play(S, an_fade_to(fit, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_wait(S, 1.0f);
    printf("chapter 5: angles %.1f", (double)deg_of(v));
    for (int k = 1; k <= 6; k++) {
        const an_vec2 w = mul(A, v);
        an_obj *aw = arrow(nm("ch5/av/%d", k), (an_vec2){ w.x * R, w.y * R }, pale(ORANGE, 0.5f), 5, true);
        an_play(S, an_draw(aw, &(an_anim_opts){ .duration = 0.6f }));
        an_wait(S, 0.5f);
        const float l = hypotf(w.x, w.y);
        const an_vec2 nv = { w.x / l, w.y / l };
        const float a0 = deg_of(v), a1 = deg_of(nv);
        an_play_async(S, an_fade_to(aw, 0, &(an_anim_opts){ .duration = 0.6f }));
        an_play_async(S, an_text_set(readout, nm("angle = %5.1f\xC2\xB0", a1), &(an_text_set_opts){ .anim = { .duration = 0.3f } }));
        arcs((an_obj *[]){ a }, &a0, &a1, 1, R, 1.0f, 12);
        v = nv;
        printf(" %.1f", (double)a1);
        an_wait(S, 0.8f);
    }
    printf("\n");
    an_wait(S, 1.5f);
    fade_all((an_obj *[]){ a, readout }, 2, 0, 0.6f);
    an_wait(S, 0.5f);

    // the whole ring, normalized after every step: pulled toward the magenta line
    make_ring("ch5", CHARCOAL);
    an_wait(S, 1.0f);
    float ang[RING], next[RING];
    for (int k = 0; k < RING; k++) ang[k] = (float)k * 15.0f;
    for (int step = 1; step <= 6; step++) {
        int still = 0;
        for (int k = 0; k < RING; k++) {
            const an_vec2 u = { cosf(ang[k] * PI / 180.0f), sinf(ang[k] * PI / 180.0f) }, w = mul(A, u);
            next[k] = deg_of(w);
            still += stays(A, u);
        }
        if (step == 1) printf("chapter 5: %d ring arrows stay put (on the magenta and cyan lines)\n", still);
        arcs(ring, ang, next, RING, 1.0f, 1.4f, 14);
        memcpy(ang, next, sizeof ang);
        an_wait(S, 0.8f);
    }
    memcpy(c5_left, ring, sizeof ring);
    c5_left[RING] = span[0];
    c5_left[RING + 1] = span[1];
    c5_left[RING + 2] = fit;
    prev_left = c5_left;
    prev_n = RING + 3;
    end_chapter(5);
}

// ---- chapter 6: recap ----------------------------------------------------------------------

static an_obj *say_at(an_obj *g, const char *text, an_vec2 at, an_color c, float size, an_align al) {
    an_obj *t = an_text(S, &(an_text_opts){ .parent = g, .text = text, .pos = at, .size = size, .align = al,
                                            .anchor = AN_ANCHOR_MIDDLE, .fill = c, .hidden = true, .riso = AN_OFF,
                                            .shadow = AN_OFF });
    an_play(S, an_fade_to(t, 1, &(an_anim_opts){ .duration = 0.6f }));
    return t;
}

static void chapter6(void) {
    an_obj *g = chapter_group(6);
    // the hook again, on the two lines that survive
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 1.5f }));
    fade_all(prev_left, prev_n, 0, 1.0f);
    an_wait(S, 1.0f);
    // the two lines that survive
    for (int k = 0; k < 2; k++) {
        an_obj *l = an_plane_line(P, (an_vec2){ 0, 0 }, k ? (an_vec2){ 1, -1 } : (an_vec2){ 1, 0 },
                                  &(an_line_opts){ .name = nm("ch6/span/%d", k), .z = -3, .stroke = k ? CYAN : MAGENTA,
                                                   .stroke_width = 4, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S, an_draw(l, &(an_anim_opts){ .duration = 1.2f }));
    }
    an_wait(S, 1.2f);
    make_ring("ch6", CHARCOAL);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 3 }));
    color_staying(A);
    an_wait(S, 0.5f);
    an_obj *t1 = say_at(g, "direction that survives", (an_vec2){ 1880, 495 }, MAGENTA, 42, AN_ALIGN_RIGHT);
    an_wait(S, 0.5f);
    // on the cyan line: the image of (1.6, -1.6) (A keeps the line), a little to its right
    const an_vec2 q = an_plane_point_at_cursor(P, (an_vec2){ 1.6f, -1.6f });
    an_obj *t2 = say_at(g, "direction that survives", (an_vec2){ q.x + 40, q.y }, CYAN, 42, AN_ALIGN_LEFT);
    an_wait(S, 0.8f);
    an_obj *t3 = say_at(g, "lambda = how much it stretches", (an_vec2){ 960, 1010 }, CHARCOAL, 52, AN_ALIGN_CENTER);
    an_wait(S, 5.0f);
    // the end card
    an_play_async(S, an_fade_to(P, 0, &(an_anim_opts){ .duration = 1.2f }));
    fade_all((an_obj *[]){ t1, t2, t3 }, 3, 0, 1.0f);
    an_play_async(S, an_scale_to(title, (an_vec2){ 1, 1 }, &(an_anim_opts){ .duration = 1.0f }));
    an_play(S, an_move_to(title, (an_vec2){ 960, 480 }, &(an_anim_opts){ .duration = 1.0f }));
    an_play(S, an_slap(title, &(an_verb_opts){ .impact_shake = 12 }));
    say_at(g, "the directions that don't turn", (an_vec2){ 960, 660 }, CHARCOAL, 64, AN_ALIGN_CENTER);
    end_chapter(6);
}


// ---- chapter 7: into 3D (its own scene) ------------------------------------------------------
//
// out/eigen/seven/SCRIPT.md, "3:00 Into 3D", timed to the recorded clip
// (out/eigen/seven/audio/clips/3.eigen.mp3, 54.5 s): t(s) waits until s seconds into
// it. Its own scene with an an_space: the plane story isn't ported. The 2D-to-3D
// seam is a cut: this scene starts on frame 0, a boil step (the boil steps at 12 Hz
// of frame time), and the outgoing 2D shot is cut just before one of its own boil
// steps, so the jump reads as part of the line boil.

static an_scene *S3;
static const float CH7_LEN = 54.5f;

static void t3(float sec) {
    const double now = an_scene_time(S3);
    if (now > sec + 1e-6) fprintf(stderr, "chapter 7: cue %.2f s already passed (at %.2f s)\n", (double)sec, now);
    else an_wait(S3, (float)(sec - now));
}

static an_obj *say3(const char *text, an_vec2 at, an_color c, float size, bool mono) {
    return an_text(S3, &(an_text_opts){ .text = text, .pos = at, .size = size, .font = mono ? an_font_mono(S3) : NULL,
                                        .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE, .fill = c, .hidden = true,
                                        .riso = AN_OFF, .shadow = AN_OFF });
}

// a label anchored at a 3D point of a space or frame, upright, mono
static an_obj *label3(an_obj *parent, const char *text, an_vec3 at, an_color c, float size) {
    an_obj *t = an_text(S3, &(an_text_opts){ .parent = parent, .text = text, .size = size, .font = an_font_mono(S3),
                                             .align = AN_ALIGN_LEFT, .anchor = AN_ANCHOR_MIDDLE, .fill = c, .hidden = true,
                                             .riso = AN_OFF, .shadow = AN_OFF });
    an_obj_set_pos3(t, at);
    return t;
}

static void fade3(an_obj *const *objs, int n, float to, float dur) {
    for (int k = 0; k < n; k++)
        if (objs[k]) an_play_async(S3, an_fade_to(objs[k], to, &(an_anim_opts){ .duration = dur }));
}

static an_mat3 mat3_mul(an_mat3 a, an_mat3 b) {
    an_mat3 m;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[3 * i + j] = a.m[3 * i] * b.m[j] + a.m[3 * i + 1] * b.m[3 + j] + a.m[3 * i + 2] * b.m[6 + j];
    return m;
}

static an_mat3 mat3_transpose(an_mat3 a) {
    an_mat3 m;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m.m[3 * i + j] = a.m[3 * j + i];
    return m;
}

static an_vec3 mat3_apply(an_mat3 m, an_vec3 v) {
    return (an_vec3){ m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z, m.m[3] * v.x + m.m[4] * v.y + m.m[5] * v.z,
                      m.m[6] * v.x + m.m[7] * v.y + m.m[8] * v.z };
}

static an_vec3 scale3(an_vec3 v, float k) { return (an_vec3){ v.x * k, v.y * k, v.z * k }; }

// a globe that spins about `axis` (unit), centered at c: the wire globe in a spinning
// frame, and its axis (an arrow up, a line down) in a still frame at the same place,
// drawn after the globe so it shows over it
typedef struct globe3 { an_obj *spin, *still, *ball, *up, *down; } globe3;

static globe3 make_globe(an_obj *sp, an_vec3 c, an_vec3 axis, float r) {
    globe3 g;
    g.spin = an_space_frame(sp, &(an_frame_opts){ .origin = c, .spin_axis = axis });
    g.ball = an_space_sphere(g.spin, &(an_sphere_opts){ .radius = r, .back = 0.18f, .stroke_width = 3, .hidden = true,
                                                        .riso = AN_OFF, .shadow = AN_OFF });
    g.still = an_space_frame(sp, &(an_frame_opts){ .origin = c });
    const an_vec3 tip = scale3(axis, r * 1.6f);
    g.up = an_space_vector(g.still, tip, &(an_arrow_opts){ .stroke = ORANGE, .stroke_width = 8, .undrawn = true,
                                                           .riso = AN_OFF, .shadow = AN_OFF });
    g.down = an_space_path(g.still, (an_vec3[]){ { 0, 0, 0 }, scale3(tip, -1) }, 2, false,
                           &(an_line_opts){ .stroke = ORANGE, .stroke_width = 6, .undrawn = true, .riso = AN_OFF,
                                            .shadow = AN_OFF });
    return g;
}

static void light_axis(globe3 *g) {
    an_play_async(S3, an_draw(g->down, &(an_anim_opts){ .duration = 0.5f }));
    an_play(S3, an_draw(g->up, &(an_anim_opts){ .duration = 0.7f }));
}

static void chapter7_build(void) {
    S3 = an_scene_create(&(an_scene_desc){ .width = 1920, .height = 1080, .fps = FPS, .seed = 7,
                                           .style = { .grain_mode = AN_GRAIN_STATIC } });
    // the space starts straight overhead: the floor grid looks like the plane's
    an_obj *sp = an_space(S3, &(an_space_opts){ .axes = true, .grid_extent = 7 });

    // "Everything so far was flat." two arrows on the floor, the plane's colors
    an_obj *flat[2] = {
        an_space_vector(sp, (an_vec3){ 2, 0, 0 }, &(an_arrow_opts){ .stroke = MAGENTA, .stroke_width = 7, .riso = AN_OFF, .shadow = AN_OFF }),
        an_space_vector(sp, (an_vec3){ 1.4f, -1.4f, 0 }, &(an_arrow_opts){ .stroke = CYAN, .stroke_width = 7, .riso = AN_OFF, .shadow = AN_OFF }),
    };
    // "Let's add a dimension." tilt back into space
    t3(3.2f);
    an_play_async(S3, an_space_target_to(sp, (an_vec3){ 0, 0, 1.4f }, &(an_anim_opts){ .duration = 1.8f }));
    an_play_async(S3, an_space_orbit_to(sp, (an_orbit){ -0.45f, 1.12f, 9 }, &(an_anim_opts){ .duration = 1.8f }));
    t3(4.4f);
    fade3(flat, 2, 0, 0.5f);
    // a slow drift for the rest of the chapter
    t3(5.0f);
    an_play_async(S3, an_space_orbit_to(sp, (an_orbit){ 0.55f, 1.0f, 9.5f }, &(an_anim_opts){ .duration = 38, .ease = AN_EASE_LINEAR }));

    // "Take a spinning globe, every point on it is moving,"
    const float tilt = 0.4084f; // 23.4 degrees
    const an_vec3 axis1 = { sinf(tilt), 0, cosf(tilt) }, c1 = { 0, 0, 1.6f };
    globe3 g1 = make_globe(sp, c1, axis1, 1.6f);
    an_play(S3, an_fade_to(g1.ball, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_play_async(S3, an_frame_spin_to(g1.spin, 2 * PI * 2.5f, &(an_anim_opts){ .duration = 13, .ease = AN_EASE_LINEAR }));
    // points on the surface, riding the spin (in the spinning frame's own axes: the
    // axis is its z): six around the middle latitudes
    t3(6.0f);
    an_obj *pts[6];
    for (int k = 0; k < 6; k++) {
        const float lon = (float)k * PI / 3.0f, lat = k % 2 ? 0.35f : -0.25f;
        pts[k] = an_space_dot(g1.spin, (an_vec3){ 1.6f * cosf(lat) * cosf(lon), 1.6f * cosf(lat) * sinf(lon), 1.6f * sinf(lat) },
                              &(an_circle_opts){ .radius = 9, .fill = MAGENTA, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    }
    fade3(pts, 6, 1, 0.3f);
    // "except the one on the axis." the poles: still
    t3(8.2f);
    an_obj *poles[2];
    for (int k = 0; k < 2; k++) {
        poles[k] = an_space_dot(g1.still, scale3(axis1, k ? -1.6f : 1.6f),
                                &(an_circle_opts){ .radius = 12, .fill = ORANGE, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S3, an_slap(poles[k], &(an_verb_opts){ .enter = AN_ON }));
    }
    // "The axis is an eigenvector ... with eigenvalue one." the axis lights up
    t3(9.6f);
    light_axis(&g1);
    t3(12.4f);
    an_obj *lam1 = label3(g1.still, "\xCE\xBB = 1", scale3(axis1, 1.6f * 1.75f), ORANGE, 48);
    an_play(S3, an_slap(lam1, &(an_verb_opts){ .enter = AN_ON }));
    // "The rotation doesn't turn it and doesn't stretch it either." a radius on the
    // equator turns with the globe; the axis doesn't
    t3(13.3f);
    an_obj *radius = an_space_vector(g1.spin, (an_vec3){ 1.6f, 0, 0 }, &(an_arrow_opts){ .stroke = CHARCOAL, .stroke_width = 6,
                                                                                    .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S3, an_draw(radius, &(an_anim_opts){ .duration = 0.5f }));

    // "And here's the cool part. Every rotation in 3D has one." another spin, another axis
    t3(17.2f);
    an_obj *g1_all[7] = { g1.ball, g1.up, g1.down, lam1, radius, poles[0], poles[1] };
    fade3(g1_all, 7, 0, 0.6f);
    fade3(pts, 6, 0, 0.6f);
    t3(18.3f);
    an_vec3 axis2 = { 0.8f, -0.45f, 0.4f };
    const float l2 = sqrtf(axis2.x * axis2.x + axis2.y * axis2.y + axis2.z * axis2.z);
    axis2 = scale3(axis2, 1.0f / l2);
    globe3 g2 = make_globe(sp, c1, axis2, 1.6f);
    an_play(S3, an_fade_to(g2.ball, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_play_async(S3, an_frame_spin_to(g2.spin, -2 * PI * 4.0f, &(an_anim_opts){ .duration = 25, .ease = AN_EASE_LINEAR }));
    // "However you spin a ball, some line through the middle stays put."
    t3(21.7f);
    light_axis(&g2);
    // "That's Euler's rotation theorem."
    t3(24.3f);
    an_obj *euler = say3("EULER'S ROTATION THEOREM", (an_vec2){ 960, 120 }, MAGENTA, 72, false);
    an_play(S3, an_slap(euler, &(an_verb_opts){ .enter = AN_ON, .impact_shake = 10 }));
    // "And it's an eigenvector hiding in plain sight."
    t3(26.6f);
    an_play(S3, an_stroke_width_to(g2.up, 13, &(an_anim_opts){ .duration = 0.4f }));
    an_play(S3, an_stroke_width_to(g2.up, 8, &(an_anim_opts){ .duration = 0.6f }));

    // "Remember how a flat rotation had no eigenvector at all? In 3D the spin keeps one
    // real direction, and the other two come out as complex numbers ... round and round."
    t3(29.1f);
    an_play_async(S3, an_fade_to(euler, 0, &(an_anim_opts){ .duration = 0.5f }));
    // a ring of arrows in the plane across the axis: they go round and round
    an_obj *ring3[8];
    for (int k = 0; k < 8; k++) {
        const float a = (float)k * PI / 4.0f;
        ring3[k] = an_space_vector(g2.spin, (an_vec3){ 1.25f * cosf(a), 1.25f * sinf(a), 0 },
                                   &(an_arrow_opts){ .stroke = CYAN, .stroke_width = 5, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S3, an_draw(ring3[k], &(an_anim_opts){ .duration = 0.4f }));
        an_wait(S3, 0.06f);
    }
    t3(33.9f);
    an_obj *lam_real = label3(g2.still, "\xCE\xBB = 1: real", scale3(axis2, 1.6f * 1.75f), ORANGE, 44);
    an_play(S3, an_slap(lam_real, &(an_verb_opts){ .enter = AN_ON }));
    t3(37.6f);
    an_obj *lam_cx = say3("the other two \xCE\xBB: complex", (an_vec2){ 960, 980 }, CYAN, 54, true);
    an_play(S3, an_fade_to(lam_cx, 1, &(an_anim_opts){ .duration = 0.4f }));
    t3(41.7f);
    an_obj *round = say3("round and round", (an_vec2){ 960, 1040 }, CHARCOAL, 40, false);
    an_play(S3, an_fade_to(round, 1, &(an_anim_opts){ .duration = 0.4f }));

    // "Now stretch instead of spin. A sphere becomes an egg, and the egg's three axes are
    // the three eigenvectors. Long axis, biggest eigenvalue."
    t3(43.0f);
    an_obj *g2_all[14] = { g2.ball, g2.up, g2.down, lam_real, lam_cx, round };
    for (int k = 0; k < 8; k++) g2_all[6 + k] = ring3[k];
    fade3(g2_all, 14, 0, 0.6f);
    // the egg: a symmetric stretch S = R diag(3, 2, 1) R^T, so its axes R e_i are its
    // eigenvectors: arrows along them stretch and never turn
    t3(43.8f);
    const an_mat3 R = mat3_mul(an_mat3_rotation((an_vec3){ 0, 0, 1 }, 0.5f), an_mat3_rotation((an_vec3){ 1, 0, 0 }, 0.4f));
    const an_mat3 Sm = mat3_mul(mat3_mul(R, an_mat3_scale((an_vec3){ 3, 2, 1 })), mat3_transpose(R));
    an_obj *egg = an_space_frame(sp, &(an_frame_opts){ .origin = c1 });
    an_obj *shell = an_space_sphere(egg, &(an_sphere_opts){ .radius = 1.0f, .meridians = 12, .parallels = 6, .back = 0.18f,
                                                            .stroke_width = 3, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    static const an_color cols[3] = { MAGENTA, CYAN, ORANGE };
    static const char *const by[3] = { "\xC3\x97" "3", "\xC3\x97" "2", "\xC3\x97" "1" };
    an_obj *arms[3], *tags[3];
    for (int k = 0; k < 3; k++) {
        const an_vec3 e = mat3_apply(R, (an_vec3){ k == 0, k == 1, k == 2 });
        arms[k] = an_space_vector(egg, e, &(an_arrow_opts){ .stroke = cols[k], .stroke_width = 7, .undrawn = true,
                                                            .riso = AN_OFF, .shadow = AN_OFF });
        tags[k] = label3(egg, by[k], scale3(e, 1.3f), cols[k], 46);
    }
    an_play(S3, an_fade_to(shell, 1, &(an_anim_opts){ .duration = 0.5f }));
    for (int k = 0; k < 3; k++) an_play(S3, an_draw(arms[k], &(an_anim_opts){ .duration = 0.3f }));
    t3(45.8f);
    an_play(S3, an_space_matrix_to(egg, Sm, &(an_anim_opts){ .duration = 2.2f }));
    {   // computed, not typed: S e_i = lambda_i e_i
        for (int k = 0; k < 3; k++) {
            const an_vec3 e = mat3_apply(R, (an_vec3){ k == 0, k == 1, k == 2 }), se = mat3_apply(Sm, e);
            const float lam = se.x * e.x + se.y * e.y + se.z * e.z;
            const an_vec3 off = { se.x - lam * e.x, se.y - lam * e.y, se.z - lam * e.z };
            printf("chapter 7: S e%d = %.4f e%d (off the line by %.1e)\n", k + 1, (double)lam, k + 1,
                   (double)sqrtf(off.x * off.x + off.y * off.y + off.z * off.z));
        }
    }
    t3(49.3f);
    for (int k = 0; k < 3; k++) {
        an_play_async(S3, an_slap(tags[k], &(an_verb_opts){ .enter = AN_ON }));
        an_wait(S3, 0.25f);
    }
    t3(51.5f);
    an_play(S3, an_stroke_width_to(arms[0], 12, &(an_anim_opts){ .duration = 0.4f }));
    t3(52.6f);
    an_obj *big = say3("long axis = biggest \xCE\xBB", (an_vec2){ 960, 990 }, MAGENTA, 54, true);
    an_play(S3, an_slap(big, &(an_verb_opts){ .enter = AN_ON }));
    t3(CH7_LEN);
}

// ---- main -----------------------------------------------------------------------------------

int main(int argc, char **argv) {
    int chapter = -1, every = 30;
    float from = -1, to = -1; // chapter 7: seconds into it (stills around a moment)
    const char *dir = NULL, *mp4 = NULL;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (strcmp(argv[k], "--chapter") == 0) chapter = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--from") == 0) from = (float)atof(argv[k + 1]);
        else if (strcmp(argv[k], "--to") == 0) to = (float)atof(argv[k + 1]);
        else if (strcmp(argv[k], "--frames") == 0) dir = argv[k + 1];
        else if (strcmp(argv[k], "--every") == 0) every = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--export") == 0) mp4 = argv[k + 1];
    }
    if (chapter == 7) { // the 3D section: its own scene (see chapter7_build)
        chapter7_build();
        printf("chapter 7: %.2f s, frames 0..%lld\n", an_scene_time(S3), (long long)an_scene_length(S3));
        int rc7 = 0;
        if (dir) {
            const int64_t first = from >= 0 ? (int64_t)llroundf(from * FPS) : 0;
            const int64_t last = to >= 0 ? (int64_t)llroundf(to * FPS) : an_scene_length(S3);
            const int64_t n = an_scene_save_frames(S3, &(an_frames_desc){ .dir = dir, .every = every, .first = first,
                                                                          .last = last, .prefix = "ch7" });
            if (n >= 0) printf("wrote %lld PNG frames (%lld..%lld, every %d) to %s\n", (long long)n, (long long)first,
                               (long long)last, every, dir);
            rc7 = n >= 0 ? 0 : 1;
        }
        if (mp4 && rc7 == 0) rc7 = an_scene_export(S3, &(an_export_desc){ .path = mp4 }) > 0 ? 0 : 1;
        if (!dir && !mp4) an_scene_preview(S3);
        an_scene_destroy(S3);
        return rc7;
    }
    S = an_scene_create(&(an_scene_desc){ .width = 1920, .height = 1080, .fps = FPS, .seed = 12,
                                          .style = { .grain_mode = AN_GRAIN_STATIC } }); // shadows only where asked (the title):
    // a callout's label doesn't take the callout's .shadow, so the scene default is off
    if (!S) return 1;
    P = an_plane(S, &(an_plane_opts){ .name = "plane" }); // 1 unit = 100 px at 1080p, origin at the center

    void (*const chapters[NCH])(void) = { chapter0, chapter1, chapter2, chapter3, chapter4, chapter5, chapter6 };
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
