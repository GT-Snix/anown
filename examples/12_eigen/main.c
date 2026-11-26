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

enum { NCH = 7, RING = 24 };
static int FPS = 30; // --fps; every chapter is timed in seconds
// chapter durations, seconds (retime to the voiceover here)
// chapter lengths, seconds (ch7: CH7_LEN). Chapter 6 is retired (the recap is done in the editor).
static const float DUR[NCH] = { 14, 9, 8, 32, 21.5f, 17, 30 };
static const bool BUILT[NCH] = { true, true, true, true, true, true, false };

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
static an_obj *title; // chapter 6 (retired) brings the title back; nothing makes it now
static an_obj *c3_lines[2]; // chapter 3's magenta and cyan lines

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

// ---- the 7-minute cut: self-contained chapters ---------------------------------------------
//
// Each chapter is its own scene and starts from the state it declares (the plane's
// matrix and what's on it), so a chapter renders without the ones before it and the
// seams between chapters are cuts. Cues are seconds into the chapter, placed on the
// recorded narration (out/eigen/seven/audio): chapters 1, 2, 0, 3 and 4 are the
// "fast version" (clip 1, in that order), chapter 5 sits in "eigen-coordinates"
// (clip 4, 14.3 s in). Chapter 6 (the recap) is retired: done in the editor.

// the 24 arrows, drawn on at a given pace
static void make_ring_paced(const char *prefix, an_color c, float gap, float dur) {
    for (int k = 0; k < RING; k++) {
        const float a = (float)k * 15.0f * PI / 180.0f;
        ring[k] = arrow(nm("%s/ring/%d", prefix, k * 15), (an_vec2){ cosf(a), sinf(a) }, c, 5, true);
        an_play_async(S, an_draw(ring[k], &(an_anim_opts){ .duration = dur }));
        an_wait(S, gap);
    }
}

static an_vec2 unit_at(float deg) { return (an_vec2){ cosf(deg * PI / 180.0f), sinf(deg * PI / 180.0f) }; }
static float deg_of(an_vec2 v) { return atan2f(v.y, v.x) * 180.0f / PI; }

// the angle between v and A v, in degrees (from the cross and dot products)
static float angle_v_av(an_vec2 v) {
    const an_vec2 w = mul(A, v);
    return fabsf(atan2f(v.x * w.y - v.y * w.x, v.x * w.x + v.y * w.y)) * 180.0f / PI;
}

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

static an_obj *span(const char *name, an_vec2 dir, an_color c, bool undrawn) {
    return an_plane_line(P, (an_vec2){ 0, 0 }, dir, &(an_line_opts){ .name = name, .z = -3, .stroke = c, .stroke_width = 4,
                                                                     .undrawn = undrawn, .riso = AN_OFF, .shadow = AN_OFF });
}

// ---- chapter 1: vectors and directions (clip 1, 0.0 - 9.0 s) ------------------------------------
// Declared state: the plane at I, the grid, nothing on it.
static void chapter1(void) {
    chapter_group(1);
    // "A vector is an arrow from the origin."
    hold_until(1, 1.7f);
    an_obj *v = arrow("ch1/v", (an_vec2){ 2, 1 }, CHARCOAL, 6, false);
    pop_arrow(v);
    an_obj *vl = plane_label("ch1/v/label", "v", (an_vec2){ 2.15f, 1.35f }, CHARCOAL, 44);
    an_obj_set_opacity(vl, 0);
    an_play(S, an_fade_to(vl, 1, &(an_anim_opts){ .duration = 0.4f }));
    // "Every arrow points along a line, its direction"
    hold_until(1, 4.7f);
    an_obj *line = an_plane_line(P, (an_vec2){ 0, 0 }, (an_vec2){ 2, 1 }, &(an_line_opts){ .name = "ch1/v/line", .z = -4,
                                  .stroke = pale(CHARCOAL, 0.55f), .stroke_width = 2, .hidden = true, .shadow = AN_OFF });
    an_play(S, an_fade_to(line, 1, &(an_anim_opts){ .duration = 1.2f }));
    end_chapter(1);
}

// ---- chapter 2: a transformation moves everything (clip 1, 9.9 - 17.9 s) ------------------------
// Declared state: the plane at I, the grid.
static void chapter2(void) {
    chapter_group(2);
    // "A transformation moves every arrow at once."
    hold_until(2, 0.2f);
    an_obj *e1 = arrow("ch2/e1", (an_vec2){ 1, 0 }, CHARCOAL, 6, true), *e2 = arrow("ch2/e2", (an_vec2){ 0, 1 }, CHARCOAL, 6, true);
    an_play_async(S, an_draw(e1, &(an_anim_opts){ .duration = 0.6f }));
    an_play(S, an_draw(e2, &(an_anim_opts){ .duration = 0.6f }));
    hold_until(2, 1.0f);
    // "Grid lines stay straight, parallel and evenly spaced, and the origin stays put."
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 2.5f }));
    end_chapter(2);
}

// ---- chapter 0: the hook ring (clip 1, 18.0 - 32.0 s) ---------------------------------------------
// Declared state: the plane at I, the grid. (The title moved to the cold open.)
static void chapter0(void) {
    chapter_group(0);
    // "Now take any transformation of the plane."
    hold_until(0, 0.7f);
    make_ring("ch0", CHARCOAL);
    // "Almost every arrow gets knocked off the line it started on."
    hold_until(0, 3.7f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 3 }));
    const int n = ring_count_staying(A);
    printf("chapter 0: %d of %d arrows stay on their line\n", n, RING);
    // "But a few special arrows don't turn at all, they just stretch."
    hold_until(0, 7.6f);
    color_staying(A);
    end_chapter(0);
}

// ---- chapter 3: eigenvectors and eigenvalues (clip 1, 32.0 - 64.0 s) -----------------------------
// Declared state: the plane at I, the grid.
static void chapter3(void) {
    an_obj *g = chapter_group(3);
    // "Spin an arrow around and compare it with what the transformation does to it."
    // The probe sweeps at 27 degrees a second (3x the old 9), from 30 degrees: it lines up
    // at 135 on "here they are agreeing", holds 1 s, and at 180 on "those two lines".
    const float deg0 = 30.0f, step = 2.5f, rate = 27.0f;
    an_obj *probe = arrow("ch3/probe", unit_at(deg0), ORANGE, 6, false);
    an_obj *av = arrow("ch3/av", mul(A, unit_at(deg0)), pale(ORANGE, 0.5f), 5, false);
    an_obj *avl = plane_label("ch3/av/label", "Av", (an_vec2){ mul(A, unit_at(deg0)).x * 1.12f, mul(A, unit_at(deg0)).y * 1.12f + 0.25f },
                              pale(ORANGE, 0.3f), 40);
    an_obj *readout = an_text(S, &(an_text_opts){ .parent = g, .text = nm("angle(v, Av) = %5.1f\xC2\xB0", angle_v_av(unit_at(deg0))),
                                                  .pos = { 1880, 70 }, .size = 46, .font = an_font_mono(S),
                                                  .align = AN_ALIGN_RIGHT, .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL,
                                                  .riso = AN_OFF, .shadow = AN_OFF, .hidden = true });
    pop_arrow(probe);
    an_play_async(S, an_fade_to(readout, 1, &(an_anim_opts){ .duration = 0.4f }));
    pop_arrow(av);
    // "Most of the time they disagree about direction."
    hold_until(3, 4.7f);
    an_obj *rings[2] = { NULL, NULL };
    int hits = 0;
    const float dt = step / rate;
    const an_anim_opts lin = { .duration = dt, .ease = AN_EASE_LINEAR };
    for (int k = 1; hits < 2 && k <= 80; k++) {
        const float deg = deg0 + step * (float)k;
        const an_vec2 v = unit_at(deg), w = mul(A, v);
        an_play_async(S, an_move_to(probe, v, &lin));
        an_play_async(S, an_move_to(av, w, &lin));
        an_play_async(S, an_move_to(avl, (an_vec2){ w.x * 1.12f, w.y * 1.12f + 0.25f }, &lin));
        an_play_async(S, an_text_set(readout, nm("angle(v, Av) = %5.1f\xC2\xB0", angle_v_av(v)),
                                     &(an_text_set_opts){ .anim = { .duration = 0.05f } }));
        an_wait(S, dt);
        if (!stays(A, v)) continue;
        // a hit: a ring on the arrow, the whole line in its color, and a 1 s hold
        const an_color c = fabsf(v.y) < 1e-6f ? MAGENTA : CYAN;
        printf("chapter 3: v at %.1f degrees stays on its line (A v = %.4g v)\n", deg, hypotf(w.x, w.y));
        rings[hits] = an_plane_dot(P, v, &(an_circle_opts){ .name = nm("ch3/ring/%d", hits), .radius = 30, .no_fill = true,
                                                             .stroke = c, .stroke_width = 6, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S, an_slap(rings[hits], &(an_verb_opts){ .enter = AN_ON }));
        c3_lines[hits] = span(nm("ch3/span/%d", hits), v, c, true);
        an_play_async(S, an_draw(c3_lines[hits], &(an_anim_opts){ .duration = 0.8f }));
        an_wait(S, 1.0f);
        hits++;
    }
    hold_until(3, 12.5f);
    fade_all((an_obj *[]){ probe, av, avl, readout, rings[0], rings[1] }, 6, 0, 0.5f);
    // "Along each line, all it does is stretch."
    hold_until(3, 14.0f);
    static const an_vec2 on[8] = { { 1, 0 }, { 2, 0 }, { -1, 0 }, { -2, 0 }, { 1, -1 }, { 2, -2 }, { -1, 1 }, { -2, 2 } };
    an_obj *dots[8];
    for (int k = 0; k < 8; k++)
        dots[k] = an_plane_dot(P, on[k], &(an_circle_opts){ .name = nm("ch3/dot/%d", k), .radius = 9, .hidden = true,
                                                             .fill = k < 4 ? MAGENTA : CYAN, .riso = AN_OFF, .shadow = AN_OFF });
    fade_all(dots, 8, 1, 0.4f);
    hold_until(3, 15.3f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 2.5f }));
    // "That stretch factor is the eigenvalue. Three along this one, two along that one."
    hold_until(3, 21.1f);
    for (int k = 0; k < 2; k++) {
        const an_vec2 u = k ? (an_vec2){ 1, -1 } : (an_vec2){ 1, 0 };
        const an_vec2 q = an_plane_point_at_cursor(P, u), o = an_plane_point_at_cursor(P, (an_vec2){ 0, 0 });
        const float lam = hypotf(mul(A, u).x, mul(A, u).y) / hypotf(u.x, u.y);
        an_vec2 n = { -(q.y - o.y), q.x - o.x };
        const float nl = hypotf(n.x, n.y);
        n = (an_vec2){ n.x / nl, n.y / nl };
        an_obj *tick = an_line(S, &(an_line_opts){ .parent = g, .from = { q.x - n.x * 22, q.y - n.y * 22 },
                                                   .to = { q.x + n.x * 22, q.y + n.y * 22 }, .stroke = k ? CYAN : MAGENTA,
                                                   .stroke_width = 5, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_obj *lbl = an_text(S, &(an_text_opts){ .parent = g, .text = nm("\xC3\x97%g", lam), .pos = { q.x - n.x * 60, q.y - n.y * 60 },
                                                   .size = 52, .font = an_font_mono(S), .align = AN_ALIGN_CENTER,
                                                   .anchor = AN_ANCHOR_MIDDLE, .fill = k ? CYAN : MAGENTA, .hidden = true,
                                                   .riso = AN_OFF, .shadow = AN_OFF });
        an_play(S, an_draw(tick, &(an_anim_opts){ .duration = 0.4f }));
        an_play(S, an_fade_to(lbl, 1, &(an_anim_opts){ .duration = 0.4f }));
        if (k == 0) hold_until(3, 22.9f); // "two along that one"
    }
    // "A v equals lambda v."
    hold_until(3, 24.6f);
    an_obj *eq = an_text(S, &(an_text_opts){ .parent = g, .text = "A v = \xCE\xBB v", .pos = { 960, 175 }, .size = 120,
                                             .font = an_font_mono(S), .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                             .fill = CHARCOAL, .riso = AN_OFF, .shadow = AN_OFF });
    for (int k = 0; k < an_text_length(eq); k++) {
        an_play_async(S, an_slap(an_text_glyph(eq, k), &(an_verb_opts){ .enter = AN_ON }));
        an_wait(S, 0.08f);
    }
    // "The arrow you put in, times a plain number."
    hold_until(3, 27.4f);
    an_obj *l1 = an_text(S, &(an_text_opts){ .parent = g, .text = nm("\xCE\xBB\xE2\x82\x81 = %g", 3.0), .pos = { 120, 880 },
                                             .size = 64, .font = an_font_mono(S), .anchor = AN_ANCHOR_MIDDLE, .fill = MAGENTA,
                                             .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *l2 = an_text(S, &(an_text_opts){ .parent = g, .text = nm("\xCE\xBB\xE2\x82\x82 = %g", 2.0), .pos = { 120, 970 },
                                             .size = 64, .font = an_font_mono(S), .anchor = AN_ANCHOR_MIDDLE, .fill = CYAN,
                                             .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S, an_fade_to(l1, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_play(S, an_fade_to(l2, 1, &(an_anim_opts){ .duration = 0.4f }));
    end_chapter(3);
}

// ---- chapter 4: edge cases and "eigen = own" (clip 1, 63.2 - 84.7 s) -------------------------------
// Declared state: the plane at I, the grid. About 4 s per case, then a 9.5 s end card.
static an_obj **prev_left; // what the chapter before 6 left (chapter 6 is retired)
static int prev_n;

static void chapter4(void) {
    an_obj *g = chapter_group(4);
    // a) "Lambda can be negative. The arrow flips but stays on its line."
    const an_mat2 B = { -1, 0, 0, 2 };
    an_obj *x = arrow("ch4/a/x", (an_vec2){ 1.5f, 0 }, CHARCOAL, 6, true), *y = arrow("ch4/a/y", (an_vec2){ 0, 1.5f }, CHARCOAL, 6, true);
    an_play_async(S, an_draw(x, &(an_anim_opts){ .duration = 0.4f }));
    an_play(S, an_draw(y, &(an_anim_opts){ .duration = 0.4f }));
    recolor(x, ORANGE, 0.3f);
    hold_until(4, 0.9f);
    an_play(S, an_plane_matrix_to(P, B, &(an_anim_opts){ .duration = 1.4f }));
    printf("chapter 4a: B keeps (1, 0) on its line: %s; lambda = %g\n", stays(B, (an_vec2){ 1, 0 }) ? "yes" : "no",
           (double)mul(B, (an_vec2){ 1, 0 }).x);
    const an_vec2 tip = an_plane_point_at_cursor(P, (an_vec2){ 1.5f, 0 });
    an_obj *ca = note(g, "lambda = -1: flipped, same line", (an_vec2){ 640, 330 }, (an_vec2){ tip.x - 10, tip.y - 14 }, -0.3f);
    hold_until(4, 3.6f);
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 0.4f }));
    fade_all((an_obj *[]){ x, y, ca }, 3, 0, 0.4f);
    // b) "It can be zero as well. The whole direction gets crushed flat."
    hold_until(4, 4.1f);
    const an_mat2 C = { 1, 1, 1, 1 };
    an_obj *u = arrow("ch4/b/u", (an_vec2){ 1, 1 }, CHARCOAL, 6, true), *w = arrow("ch4/b/w", (an_vec2){ 1, -1 }, CHARCOAL, 6, true);
    an_play_async(S, an_draw(u, &(an_anim_opts){ .duration = 0.4f }));
    an_play(S, an_draw(w, &(an_anim_opts){ .duration = 0.4f }));
    recolor(w, ORANGE, 0.3f);
    hold_until(4, 5.0f);
    an_play(S, an_plane_matrix_to(P, C, &(an_anim_opts){ .duration = 1.8f }));
    const an_vec2 cw = mul(C, (an_vec2){ 1, -1 });
    printf("chapter 4b: C (1, -1) = (%g, %g)\n", (double)cw.x, (double)cw.y);
    const an_vec2 o = an_plane_point_at_cursor(P, (an_vec2){ 0, 0 });
    an_obj *cb = note(g, "lambda = 0: crushed flat", (an_vec2){ 1300, 860 }, (an_vec2){ o.x + 12, o.y + 14 }, 0.3f);
    hold_until(4, 8.2f);
    an_play_async(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 0.4f }));
    fade_all((an_obj *[]){ u, w, cb }, 3, 0, 0.4f);
    // c) "And a rotation has none at all. Every arrow turns." a quarter turn in three
    // keyframes, so lengths don't shrink much
    hold_until(4, 8.6f);
    make_ring_paced("ch4", CHARCOAL, 0.02f, 0.3f);
    hold_until(4, 9.6f);
    for (int k = 1; k <= 3; k++)
        an_play(S, an_plane_matrix_to(P, rotation(30.0f * (float)k), &(an_anim_opts){ .duration = 0.7f, .ease = AN_EASE_LINEAR }));
    printf("chapter 4c: a quarter turn keeps %d of %d arrows on their line\n", ring_count_staying(rotation(90)), RING);
    // the end card (9.5 s): "Eigen is German for own. These are the transformation's own
    // directions. That's the short version. Now let's earn it."
    hold_until(4, 12.0f);
    an_play_async(S, an_fade_to(P, 0, &(an_anim_opts){ .duration = 0.4f }));
    hold_until(4, 12.2f);
    an_obj *own = an_text(S, &(an_text_opts){ .parent = g, .text = "eigen = own", .pos = { 960, 470 }, .size = 150,
                                              .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE, .fill = MAGENTA,
                                              .stroke = CHARCOAL, .stroke_width = 7, .shadow = AN_OFF, .riso = AN_OFF });
    for (int k = 0; k < an_text_length(own); k++) {
        an_play_async(S, an_slap(an_text_glyph(own, k), &(an_verb_opts){ .enter = AN_ON,
                                                                         .impact_shake = k == an_text_length(own) - 1 ? 14.0f : 0.0f }));
        an_wait(S, 0.06f);
    }
    hold_until(4, 14.2f);
    an_obj *eq = an_text(S, &(an_text_opts){ .parent = g, .text = "A v = \xCE\xBB v", .pos = { 960, 680 }, .size = 96,
                                             .font = an_font_mono(S), .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                             .fill = CHARCOAL, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S, an_fade_to(eq, 1, &(an_anim_opts){ .duration = 0.5f }));
    end_chapter(4);
}

// ---- chapter 5: the single arrow, again and again (clip 4, 14.3 - 31.3 s) ---------------------------
// Declared state: the plane at I, the grid, the magenta and cyan lines already drawn.
static void chapter5(void) {
    an_obj *g = chapter_group(5);
    span("ch5/span/0", (an_vec2){ 1, 0 }, MAGENTA, false);
    span("ch5/span/1", (an_vec2){ 1, -1 }, CYAN, false);
    // "Remember that arrow that kept swinging toward the magenta line? This is why."
    an_obj *rem = an_text(S, &(an_text_opts){ .parent = g, .text = "remember this?", .pos = { 60, 70 }, .size = 44,
                                              .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL, .hidden = true,
                                              .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_fade_to(rem, 1, &(an_anim_opts){ .duration = 0.4f }));
    const float R = 2.0f; // drawn at length 2 so the angle is easy to see
    an_vec2 v = { 0, 1 };
    hold_until(5, 0.4f);
    an_obj *a = arrow("ch5/v", (an_vec2){ 0, R }, ORANGE, 6, false);
    pop_arrow(a);
    an_obj *readout = an_text(S, &(an_text_opts){ .parent = g, .text = nm("angle = %5.1f\xC2\xB0", deg_of(v)),
                                                  .pos = { 1880, 70 }, .size = 52, .font = an_font_mono(S),
                                                  .align = AN_ALIGN_RIGHT, .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL,
                                                  .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play(S, an_fade_to(readout, 1, &(an_anim_opts){ .duration = 0.4f }));
    // A applied again and again, rescaled to unit length after each step: 6 steps of 2 s
    hold_until(5, 3.0f);
    printf("chapter 5: angles %.1f", (double)deg_of(v));
    for (int k = 1; k <= 6; k++) {
        const an_vec2 w = mul(A, v);
        an_obj *aw = arrow(nm("ch5/av/%d", k), (an_vec2){ w.x * R, w.y * R }, pale(ORANGE, 0.5f), 5, true);
        an_play(S, an_draw(aw, &(an_anim_opts){ .duration = 0.5f }));
        an_wait(S, 0.3f);
        const float l = hypotf(w.x, w.y);
        const an_vec2 nv = { w.x / l, w.y / l };
        const float a0 = deg_of(v), a1 = deg_of(nv);
        an_play_async(S, an_fade_to(aw, 0, &(an_anim_opts){ .duration = 0.5f }));
        an_play_async(S, an_text_set(readout, nm("angle = %5.1f\xC2\xB0", a1), &(an_text_set_opts){ .anim = { .duration = 0.3f } }));
        arcs((an_obj *[]){ a }, &a0, &a1, 1, R, 0.8f, 10);
        v = nv;
        printf(" %.1f", (double)a1);
        hold_until(5, 3.0f + 2.0f * (float)k);
    }
    printf("\n");
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

// captions sit at fixed screen spots in the bottom-left corner, clear of the globe
// and the egg (which stay around the middle): line 0 above line 1
static an_obj *caption3(const char *text, int line, an_color c, float size, bool mono) {
    return an_text(S3, &(an_text_opts){ .text = text, .pos = { 90, line ? 1030.0f : 975.0f }, .size = size,
                                        .font = mono ? an_font_mono(S3) : NULL, .align = AN_ALIGN_LEFT,
                                        .anchor = AN_ANCHOR_MIDDLE, .fill = c, .hidden = true, .riso = AN_OFF,
                                        .shadow = AN_OFF });
}

// a label anchored at a 3D point of a space or frame, upright, mono
static an_obj *label3(an_obj *parent, const char *text, an_vec3 at, an_color c, float size, an_align al) {
    an_obj *t = an_text(S3, &(an_text_opts){ .parent = parent, .text = text, .size = size, .font = an_font_mono(S3),
                                             .align = al, .anchor = AN_ANCHOR_MIDDLE, .fill = c, .hidden = true,
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
    an_obj *sp = an_space(S3, &(an_space_opts){ .grid_extent = 7 });
    // the axes as the space draws them, but ours, so the z axis can leave for the egg
    an_obj *axes[3];
    for (int k = 0; k < 3; k++)
        axes[k] = an_space_line(sp, (an_vec3){ 0, 0, 0 }, (an_vec3){ k == 0, k == 1, k == 2 },
                                &(an_line_opts){ .z = -5, .stroke_width = 4, .stroke = CHARCOAL, .shadow = AN_OFF });

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
    an_obj *lam1 = label3(g1.still, "\xCE\xBB = 1", scale3(axis1, 1.6f * 1.75f), ORANGE, 48, AN_ALIGN_LEFT);
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
    an_obj *lam_real = label3(g2.still, "\xCE\xBB = 1: real", scale3(axis2, 1.6f * 1.75f), ORANGE, 44, AN_ALIGN_LEFT);
    an_play(S3, an_slap(lam_real, &(an_verb_opts){ .enter = AN_ON }));
    t3(37.6f);
    an_obj *lam_cx = caption3("the other two \xCE\xBB: complex", 0, CYAN, 50, true);
    an_play(S3, an_fade_to(lam_cx, 1, &(an_anim_opts){ .duration = 0.4f }));
    t3(41.7f);
    an_obj *round = caption3("round and round", 1, CHARCOAL, 40, false);
    an_play(S3, an_fade_to(round, 1, &(an_anim_opts){ .duration = 0.4f }));

    // "Now stretch instead of spin. A sphere becomes an egg, and the egg's three axes are
    // the three eigenvectors. Long axis, biggest eigenvalue."
    t3(43.0f);
    an_obj *g2_all[14] = { g2.ball, g2.up, g2.down, lam_real, lam_cx, round };
    for (int k = 0; k < 8; k++) g2_all[6 + k] = ring3[k];
    fade3(g2_all, 14, 0, 0.6f);
    an_play_async(S3, an_fade_to(axes[2], 0, &(an_anim_opts){ .duration = 0.6f })); // no z axis through the egg
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
    // the x3 / x2 / x1 labels: beside each stretched arrow, near its tip, offset
    // perpendicular to it on screen. The camera holds still here (its drift ended at
    // 43 s), so the offset is a 3D direction across both the arrow and the line of
    // sight: arrow x view. In the egg's frame that point is S^-1 of it.
    {
        const an_orbit ob = an_space_orbit_at_cursor(sp);
        const an_vec3 view = { -sinf(ob.tilt) * sinf(ob.yaw), sinf(ob.tilt) * cosf(ob.yaw), -cosf(ob.tilt) };
        const an_mat3 Sinv = mat3_mul(mat3_mul(R, an_mat3_scale((an_vec3){ 1.0f / 3, 1.0f / 2, 1 })), mat3_transpose(R));
        static const float lam[3] = { 3, 2, 1 };
        for (int k = 0; k < 3; k++) {
            const an_vec3 e = mat3_apply(R, (an_vec3){ k == 0, k == 1, k == 2 });
            an_vec3 n = { e.y * view.z - e.z * view.y, e.z * view.x - e.x * view.z, e.x * view.y - e.y * view.x };
            const float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
            n = scale3(n, 0.75f / nl);
            const an_vec3 at = { e.x * lam[k] * 0.8f + n.x, e.y * lam[k] * 0.8f + n.y, e.z * lam[k] * 0.8f + n.z }; // world, egg-centered
            tags[k] = label3(egg, by[k], mat3_apply(Sinv, at), cols[k], 46, AN_ALIGN_CENTER);
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
    an_obj *big = caption3("long axis = biggest \xCE\xBB", 0, MAGENTA, 50, true);
    an_play(S3, an_slap(big, &(an_verb_opts){ .enter = AN_ON }));
    t3(CH7_LEN);
}

// The first and last frames of a chapter as PNGs (DIR/chN_first.png, chN_last.png,
// through the frame dumper), for checking the seams between chapters.
static int save_seams(an_scene *s, const char *dir, int ch, int64_t first, int64_t last) {
    const int64_t f[2] = { first, last };
    for (int k = 0; k < 2; k++) {
        // one frame: from f[k], stepping past the end (last = 0 would mean the whole timeline)
        const int64_t n = an_scene_save_frames(s, &(an_frames_desc){ .dir = dir, .first = f[k], .last = f[k] + 1,
                                                                     .every = 1000000, .prefix = nm("ch%d_%s", ch, k ? "last" : "first") });
        if (n != 1) return 1;
    }
    return 0;
}

// ---- main -----------------------------------------------------------------------------------

int main(int argc, char **argv) {
    int chapter = -1, every = 30;
    float from = -1, to = -1; // chapter 7: seconds into it (stills around a moment)
    const char *dir = NULL, *mp4 = NULL, *seams = NULL;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (strcmp(argv[k], "--chapter") == 0) chapter = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--from") == 0) from = (float)atof(argv[k + 1]);
        else if (strcmp(argv[k], "--fps") == 0) FPS = atoi(argv[k + 1]) > 0 ? atoi(argv[k + 1]) : 30;
        else if (strcmp(argv[k], "--seams") == 0) seams = argv[k + 1];
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
        if (seams && rc7 == 0) rc7 = save_seams(S3, seams, 7, 0, an_scene_length(S3));
        if (mp4 && rc7 == 0) rc7 = an_scene_export(S3, &(an_export_desc){ .path = mp4 }) > 0 ? 0 : 1;
        printf("chapter 7 range: frames 0..%lld\n", (long long)an_scene_length(S3));
        if (!dir && !mp4) an_scene_preview(S3);
        an_scene_destroy(S3);
        return rc7;
    }
    void (*const chapters[NCH])(void) = { chapter0, chapter1, chapter2, chapter3, chapter4, chapter5, chapter6 };
    if (chapter < 0 || chapter >= NCH || !BUILT[chapter]) {
        fprintf(stderr, chapter == 6 ? "chapter 6 is retired (the recap is done in the editor)\n"
                                     : "pick a chapter: --chapter 0..5 or 7\n");
        return 1;
    }
    // each chapter is its own scene, starting from the state it declares
    S = an_scene_create(&(an_scene_desc){ .width = 1920, .height = 1080, .fps = FPS, .seed = 12,
                                          .style = { .grain_mode = AN_GRAIN_STATIC } });
    if (!S) return 1;
    P = an_plane(S, &(an_plane_opts){ .name = "plane" }); // 1 unit = 100 px at 1080p, origin at the center
    ch_start[chapter] = 0;
    chapters[chapter]();
    printf("chapter %d: %.2f s (DUR %.2f), frames 0..%lld\n", chapter, ch_end[chapter], (double)DUR[chapter],
           (long long)an_scene_length(S));

    int rc = 0;
    const int64_t L = an_scene_length(S);
    if (dir) {
        const int64_t n = an_scene_save_frames(S, &(an_frames_desc){ .dir = dir, .every = every, .prefix = nm("ch%d", chapter) });
        if (n >= 0) printf("wrote %lld PNG frames (0..%lld, every %d) to %s\n", (long long)n, (long long)L, every, dir);
        rc = n >= 0 ? 0 : 1;
    }
    printf("chapter %d range: frames 0..%lld\n", chapter, (long long)L);
    if (seams && rc == 0) rc = save_seams(S, seams, chapter, 0, L);
    if (mp4 && rc == 0) rc = an_scene_export(S, &(an_export_desc){ .path = mp4 }) > 0 ? 0 : 1;
    if (!dir && !mp4) an_scene_preview(S);
    an_scene_destroy(S);
    return rc;
}
