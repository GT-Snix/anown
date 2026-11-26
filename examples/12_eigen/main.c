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
// The fast version starts each chapter on its own words in clip 1 (1 at 0 s, 2 at 9.9, 0 at 18.0,
// 3 at 32.0, 4 at 63.2; the clip is 84.53 s), so the cut lands on the narration.
static const float DUR[NCH] = { 14, 9.9f, 8.1f, 31.2f, 21.332653f, 17, 30 };
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

// ---- the 7-minute video: the new sections (chapters 8..19) ------------------------------------
//
// out/eigen/seven/SCRIPT.md, cued to the recorded clips (out/eigen/seven/audio).
// Every section is its own scene; scripts/assemble-eigen.sh cuts them together with
// the narration. The look borrows the comic-panel energy the script asks for, from
// what the library already has: shards (polygons), halftone discs, slaps and shakes,
// hard cuts. No text has a shadow.

#define YELLOW AN_HEX(0xFFE600)
#define LIME AN_HEX(0xB6FF00)
#define PAPER AN_HEX(0xF3EDE2)

// The assembled timeline: the clips' lengths (s), and a 0.8 s stinger before
// sections 1..6. The clock reads 7:00 until it starts, and 0:00 on the outro's
// "Seven minutes" (9.2 s into it).
static const double CLIP[8] = { 24.422857, 84.532653, 65.825692, 54.477755, 52.827143, 101.382630, 27.861633, 13.762653 };
static const double STING = 0.8;
static double section_start(int k) {
    double t = 0;
    for (int j = 1; j <= k; j++) t += CLIP[j - 1] + (j <= 6 ? STING : 0.0);
    return t;
}
static double clock_zero(void) { return section_start(7) + 9.2; }
static double clock_start(void) { return clock_zero() - 420.0; }
// seconds on the clock at global time t (shown rounded up)
static int clock_secs(double t) {
    const double left = 420.0 - (t > clock_start() ? t - clock_start() : 0.0);
    const int s = (int)ceil(left - 1e-9);
    return s < 0 ? 0 : s;
}
static const char *clock_str(int secs) { return nm("%d:%02d", secs / 60, secs % 60); }

// lengths of the new chapters (s), by chapter number - 8
enum { CH_COLD = 8, CH_HAND, CH_COORD_A, CH_COORD_B, CH_FIB, CH_GOOGLE, CH_MUSIC, CH_DATA, CH_OUTRO, CH_STING, CH_RECAP_T,
       CH_CLOCK, CH_LAST = CH_CLOCK };
static float NEW_DUR[CH_LAST - CH_COLD + 1];
static const char *sting_title = "TITLE";

static void cut_to(an_obj *o, float opacity) { // an instant change at the cursor (a hard cut)
    an_play_async(S, an_fade_to(o, opacity, &(an_anim_opts){ .duration = 1.0f / 240.0f }));
}

// A clock attached to this scene: its text ticks at every whole second after the
// clock starts (global time g0 + local time), up to local time `until`. at() fires the
// ticks that fall before its target, so cues and ticks share one timeline.
static an_obj *clk_text;
static double clk_g0;
static float clk_until;
static int clk_ticks; // a text holds at most 256 versions: a fresh one every 60 ticks
static double clk_m;  // the last whole second of clock time ticked (a tick is never skipped)

static void at(float t) {
    if (clk_text) {
        const double off = clock_start() - clk_g0; // the clock's start, in local time
        for (;;) {
            const double now = an_scene_time(S);
            const double m = clk_m + 1.0, tick = off + m;
            if (tick > t + 1e-9 || tick > clk_until) break;
            clk_m = m;
            if (tick > now) an_wait(S, (float)(tick - now)); // (a late tick fires at once)
            const char *str = clock_str(clock_secs(clk_g0 + tick));
            if (++clk_ticks % 60 == 0) { // hand over to a fresh text, made now with the new time
                an_obj *fresh = an_text(S, &(an_text_opts){ .parent = an_obj_parent(clk_text), .text = str, .pos = { 0, 2 }, .size = 66,
                                                            .font = an_font_mono(S), .align = AN_ALIGN_CENTER,
                                                            .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL, .riso = AN_OFF,
                                                            .shadow = AN_OFF });
                cut_to(clk_text, 0);
                clk_text = fresh;
            } else {
                an_play_async(S, an_text_set(clk_text, str, &(an_text_set_opts){ .anim = { .duration = 1.0f / 240.0f } }));
            }
        }
    }
    const double now = an_scene_time(S);
    if (now > t + 1e-6) fprintf(stderr, "cue %.2f s already passed (at %.2f s)\n", (double)t, now);
    else an_wait(S, (float)(t - now));
}

static void wait_s(float dt) { at((float)an_scene_time(S) + dt); }

static void clock_attach(an_obj *text, double g0, float until) {
    clk_text = text;
    clk_g0 = g0;
    clk_until = until;
    clk_ticks = 0;
    const double now = an_scene_time(S) + g0 - clock_start(); // clock time elapsed
    clk_m = now < 0 ? 0.0 : floor(now + 1e-9);
}


// fade a few objects together without waiting (fade_all waits)
static void fade_async(an_obj *const *objs, int n, float to, float dur) {
    for (int k = 0; k < n; k++)
        if (objs[k]) an_play_async(S, an_fade_to(objs[k], to, &(an_anim_opts){ .duration = dur }));
}

static void show(an_obj *o, float dur) { an_play_async(S, an_fade_to(o, 1, &(an_anim_opts){ .duration = dur })); }

// text, hidden until shown; never a shadow. ink: an outline in charcoal
static an_obj *word(an_obj *parent, const char *text, an_vec2 pos, float size, an_color fill, bool mono, an_align al, bool ink) {
    return an_text(S, &(an_text_opts){ .parent = parent, .text = text, .pos = pos, .size = size,
                                       .font = mono ? an_font_mono(S) : NULL, .align = al, .anchor = AN_ANCHOR_MIDDLE,
                                       .fill = fill, .stroke = ink ? CHARCOAL : (an_color){ 0 },
                                       .stroke_width = ink ? size * 0.045f : 0.0f, .hidden = true, .riso = AN_OFF,
                                       .shadow = AN_OFF });
}

// letters slap in one after another; the last shakes the camera
static void slap_in(an_obj *t, float gap, float shake) {
    an_obj_set_opacity(t, 1);
    const int n = an_text_length(t);
    for (int k = 0; k < n; k++) {
        an_play_async(S, an_slap(an_text_glyph(t, k), &(an_verb_opts){ .enter = AN_ON, .impact_shake = k == n - 1 ? shake : 0.0f }));
        wait_s(gap);
    }
}

static void slap_whole(an_obj *o, float shake) {
    an_obj_set_opacity(o, 1);
    an_play_async(S, an_slap(o, &(an_verb_opts){ .enter = AN_ON, .impact_shake = shake }));
}

// a comic burst: n shards radiating from c, slapped in fast, plus halftone discs
static int burst_n;
static an_obj *burst_objs[40];
static void burst(an_vec2 c, int n, float r0, float r1, float stagger) {
    static const an_color cols[5] = { YELLOW, MAGENTA, CYAN, LIME, ORANGE };
    an_rng *rng = an_scene_rng(S);
    burst_n = 0;
    for (int k = 0; k < 3 && burst_n < 40; k++) {
        const an_vec2 p = { c.x + an_rng_range(rng, -700, 700), c.y + an_rng_range(rng, -380, 380) };
        an_obj *d = an_circle(S, &(an_circle_opts){ .pos = p, .radius = an_rng_range(rng, 160, 300), .fill = cols[(k + 1) % 5],
                                                    .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 22, .density = 0.45f },
                                                    .hidden = true, .riso = AN_OFF, .shadow = AN_OFF, .z = -2 });
        burst_objs[burst_n++] = d;
        slap_whole(d, 0);
    }
    for (int k = 0; k < n && burst_n < 40; k++) {
        const float a = (float)k * 2.0f * PI / (float)n + an_rng_range(rng, -0.12f, 0.12f);
        const float w = an_rng_range(rng, 0.07f, 0.16f), ra = r0 * an_rng_range(rng, 0.8f, 1.2f), rb = r1 * an_rng_range(rng, 0.75f, 1.15f);
        const an_vec2 pts[3] = { { ra * cosf(a - w), ra * sinf(a - w) }, { rb * cosf(a), rb * sinf(a) }, { ra * cosf(a + w), ra * sinf(a + w) } };
        an_obj *sh = an_polygon(S, &(an_polygon_opts){ .pos = c, .points = pts, .count = 3, .fill = cols[k % 5], .stroke = CHARCOAL,
                                                       .stroke_width = 5, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF, .z = -1 });
        burst_objs[burst_n++] = sh;
        slap_whole(sh, 0);
        wait_s(stagger);
    }
}

static void burst_fade(float to, float dur) { fade_async(burst_objs, burst_n, to, dur); }

// a matrix as chunky blocks: rows x cols cells with mono numbers; top-left corner at tl
static an_obj *blocks(const char *const *cells, const an_color *ink, int rows, int cols, an_vec2 tl, float cell) {
    an_obj *g = an_group(S, &(an_group_opts){ .pos = tl, .hidden = true });
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const an_vec2 ctr = { (c + 0.5f) * cell, (r + 0.5f) * cell };
            an_rect(S, &(an_rect_opts){ .parent = g, .pos = ctr, .size = { cell - 8, cell - 8 }, .fill = YELLOW, .stroke = CHARCOAL,
                                        .stroke_width = 5, .riso = AN_OFF, .shadow = AN_OFF });
            an_text(S, &(an_text_opts){ .parent = g, .text = cells[r * cols + c], .pos = ctr,
                                        .size = cell * (strlen(cells[r * cols + c]) > 2 ? 0.27f : 0.5f),
                                        .font = an_font_mono(S), .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                        .fill = ink ? ink[r * cols + c] : CHARCOAL, .riso = AN_OFF, .shadow = AN_OFF });
        }
    return g;
}

// the clock tag: a yellow label with an ink border and the time (shared by the cold
// open, the overlay and the outro so the hand-offs match)
static an_obj *clock_tag(an_vec2 pos, float scale, an_obj **text, const char *str) {
    an_obj *g = an_group(S, &(an_group_opts){ .pos = pos, .scale = { scale, scale } });
    an_rect(S, &(an_rect_opts){ .parent = g, .size = { 294, 104 }, .fill = YELLOW, .stroke = CHARCOAL, .stroke_width = 6,
                                .riso = AN_OFF, .shadow = AN_OFF });
    *text = an_text(S, &(an_text_opts){ .parent = g, .text = str, .pos = { 0, 2 }, .size = 66, .font = an_font_mono(S),
                                        .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE, .fill = CHARCOAL,
                                        .riso = AN_OFF, .shadow = AN_OFF });
    return g;
}
static const an_vec2 TAG_POS = { 1750, 1005 }; // the overlay's place on screen (bottom right)

// ---- 8: the cold open (clip 0, 24.4 s) ---------------------------------------------------------
static void ch_cold_open(void) {
    // black, then a hard cut to the burst on "Seven minutes"
    an_obj *black = an_rect(S, &(an_rect_opts){ .pos = { 960, 540 }, .size = { 1960, 1120 }, .fill = CHARCOAL, .z = 50,
                                                .riso = AN_OFF, .shadow = AN_OFF });
    at(0.12f);
    cut_to(black, 0);
    an_obj *tt;
    an_obj *tag = clock_tag((an_vec2){ 960, 470 }, 3.0f, &tt, "7:00");
    an_obj_set_opacity(tag, 0);
    clock_attach(tt, section_start(0), NEW_DUR[CH_COLD - CH_COLD]);
    slap_whole(tag, 24);
    burst((an_vec2){ 960, 470 }, 16, 230, 1150, 0.012f);
    // "Seven minutes is all I can spare to play with you."
    at(2.1f);
    an_obj *spare = word(NULL, "is all I can spare to play with you.", (an_vec2){ 960, 880 }, 58, CHARCOAL, false, AN_ALIGN_CENTER, false);
    show(spare, 0.25f);
    // the clock starts; the tag moves up for the title
    at((float)clock_start() - 0.4f);
    fade_async((an_obj *[]){ spare }, 1, 0, 0.3f);
    burst_fade(0.35f, 0.4f);
    an_play_async(S, an_move_to(tag, (an_vec2){ 960, 200 }, &(an_anim_opts){ .duration = 0.5f }));
    an_play_async(S, an_scale_to(tag, (an_vec2){ 1.6f, 1.6f }, &(an_anim_opts){ .duration = 0.5f }));
    // "In here I am explaining eigenvectors."
    at(6.3f);
    an_obj *title = word(NULL, "EIGENVECTORS", (an_vec2){ 960, 560 }, 170, MAGENTA, false, AN_ALIGN_CENTER, true);
    an_play_async(S, an_rotate_to(title, -0.03f, &(an_anim_opts){ .duration = 0.01f }));
    slap_in(title, 0.06f, 18);
    // "a word that sounds like a villain's last name"
    at(13.0f);
    an_obj *dr = word(NULL, "DR. EIGEN?", (an_vec2){ 1350, 760 }, 78, ORANGE, false, AN_ALIGN_CENTER, true);
    an_play_async(S, an_rotate_to(dr, 0.08f, &(an_anim_opts){ .duration = 0.01f }));
    slap_whole(dr, 10);
    // "and turns out it's one of the most useful ideas in mathematics"
    at(16.0f);
    an_obj *useful = word(NULL, "one of the most useful ideas in maths", (an_vec2){ 960, 880 }, 46, CHARCOAL, true, AN_ALIGN_CENTER, false);
    show(useful, 0.3f);
    // "So do not memorize it, just understand it,"
    at(20.4f);
    fade_async((an_obj *[]){ dr, useful }, 2, 0, 0.25f);
    an_obj *mem = word(NULL, "MEMORIZE", (an_vec2){ 700, 800 }, 84, CHARCOAL, false, AN_ALIGN_CENTER, false);
    slap_whole(mem, 0);
    at(21.2f);
    an_obj *strike = an_line(S, &(an_line_opts){ .from = { 460, 790 }, .to = { 940, 810 }, .stroke = MAGENTA, .stroke_width = 14,
                                                 .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_draw(strike, &(an_anim_opts){ .duration = 0.25f }));
    at(22.2f);
    an_obj *und = word(NULL, "UNDERSTAND", (an_vec2){ 1300, 900 }, 96, MAGENTA, false, AN_ALIGN_CENTER, true);
    slap_whole(und, 14);
    // "the clock is running." the tag runs to its corner
    at(23.0f);
    fade_async((an_obj *[]){ title, mem, strike, und }, 4, 0, 0.35f);
    burst_fade(0, 0.35f);
    an_play_async(S, an_move_to(tag, TAG_POS, &(an_anim_opts){ .duration = 0.8f }));
    an_play_async(S, an_scale_to(tag, (an_vec2){ 1, 1 }, &(an_anim_opts){ .duration = 0.8f }));
    at(NEW_DUR[0]);
}

// ---- 9: finding them by hand (clip 2, 65.8 s) ---------------------------------------------------
static an_mat2 a_minus(float lam) { return (an_mat2){ 3 - lam, 1, 0, 2 - lam }; }

static void ch_by_hand(void) {
    P = an_plane(S, &(an_plane_opts){ .name = "plane", .pos = { 1390, 560 }, .unit = 78 });
    // the left panel: paper over the grid, an ink border
    an_rect(S, &(an_rect_opts){ .pos = { 455, 540 }, .size = { 850, 1010 }, .fill = PAPER, .stroke = CHARCOAL, .stroke_width = 6,
                                .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *alab = word(NULL, "A =", (an_vec2){ 90, 190 }, 64, CHARCOAL, true, AN_ALIGN_LEFT, false);
    an_obj *ab = blocks((const char *[]){ "3", "1", "0", "2" }, NULL, 2, 2, (an_vec2){ 230, 110 }, 82);
    const float x0 = 80, y0 = 360, dy = 88;
    static const char *const eqs[6] = { "A v = \xCE\xBB v", "A v \xE2\x88\x92 \xCE\xBB v = 0", "(A \xE2\x88\x92 \xCE\xBB I) v = 0",
                                        "det(A \xE2\x88\x92 \xCE\xBB I) = 0", "(3 \xE2\x88\x92 \xCE\xBB)(2 \xE2\x88\x92 \xCE\xBB) = 0",
                                        "\xCE\xBB = 3  or  \xCE\xBB = 2" };
    static const float when[6] = { 0.1f, 3.4f, 5.9f, 16.1f, 21.1f, 28.1f };
    an_obj *eq[6];
    for (int k = 0; k < 6; k++)
        eq[k] = word(NULL, eqs[k], (an_vec2){ x0, y0 + dy * (float)k }, 56, k == 3 ? MAGENTA : CHARCOAL, true, AN_ALIGN_LEFT, false);
    // the plane: v = (1, 1) on it, a readout for lambda
    an_obj *v = arrow("hand/v", (an_vec2){ 1, 1 }, CHARCOAL, 6, false);
    an_obj_set_opacity(v, 0);
    an_obj *lam = word(NULL, "\xCE\xBB = 0", (an_vec2){ 1870, 70 }, 52, CHARCOAL, true, AN_ALIGN_RIGHT, false);
    // "A v = lambda v."
    at(when[0]);
    slap_whole(ab, 0);
    show(alab, 0.2f);
    slap_in(eq[0], 0.06f, 0);
    // "Move everything to one side: A - lambda times the identity, times v, equals zero."
    at(when[1]);
    show(eq[1], 0.35f);
    at(when[2]);
    show(eq[2], 0.35f);
    // "We want a v that isn't zero."
    at(10.1f);
    an_obj *nz = word(NULL, "v \xE2\x89\xA0 0", (an_vec2){ 700, y0 + dy * 2 }, 50, ORANGE, true, AN_ALIGN_LEFT, false);
    slap_whole(nz, 0);
    show(v, 0.3f);
    // "So this new matrix has to crush some arrows down to nothing."
    at(13.0f);
    show(lam, 0.3f);
    an_play(S, an_plane_matrix_to(P, a_minus(0), &(an_anim_opts){ .duration = 1.2f }));
    // "And a matrix crushes something flat exactly when its determinant is zero."
    at(when[3]);
    slap_in(eq[3], 0.05f, 8);
    // "For our matrix that's (3 - lambda)(2 - lambda) = 0."
    at(when[4]);
    show(eq[4], 0.4f);
    // "So lambda is 3 or 2. The same numbers we saw stretching."
    at(when[5]);
    slap_in(eq[5], 0.05f, 10);
    // "Try anything else, say two and a half: the determinant isn't zero."
    at(32.0f);
    an_play_async(S, an_text_set(lam, nm("\xCE\xBB = 2.5   det = %g", (double)(0.5f * -0.5f)), NULL));
    an_play(S, an_plane_matrix_to(P, a_minus(2.5f), &(an_anim_opts){ .duration = 1.5f }));
    // "Nothing gets crushed, and there's no eigenvector. Only 3 and 2 collapse the plane."
    at(39.0f);
    an_play_async(S, an_text_set(lam, "\xCE\xBB = 3   det = 0", NULL));
    an_play(S, an_plane_matrix_to(P, a_minus(3), &(an_anim_opts){ .duration = 1.2f }));
    // "Plug 3 back in, and the matrix kills every arrow along the x-axis."
    at(41.0f);
    an_play_async(S, an_fade_to(v, 0, &(an_anim_opts){ .duration = 0.3f }));
    an_play(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 0.6f }));
    an_obj *u1l = span("hand/span/0", (an_vec2){ 1, 0 }, MAGENTA, true);
    an_obj *u1 = arrow("hand/u1", (an_vec2){ 2, 0 }, MAGENTA, 7, true);
    an_play_async(S, an_draw(u1l, &(an_anim_opts){ .duration = 0.6f }));
    an_play(S, an_draw(u1, &(an_anim_opts){ .duration = 0.5f }));
    at(43.6f);
    an_play(S, an_plane_matrix_to(P, a_minus(3), &(an_anim_opts){ .duration = 1.3f }));
    printf("by hand: (A - 3I)(1, 0) = (%g, %g)\n", (double)mul(a_minus(3), (an_vec2){ 1, 0 }).x, (double)mul(a_minus(3), (an_vec2){ 1, 0 }).y);
    // "Plug in 2, and it kills the diagonal line going down to the right."
    at(46.0f);
    an_play_async(S, an_text_set(lam, "\xCE\xBB = 2   det = 0", NULL));
    an_play(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 0.6f }));
    an_obj *u2l = span("hand/span/1", (an_vec2){ 1, -1 }, CYAN, true);
    an_obj *u2 = arrow("hand/u2", (an_vec2){ 1.5f, -1.5f }, CYAN, 7, true);
    an_play_async(S, an_draw(u2l, &(an_anim_opts){ .duration = 0.6f }));
    an_play(S, an_draw(u2, &(an_anim_opts){ .duration = 0.5f }));
    at(47.8f);
    an_play(S, an_plane_matrix_to(P, a_minus(2), &(an_anim_opts){ .duration = 1.3f }));
    printf("by hand: (A - 2I)(1, -1) = (%g, %g)\n", (double)mul(a_minus(2), (an_vec2){ 1, -1 }).x, (double)mul(a_minus(2), (an_vec2){ 1, -1 }).y);
    // "Those are our eigenvectors, found with nothing but algebra."
    at(51.0f);
    an_play_async(S, an_fade_to(lam, 0, &(an_anim_opts){ .duration = 0.3f }));
    an_play(S, an_plane_matrix_to(P, IDENTITY, &(an_anim_opts){ .duration = 0.8f }));
    an_obj *t1 = plane_label("hand/u1/label", "(1, 0)", (an_vec2){ 2.3f, 0.45f }, MAGENTA, 40);
    an_obj *t2 = plane_label("hand/u2/label", "(1, \xE2\x88\x92" "1)", (an_vec2){ 2.3f, -1.7f }, CYAN, 40);
    an_obj_set_opacity(t1, 0);
    an_obj_set_opacity(t2, 0);
    show(t1, 0.3f);
    show(t2, 0.3f);
    // "And a free sanity check: the eigenvalues add up to the trace, 3 + 2 = 5."
    at(57.8f);
    an_obj *tr = word(NULL, "trace = 3 + 2 = 5", (an_vec2){ 80, 930 }, 54, MAGENTA, true, AN_ALIGN_LEFT, false);
    slap_whole(tr, 8);
    // "They multiply to the determinant, 3 times 2 = 6."
    at(62.3f);
    an_obj *de = word(NULL, "det = 3 \xC3\x97 2 = 6", (an_vec2){ 80, 1010 }, 54, CYAN, true, AN_ALIGN_LEFT, false);
    slap_whole(de, 8);
    at(NEW_DUR[CH_HAND - CH_COLD]);
}

// ---- 10: eigen-coordinates, first part (clip 4, 0 - 13.9 s) -------------------------------------
static void ch_coord_a(void) {
    P = an_plane(S, &(an_plane_opts){ .name = "plane", .grid_step = -1 });
    // the ordinary grid, drawn as lines (so it can leave)
    an_obj *grid[34];
    int ng = 0;
    for (int k = -10; k <= 10; k++)
        grid[ng++] = an_plane_line(P, (an_vec2){ (float)k, 0 }, (an_vec2){ 0, 1 }, &(an_line_opts){ .name = nm("ca/gx/%d", k), .z = -9,
                                   .stroke = pale(CHARCOAL, 0.75f), .stroke_width = 2, .shadow = AN_OFF, .riso = AN_OFF });
    for (int k = -6; k <= 6; k++)
        grid[ng++] = an_plane_line(P, (an_vec2){ 0, (float)k }, (an_vec2){ 1, 0 }, &(an_line_opts){ .name = nm("ca/gy/%d", k), .z = -9,
                                   .stroke = pale(CHARCOAL, 0.75f), .stroke_width = 2, .shadow = AN_OFF, .riso = AN_OFF });
    // "Use the eigenvectors as your grid lines,"
    at(3.06f);
    fade_all(grid, ng, 0, 0.6f);
    an_obj *eg[48];
    int ne = 0;
    for (int k = -8; k <= 8; k++)
        eg[ne++] = an_plane_line(P, (an_vec2){ (float)k, (float)-k }, (an_vec2){ 1, 0 }, &(an_line_opts){ .name = nm("ca/em/%d", k), .z = -8,
                                 .stroke = k ? pale(MAGENTA, 0.45f) : MAGENTA, .stroke_width = k ? 2 : 4, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    for (int k = -12; k <= 12; k++)
        eg[ne++] = an_plane_line(P, (an_vec2){ (float)k, 0 }, (an_vec2){ 1, -1 }, &(an_line_opts){ .name = nm("ca/ec/%d", k), .z = -8,
                                 .stroke = k ? pale(CYAN, 0.45f) : CYAN, .stroke_width = k ? 2 : 4, .undrawn = true, .shadow = AN_OFF, .riso = AN_OFF });
    for (int k = 0; k < ne; k++) {
        an_play_async(S, an_draw(eg[k], &(an_anim_opts){ .duration = 0.5f }));
        an_wait(S, 0.02f);
    }
    // "and the transformation stops being messy."
    at(5.0f);
    an_play(S, an_plane_matrix_to(P, A, &(an_anim_opts){ .duration = 2.2f }));
    // "It's just: stretch this way by 3, stretch that way by 2."
    at(7.8f);
    an_obj *x3 = word(NULL, "\xC3\x97" "3", (an_vec2){ 1660, 500 }, 96, MAGENTA, true, AN_ALIGN_CENTER, true);
    slap_whole(x3, 10);
    at(9.8f);
    const an_vec2 q = an_plane_point_at_cursor(P, (an_vec2){ -1.6f, 1.6f });
    an_obj *x2 = word(NULL, "\xC3\x97" "2", (an_vec2){ q.x - 60, q.y - 40 }, 96, CYAN, true, AN_ALIGN_CENTER, true);
    slap_whole(x2, 10);
    // "That is called diagonalizing."
    at(11.8f);
    an_obj *dg = word(NULL, "DIAGONALIZING", (an_vec2){ 960, 120 }, 110, MAGENTA, false, AN_ALIGN_CENTER, true);
    slap_in(dg, 0.035f, 14);
    an_obj *d = blocks((const char *[]){ "3", "0", "0", "2" }, (const an_color[]){ MAGENTA, CHARCOAL, CHARCOAL, CYAN }, 2, 2,
                       (an_vec2){ 90, 760 }, 110);
    slap_whole(d, 0);
    at(NEW_DUR[CH_COORD_A - CH_COLD]);
}

// ---- 11: eigen-coordinates, second part (clip 4, 30.9 - 52.8 s) ---------------------------------
static void ch_coord_b(void) {
    // "Apply it a hundred times, and it's just 3 to the 100, and 2 to the 100."
    at(0.2f);
    an_obj *a100 = word(NULL, "A\xC2\xB9\xE2\x81\xB0\xE2\x81\xB0", (an_vec2){ 960, 300 }, 190, CHARCOAL, true, AN_ALIGN_CENTER, false);
    slap_whole(a100, 16);
    const double p3 = pow(3.0, 100), p2 = pow(2.0, 100);
    const int e3 = (int)floor(log10(p3)), e2 = (int)floor(log10(p2));
    printf("coordinates: 3^100 = %.3g, 2^100 = %.3g, ratio %.3g\n", p3, p2, p3 / p2);
    // the exponents as superscript digits
    static const char *const sup[10] = { "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4", "\xE2\x81\xB5", "\xE2\x81\xB6",
                                         "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9" };
    at(2.6f);
    an_obj *t3 = word(NULL, nm("3\xC2\xB9\xE2\x81\xB0\xE2\x81\xB0 \xE2\x89\x88 %.1f \xC3\x97 10%s%s", p3 / pow(10, e3), sup[e3 / 10], sup[e3 % 10]),
                      (an_vec2){ 960, 560 }, 80, MAGENTA, true, AN_ALIGN_CENTER, false);
    slap_whole(t3, 8);
    at(4.1f);
    an_obj *t2 = word(NULL, nm("2\xC2\xB9\xE2\x81\xB0\xE2\x81\xB0 \xE2\x89\x88 %.1f \xC3\x97 10%s%s", p2 / pow(10, e2), sup[e2 / 10], sup[e2 % 10]),
                      (an_vec2){ 960, 700 }, 80, CYAN, true, AN_ALIGN_CENTER, false);
    slap_whole(t2, 8);
    // "In the long run, the biggest eigenvalue takes over everything."
    at(6.6f);
    an_obj *bm = an_line(S, &(an_line_opts){ .from = { 150, 860 }, .to = { 2100, 860 }, .stroke = MAGENTA, .stroke_width = 60,
                                             .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *bc = an_line(S, &(an_line_opts){ .from = { 150, 960 }, .to = { 156, 960 }, .stroke = CYAN, .stroke_width = 60,
                                             .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_draw(bm, &(an_anim_opts){ .duration = 1.2f }));
    an_play_async(S, an_draw(bc, &(an_anim_opts){ .duration = 1.2f }));
    an_wait(S, 1.3f);
    an_obj *ratio = word(NULL, nm("3\xC2\xB9\xE2\x81\xB0\xE2\x81\xB0 / 2\xC2\xB9\xE2\x81\xB0\xE2\x81\xB0 \xE2\x89\x88 %.0f \xC3\x97 10%s%s",
                                  p3 / p2 / pow(10, floor(log10(p3 / p2))), sup[(int)floor(log10(p3 / p2)) / 10],
                                  sup[(int)floor(log10(p3 / p2)) % 10]),
                         (an_vec2){ 200, 960 }, 50, CHARCOAL, true, AN_ALIGN_LEFT, false);
    show(ratio, 0.3f);
    // "Engineers use exactly this to ask: will this system blow up, or calm down?"
    at(10.9f);
    fade_async((an_obj *[]){ a100, t3, t2, bm, bc, ratio }, 6, 0, 0.3f);
    an_obj *q = word(NULL, "blow up, or calm down?", (an_vec2){ 960, 110 }, 88, CHARCOAL, false, AN_ALIGN_CENTER, true);
    slap_in(q, 0.03f, 10);
    an_obj *pl[2], *vec[2];
    static const float lamv[2] = { 1.25f, 0.75f };
    for (int k = 0; k < 2; k++) {
        pl[k] = an_plane(S, &(an_plane_opts){ .name = nm("cb/plane/%d", k), .pos = { k ? 1440.0f : 480.0f, 640 }, .unit = 45,
                                              .axes = true, .grid_step = -1 }); // side by side: axes only
        P = pl[k];
        vec[k] = arrow(nm("cb/v/%d", k), (an_vec2){ 2.0f, 1.2f }, k ? CYAN : MAGENTA, 7, true);
        an_play_async(S, an_draw(vec[k], &(an_anim_opts){ .duration = 0.4f }));
        an_obj *lb = word(NULL, nm("\xCE\xBB = %.2f", (double)lamv[k]), (an_vec2){ k ? 1440.0f : 480.0f, 270 }, 60, k ? CYAN : MAGENTA,
                          true, AN_ALIGN_CENTER, false);
        show(lb, 0.3f);
    }
    // the same arrow, times lambda, again and again
    at(12.0f);
    float sc[2] = { 1, 1 };
    for (int step = 1; step <= 7; step++) {
        for (int k = 0; k < 2; k++) {
            sc[k] *= lamv[k];
            an_play_async(S, an_scale_to(vec[k], (an_vec2){ sc[k], sc[k] }, &(an_anim_opts){ .duration = 0.45f }));
        }
        an_wait(S, 0.6f);
    }
    // "Eigenvalues bigger than 1, it grows. Smaller than 1, it dies out."
    at(16.3f);
    an_obj *gr = word(NULL, "bigger than 1: it grows", (an_vec2){ 480, 900 }, 52, MAGENTA, true, AN_ALIGN_CENTER, false);
    slap_whole(gr, 8);
    at(20.3f);
    an_obj *dies = word(NULL, "smaller than 1: it dies out", (an_vec2){ 1440, 900 }, 52, CYAN, true, AN_ALIGN_CENTER, false);
    slap_whole(dies, 8);
    at(NEW_DUR[CH_COORD_B - CH_COLD]);
}

// ---- 12: Fibonacci (clip 5, 0 - 38.6 s) ---------------------------------------------------------
static void ch_fib(void) {
    an_obj *ti = word(NULL, "FIBONACCI", (an_vec2){ 960, 110 }, 120, MAGENTA, false, AN_ALIGN_CENTER, true);
    at(0.2f);
    slap_in(ti, 0.04f, 14);
    // "1, 1, 2, 3, 5, 8": bars, each the sum of the two before
    int fib[10] = { 1, 1 };
    for (int k = 2; k < 10; k++) fib[k] = fib[k - 1] + fib[k - 2];
    an_obj *bars[8], *nums[8];
    static const float when[8] = { 1.5f, 1.9f, 2.3f, 2.8f, 3.2f, 3.6f, 5.0f, 5.6f };
    for (int k = 0; k < 8; k++) {
        const float h = 13.0f * (float)fib[k], x = 140.0f + 92.0f * (float)k;
        bars[k] = an_rect(S, &(an_rect_opts){ .pos = { x, 960 - h * 0.5f }, .size = { 70, h }, .fill = k < 6 ? YELLOW : LIME,
                                              .stroke = CHARCOAL, .stroke_width = 5, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
        nums[k] = word(NULL, nm("%d", fib[k]), (an_vec2){ x, 930 - h }, 40, CHARCOAL, true, AN_ALIGN_CENTER, false);
    }
    an_obj *rule = word(NULL, "next = last + the one before", (an_vec2){ 140, 290 }, 44, CHARCOAL, true, AN_ALIGN_LEFT, false);
    for (int k = 0; k < 8; k++) {
        if (k == 6) { // "Each number is the last two added."
            at(4.8f);
            show(rule, 0.3f);
        }
        at(when[k]);
        slap_whole(bars[k], 0);
        slap_whole(nums[k], 0);
    }
    // "That rule is a matrix: 1 1, 1 0."
    at(7.4f);
    an_obj *m = blocks((const char *[]){ "1", "1", "1", "0" }, NULL, 2, 2, (an_vec2){ 1080, 220 }, 100);
    slap_whole(m, 10);
    an_obj *mlab = word(NULL, "M =", (an_vec2){ 1060, 320 }, 60, CHARCOAL, true, AN_ALIGN_RIGHT, false);
    show(mlab, 0.2f);
    // "Its biggest eigenvalue is 1.618, the golden ratio."
    const double phi = (1.0 + sqrt(5.0)) / 2.0, psi = (1.0 - sqrt(5.0)) / 2.0;
    printf("fibonacci: eigenvalues of [[1,1],[1,0]]: %.6f and %.6f\n", phi, psi);
    at(10.0f);
    an_obj *l1 = word(NULL, nm("\xCE\xBB\xE2\x82\x81 = %.3f\xE2\x80\xA6", phi), (an_vec2){ 1340, 300 }, 60, MAGENTA, true, AN_ALIGN_LEFT, false);
    slap_whole(l1, 8);
    at(15.0f);
    an_obj *ph = word(NULL, "= \xCF\x86, the golden ratio", (an_vec2){ 1340, 380 }, 46, ORANGE, true, AN_ALIGN_LEFT, false);
    slap_whole(ph, 8);
    // "which is why the ratio of neighbouring Fibonacci numbers creeps toward it": a
    // number line from 1 to 2, and the ratios landing on it
    at(16.3f);
    const float lx0 = 1000, lx1 = 1820, ly = 640;
    an_obj *nl = an_line(S, &(an_line_opts){ .from = { lx0, ly }, .to = { lx1, ly }, .stroke = CHARCOAL, .stroke_width = 5, .undrawn = true,
                                             .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_draw(nl, &(an_anim_opts){ .duration = 0.4f }));
    const float xp = lx0 + (lx1 - lx0) * (float)(phi - 1.0);
    an_obj *tick = an_line(S, &(an_line_opts){ .from = { xp, ly - 26 }, .to = { xp, ly + 26 }, .stroke = ORANGE, .stroke_width = 7,
                                               .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_draw(tick, &(an_anim_opts){ .duration = 0.3f }));
    an_obj *e1 = word(NULL, "1", (an_vec2){ lx0, ly + 50 }, 36, CHARCOAL, true, AN_ALIGN_CENTER, false);
    an_obj *e2 = word(NULL, "2", (an_vec2){ lx1, ly + 50 }, 36, CHARCOAL, true, AN_ALIGN_CENTER, false);
    an_obj *ep = word(NULL, "\xCF\x86", (an_vec2){ xp, ly + 50 }, 40, ORANGE, true, AN_ALIGN_CENTER, false);
    show(e1, 0.3f);
    show(e2, 0.3f);
    show(ep, 0.3f);
    an_obj *dot = an_circle(S, &(an_circle_opts){ .pos = { lx0, ly }, .radius = 13, .fill = MAGENTA, .stroke = CHARCOAL, .stroke_width = 4,
                                                  .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *rr = word(NULL, "1/1 = 1.000", (an_vec2){ 1410, 560 }, 44, CHARCOAL, true, AN_ALIGN_CENTER, false);
    show(dot, 0.2f);
    show(rr, 0.2f);
    an_wait(S, 0.4f);
    for (int k = 1; k < 9; k++) {
        const double r = (double)fib[k + 1] / (double)fib[k];
        an_play_async(S, an_move_to(dot, (an_vec2){ lx0 + (lx1 - lx0) * (float)(r - 1.0), ly }, &(an_anim_opts){ .duration = 0.35f }));
        an_play_async(S, an_text_set(rr, nm("%d/%d = %.3f", fib[k + 1], fib[k], r), &(an_text_set_opts){ .anim = { .duration = 0.05f } }));
        an_wait(S, 0.42f);
    }
    // "The other eigenvalue is negative and smaller than one, so it flips, shrinks, and fades away."
    at(20.7f);
    an_obj *l2 = word(NULL, nm("\xCE\xBB\xE2\x82\x82 = %.3f\xE2\x80\xA6", psi), (an_vec2){ 1340, 800 }, 60, CYAN, true, AN_ALIGN_LEFT, false);
    slap_whole(l2, 8);
    an_obj *fl = an_arrow(S, &(an_arrow_opts){ .pos = { 1560, 920 }, .from = { 0, 0 }, .to = { 200, 0 }, .stroke = CYAN, .stroke_width = 8,
                                               .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    show(fl, 0.2f);
    at(26.4f);
    double sgn = 1;
    for (int k = 1; k <= 5; k++) {
        sgn *= psi;
        an_play_async(S, an_scale_to(fl, (an_vec2){ (float)sgn, (float)fabs(sgn) }, &(an_anim_opts){ .duration = 0.25f }));
        an_wait(S, 0.3f);
    }
    an_play_async(S, an_fade_to(fl, 0, &(an_anim_opts){ .duration = 0.4f }));
    // "That's how you can jump straight to the 15th Fibonacci number: golden ratio to the
    // 15th, divided by root 5, rounded. No adding required."
    at(28.7f);
    an_obj *gone[] = { rule, m, mlab, l1, ph, nl, tick, e1, e2, ep, dot, rr, l2 };
    fade_async(gone, 13, 0, 0.3f);
    fade_async(bars, 8, 0, 0.3f);
    fade_async(nums, 8, 0, 0.3f);
    int f15;
    {
        int a = 1, b = 1;
        for (int k = 3; k <= 15; k++) {
            const int c = a + b;
            a = b;
            b = c;
        }
        f15 = b;
    }
    const double bin = pow(phi, 15) / sqrt(5.0);
    printf("fibonacci: F15 = %d, phi^15 / sqrt 5 = %.6f\n", f15, bin);
    at(30.0f);
    an_obj *fq = word(NULL, "F\xE2\x82\x81\xE2\x82\x85 = ?", (an_vec2){ 960, 420 }, 120, CHARCOAL, true, AN_ALIGN_CENTER, false);
    slap_whole(fq, 12);
    at(33.0f);
    an_obj *fx = word(NULL, "\xCF\x86\xC2\xB9\xE2\x81\xB5 / \xE2\x88\x9A" "5", (an_vec2){ 960, 600 }, 100, ORANGE, true, AN_ALIGN_CENTER, false);
    slap_whole(fx, 8);
    at(35.4f);
    an_obj *fv = word(NULL, nm("= %.4f\xE2\x80\xA6 \xE2\x86\x92 %d", bin, f15), (an_vec2){ 960, 760 }, 80, MAGENTA, true, AN_ALIGN_CENTER, false);
    slap_whole(fv, 10);
    at(37.3f);
    an_obj *na = word(NULL, "no adding required", (an_vec2){ 960, 900 }, 50, CHARCOAL, false, AN_ALIGN_CENTER, false);
    show(na, 0.2f);
    at(NEW_DUR[CH_FIB - CH_COLD]);
}

// ---- 13: Google (clip 5, 38.6 - 61.2 s) ---------------------------------------------------------
enum { NPAGES = 6, NLINKS = 10 };
static void ch_google(void) {
    an_obj *ti = word(NULL, "GOOGLE", (an_vec2){ 960, 100 }, 120, MAGENTA, false, AN_ALIGN_CENTER, true);
    at(0.2f);
    slap_in(ti, 0.05f, 14);
    static const an_vec2 pos[NPAGES] = { { 360, 360 }, { 760, 300 }, { 640, 640 }, { 260, 760 }, { 1000, 830 }, { 1060, 520 } };
    static const int link[NLINKS][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 }, { 2, 0 }, { 3, 2 }, { 4, 2 }, { 4, 5 }, { 5, 0 }, { 1, 3 }, { 3, 4 } };
    static const char *const names[NPAGES] = { "A", "B", "C", "D", "E", "F" };
    // PageRank: the random surfer's long-run share of time, by power iteration (damping 0.85)
    double r[NPAGES], nr[NPAGES], hist[9][NPAGES];
    int out[NPAGES] = { 0 };
    for (int l = 0; l < NLINKS; l++) out[link[l][0]]++;
    for (int i = 0; i < NPAGES; i++) r[i] = 1.0 / NPAGES;
    for (int it = 0; it < 60; it++) {
        if (it < 9) memcpy(hist[it], r, sizeof r);
        for (int i = 0; i < NPAGES; i++) nr[i] = 0.15 / NPAGES;
        for (int l = 0; l < NLINKS; l++) nr[link[l][1]] += 0.85 * r[link[l][0]] / out[link[l][0]];
        memcpy(r, nr, sizeof r);
    }
    printf("google: ranks");
    for (int i = 0; i < NPAGES; i++) printf(" %s %.3f", names[i], r[i]);
    printf("\n");
    an_obj *node[NPAGES], *edge[NLINKS];
    at(0.55f);
    for (int i = 0; i < NPAGES; i++) {
        node[i] = an_node(S, &(an_node_opts){ .pos = pos[i], .radius = 46, .label = names[i], .fill = YELLOW, .hidden = true,
                                              .riso = AN_OFF, .shadow = AN_OFF });
        slap_whole(node[i], 0);
        an_wait(S, 0.15f);
    }
    for (int l = 0; l < NLINKS; l++) {
        edge[l] = an_edge(S, &(an_edge_opts){ .from = node[link[l][0]], .to = node[link[l][1]], .directed = true, .bend = 0.15f,
                                              .stroke_width = 4, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_play_async(S, an_draw(edge[l], &(an_anim_opts){ .duration = 0.35f }));
        an_wait(S, 0.06f);
    }
    // "Picture someone clicking random links forever."
    at(2.7f);
    an_obj *surfer = an_circle(S, &(an_circle_opts){ .pos = pos[0], .radius = 16, .fill = ORANGE, .stroke = CHARCOAL, .stroke_width = 4,
                                                     .z = 5, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    slap_whole(surfer, 0);
    an_rng *rng = an_scene_rng(S);
    int at_page = 0;
    for (int hop = 0; hop < 13; hop++) {
        int choices[NLINKS], nc = 0;
        for (int l = 0; l < NLINKS; l++)
            if (link[l][0] == at_page) choices[nc++] = link[l][1];
        at_page = choices[an_rng_below(rng, (uint32_t)nc)];
        an_play_async(S, an_move_arc(surfer, pos[at_page], 40, &(an_anim_opts){ .duration = 0.4f }));
        an_wait(S, 0.48f);
    }
    // "That's an eigenvector of the giant matrix of links, with eigenvalue 1."
    at(9.0f);
    an_play_async(S, an_fade_to(surfer, 0, &(an_anim_opts){ .duration = 0.3f }));
    for (int i = 0; i < NPAGES; i++) {
        const float k = (float)sqrt(r[i] * NPAGES);
        an_play_async(S, an_scale_to(node[i], (an_vec2){ k, k }, &(an_anim_opts){ .ease = an_spring(0.5f, 0.4f) }));
        an_obj *share = word(NULL, nm("%.0f%%", 100.0 * r[i]), (an_vec2){ pos[i].x, pos[i].y - 46 * k - 30 }, 36, MAGENTA, true,
                             AN_ALIGN_CENTER, false);
        show(share, 0.3f);
    }
    an_obj *ev = word(NULL, "ranks = eigenvector, \xCE\xBB = 1", (an_vec2){ 140, 1010 }, 48, CHARCOAL, true, AN_ALIGN_LEFT, false);
    slap_whole(ev, 8);
    // "Early Google ranked pages by it, and found it the way we did: apply the matrix
    // again and again, and watch where the arrow settles." the rank vector, step by step
    at(13.6f);
    an_obj *bar[NPAGES];
    const float bx0 = 1340, bw = 74, by = 840; // clear of the clock tag
    for (int i = 0; i < NPAGES; i++) {
        bar[i] = an_rect(S, &(an_rect_opts){ .pos = { bx0 + bw * (float)i * 1.25f, by - 50 }, .size = { bw, 100 }, .fill = YELLOW,
                                             .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_obj *bl = word(NULL, names[i], (an_vec2){ bx0 + bw * (float)i * 1.25f, by + 36 }, 36, CHARCOAL, true, AN_ALIGN_CENTER, false);
        show(bar[i], 0.2f);
        show(bl, 0.2f);
    }
    an_obj *base = an_line(S, &(an_line_opts){ .from = { bx0 - 50, by }, .to = { bx0 + bw * 6.25f + 10, by }, .stroke = CHARCOAL,
                                               .stroke_width = 5, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_play_async(S, an_draw(base, &(an_anim_opts){ .duration = 0.3f }));
    an_obj *itl = word(NULL, "step 0", (an_vec2){ 1300, 380 }, 44, CHARCOAL, true, AN_ALIGN_LEFT, false);
    show(itl, 0.2f);
    // bars grow up from the baseline: height 600 x rank (no outline, so scaling is clean)
    for (int it = 0; it < 9; it++) {
        at(14.4f + 0.75f * (float)it);
        for (int i = 0; i < NPAGES; i++) {
            const float h = 600.0f * (float)hist[it][i];
            an_play_async(S, an_scale_to(bar[i], (an_vec2){ 1, h / 100.0f }, &(an_anim_opts){ .duration = 0.5f }));
            an_play_async(S, an_move_to(bar[i], (an_vec2){ bx0 + bw * (float)i * 1.25f, by - h * 0.5f }, &(an_anim_opts){ .duration = 0.5f }));
        }
        an_play_async(S, an_text_set(itl, nm("step %d", it), &(an_text_set_opts){ .anim = { .duration = 0.05f } }));
    }
    at(NEW_DUR[CH_GOOGLE - CH_COLD]);
}

// ---- 14: music (clip 5, 61.2 - 72.8 s) -----------------------------------------------------------
typedef struct vib { float t; int n; float sign; float half; } vib;
static int vib_cmp(const void *a, const void *b) {
    const vib *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t ? 1 : x->n - y->n;
}

static void ch_music(void) {
    an_obj *ti = word(NULL, "MUSIC", (an_vec2){ 960, 100 }, 120, MAGENTA, false, AN_ALIGN_CENTER, true);
    at(0.1f);
    slap_in(ti, 0.05f, 14);
    const float x0 = 380, x1 = 1540, L = x1 - x0, dur = NEW_DUR[CH_MUSIC - CH_COLD];
    static const float ys[3] = { 360, 590, 820 }, on[3] = { 0.5f, 4.4f, 5.0f };
    static const an_color col[3] = { MAGENTA, CYAN, ORANGE };
    an_obj *str[3], *lab[3], *pegs[6];
    for (int n = 1; n <= 3; n++) {
        an_seg seg[66];
        seg[0] = AN_MOVE(0, 0);
        for (int k = 1; k <= 64; k++) {
            const float x = L * (float)k / 64.0f;
            seg[k] = AN_LINE(x, 80.0f * sinf((float)n * PI * x / L));
        }
        str[n - 1] = an_stroke(S, &(an_stroke_opts){ .pos = { x0, ys[n - 1] }, .segs = seg, .seg_count = 65, .stroke = col[n - 1],
                                                    .stroke_width = 7, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
        for (int e = 0; e < 2; e++)
            pegs[2 * (n - 1) + e] = an_circle(S, &(an_circle_opts){ .pos = { e ? x1 : x0, ys[n - 1] }, .radius = 12, .fill = CHARCOAL,
                                                                    .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
        lab[n - 1] = word(NULL, n == 1 ? "f" : nm("%df", n), (an_vec2){ 260, ys[n - 1] }, 64, col[n - 1], true, AN_ALIGN_CENTER, false);
    }
    // each string holds its shape and vibrates: it scales from 1 to -1 and back, the n-th
    // n times as fast. One time-ordered list of events for all three.
    vib ev[600];
    int ne = 0;
    for (int n = 1; n <= 3; n++) {
        ev[ne++] = (vib){ on[n - 1], n, 0, 0 }; // appear
        const float half = 0.45f / (float)n;
        int j = 0;
        for (float t = on[n - 1] + 0.4f; t + half <= dur && ne < 600; t += half, j++) ev[ne++] = (vib){ t, n, j % 2 ? 1.0f : -1.0f, half };
    }
    // the words, as events too: "the fundamental" (4.3), "and its harmonics" (5.6),
    // "are eigenvectors of the string" (6.4), "their eigenvalues set the pitch" (8.9)
    ev[ne++] = (vib){ 4.3f, 11, 0, -1 };
    ev[ne++] = (vib){ 5.6f, 12, 0, -1 };
    ev[ne++] = (vib){ 6.4f, 13, 0, -1 };
    ev[ne++] = (vib){ 8.9f, 14, 0, -1 };
    an_obj *evt = word(NULL, "shapes that hold = eigenvectors", (an_vec2){ 960, 980 }, 50, CHARCOAL, true, AN_ALIGN_CENTER, false);
    an_obj *pv = word(NULL, "eigenvalues \xE2\x86\x92 pitch", (an_vec2){ 960, 1045 }, 50, MAGENTA, true, AN_ALIGN_CENTER, false);
    qsort(ev, (size_t)ne, sizeof ev[0], vib_cmp);
    for (int i = 0; i < ne; i++) {
        at(ev[i].t);
        if (ev[i].half < 0) {
            if (ev[i].n == 11) show(lab[0], 0.2f);
            if (ev[i].n == 12) {
                show(lab[1], 0.2f);
                show(lab[2], 0.2f);
            }
            if (ev[i].n == 13) slap_whole(evt, 6);
            if (ev[i].n == 14) slap_whole(pv, 6);
            continue;
        }
        const int k = ev[i].n - 1;
        if (ev[i].half == 0) {
            show(pegs[2 * k], 0.2f);
            show(pegs[2 * k + 1], 0.2f);
            an_play_async(S, an_draw(str[k], &(an_anim_opts){ .duration = 0.35f }));
            continue;
        }
        an_play_async(S, an_scale_to(str[k], (an_vec2){ 1, ev[i].sign }, &(an_anim_opts){ .duration = ev[i].half }));
    }
    at(dur);
}

// ---- 15: data (clip 5, 72.8 - 101.4 s) -------------------------------------------------------------
enum { NPTS = 90 };
static void ch_data(void) {
    P = an_plane(S, &(an_plane_opts){ .name = "plane", .pos = { 1180, 560 }, .unit = 62 });
    an_obj *ti = word(NULL, "DATA", (an_vec2){ 300, 110 }, 120, MAGENTA, false, AN_ALIGN_CENTER, true);
    at(0.1f);
    slap_in(ti, 0.05f, 12);
    // "Throw a cloud of points on the plane": a tilted Gaussian cloud (Box-Muller),
    // shifted so its mean is the origin (the arrows start there)
    an_rng *rng = an_scene_rng(S);
    an_vec2 pts[NPTS];
    const float ang = 0.52f;
    double mx = 0, my = 0;
    for (int i = 0; i < NPTS; i++) {
        const float u1 = fmaxf(an_rng_float(rng), 1e-6f), u2 = an_rng_float(rng), u3 = fmaxf(an_rng_float(rng), 1e-6f), u4 = an_rng_float(rng);
        const float g1 = sqrtf(-2 * logf(u1)) * cosf(2 * PI * u2), g2 = sqrtf(-2 * logf(u3)) * cosf(2 * PI * u4);
        const float a = 2.4f * g1, b = 0.7f * g2;
        pts[i] = (an_vec2){ a * cosf(ang) - b * sinf(ang), a * sinf(ang) + b * cosf(ang) };
        mx += pts[i].x;
        my += pts[i].y;
    }
    mx /= NPTS;
    my /= NPTS;
    for (int i = 0; i < NPTS; i++) pts[i] = (an_vec2){ (float)(pts[i].x - mx), (float)(pts[i].y - my) };
    double sxx = 0, sxy = 0, syy = 0;
    for (int i = 0; i < NPTS; i++) {
        sxx += (double)pts[i].x * pts[i].x;
        sxy += (double)pts[i].x * pts[i].y;
        syy += (double)pts[i].y * pts[i].y;
    }
    sxx /= NPTS - 1;
    sxy /= NPTS - 1;
    syy /= NPTS - 1;
    // the eigen decomposition of the symmetric 2x2 covariance
    const double tr = sxx + syy, det = sxx * syy - sxy * sxy, disc = sqrt(tr * tr / 4 - det);
    const double l1 = tr / 2 + disc, l2 = tr / 2 - disc;
    double vx = sxy, vy = l1 - sxx;
    const double vl = hypot(vx, vy);
    vx /= vl;
    vy /= vl;
    printf("data: covariance [[%.3f, %.3f], [%.3f, %.3f]], eigenvalues %.3f %.3f, v1 (%.3f, %.3f)\n", sxx, sxy, sxy, syy, l1, l2, vx, vy);
    an_obj *dots[NPTS];
    for (int i = 0; i < NPTS; i++)
        dots[i] = an_plane_dot(P, pts[i], &(an_circle_opts){ .name = nm("data/p/%d", i), .radius = 8, .fill = CHARCOAL, .hidden = true,
                                                              .riso = AN_OFF, .shadow = AN_OFF });
    at(0.5f);
    fade_all(dots, NPTS, 1, 0.3f);
    // "and build its covariance matrix."
    at(3.3f);
    an_obj *cl = word(NULL, "covariance", (an_vec2){ 90, 290 }, 50, CHARCOAL, false, AN_ALIGN_LEFT, false);
    show(cl, 0.2f);
    an_obj *cm = blocks((const char *[]){ nm("%.2f", sxx), nm("%.2f", sxy), nm("%.2f", sxy), nm("%.2f", syy) }, NULL, 2, 2,
                        (an_vec2){ 90, 330 }, 130);
    slap_whole(cm, 8);
    // "Its eigenvectors point along the directions the data spreads the most."
    at(5.5f);
    an_obj *e1 = arrow("data/e1", (an_vec2){ (float)(vx * 2 * sqrt(l1)), (float)(vy * 2 * sqrt(l1)) }, MAGENTA, 8, true);
    an_obj *e2 = arrow("data/e2", (an_vec2){ (float)(-vy * 2 * sqrt(l2)), (float)(vx * 2 * sqrt(l2)) }, CYAN, 8, true);
    an_play(S, an_draw(e1, &(an_anim_opts){ .duration = 0.5f }));
    an_play(S, an_draw(e2, &(an_anim_opts){ .duration = 0.5f }));
    // "That's principal component analysis,"
    at(10.5f);
    an_obj *pca = word(NULL, "PCA", (an_vec2){ 300, 690 }, 140, MAGENTA, false, AN_ALIGN_CENTER, true);
    slap_in(pca, 0.06f, 14);
    // "and it's how you squeeze a thousand columns of data down to the few that matter."
    // The cloud collapses onto its magenta line, then springs back.
    at(13.0f);
    an_obj *sq = word(NULL, "1000 columns \xE2\x86\x92 the few that matter", (an_vec2){ 1000, 1010 }, 44, CHARCOAL, true, AN_ALIGN_CENTER, false);
    show(sq, 0.3f);
    at(14.0f);
    for (int i = 0; i < NPTS; i++) {
        const double d = pts[i].x * vx + pts[i].y * vy;
        an_play_async(S, an_move_to(dots[i], (an_vec2){ (float)(d * vx), (float)(d * vy) }, &(an_anim_opts){ .duration = 1.2f }));
    }
    at(16.4f);
    for (int i = 0; i < NPTS; i++) an_play_async(S, an_move_to(dots[i], pts[i], &(an_anim_opts){ .duration = 1.0f }));
    // "And because a covariance matrix is symmetric, its eigenvectors always come out
    // perfectly perpendicular."
    at(17.7f);
    fade_async((an_obj *[]){ sq }, 1, 0, 0.3f);
    an_obj *sym = word(NULL, "symmetric \xE2\x86\x92 \xE2\x8A\xA5", (an_vec2){ 90, 860 }, 64, CYAN, true, AN_ALIGN_LEFT, false);
    slap_whole(sym, 10);
    {   // the right-angle mark where the two arrows meet (the plane doesn't move here)
        const float s = 0.45f;
        const an_vec2 a1 = { (float)(vx * s), (float)(vy * s) }, a2 = { (float)(-vy * s), (float)(vx * s) };
        const an_vec2 p1 = an_plane_point_at_cursor(P, a1), p2 = an_plane_point_at_cursor(P, (an_vec2){ a1.x + a2.x, a1.y + a2.y }),
                      p3 = an_plane_point_at_cursor(P, a2);
        const an_seg seg[3] = { AN_MOVE(p1.x, p1.y), AN_LINE(p2.x, p2.y), AN_LINE(p3.x, p3.y) };
        an_obj *mark = an_stroke(S, &(an_stroke_opts){ .segs = seg, .seg_count = 3, .stroke = CHARCOAL, .stroke_width = 5, .undrawn = true,
                                                       .riso = AN_OFF, .shadow = AN_OFF });
        at(19.0f);
        an_play_async(S, an_draw(mark, &(an_anim_opts){ .duration = 0.4f }));
    }
    // "Symmetric matrices are the friendliest kind there is."
    at(24.7f);
    an_obj *fr = word(NULL, "the friendliest kind", (an_vec2){ 90, 950 }, 48, CHARCOAL, true, AN_ALIGN_LEFT, false);
    show(fr, 0.3f);
    at(NEW_DUR[CH_DATA - CH_COLD]);
}

// ---- 16: the outro (clip 7, 13.8 s) -------------------------------------------------------------
static void ch_outro(void) {
    an_obj *tt;
    // the clock tag picks up from its corner and comes forward
    an_obj *tag = clock_tag(TAG_POS, 1, &tt, clock_str(clock_secs(section_start(7))));
    clock_attach(tt, section_start(7), 9.0f); // its last tick is the 0:00 below
    an_play_async(S, an_move_to(tag, (an_vec2){ 1500, 180 }, &(an_anim_opts){ .duration = 0.6f }));
    an_play_async(S, an_scale_to(tag, (an_vec2){ 1.4f, 1.4f }, &(an_anim_opts){ .duration = 0.6f }));
    an_obj *ns = word(NULL, "NEXT STOP", (an_vec2){ 120, 300 }, 60, CHARCOAL, false, AN_ALIGN_LEFT, false);
    an_obj *svd = word(NULL, "SINGULAR VALUE DECOMPOSITION", (an_vec2){ 960, 440 }, 92, MAGENTA, false, AN_ALIGN_CENTER, true);
    an_obj *any = word(NULL, "eigen-ideas for ANY matrix", (an_vec2){ 120, 620 }, 54, CHARCOAL, true, AN_ALIGN_LEFT, false);
    an_obj *ns2 = NULL, *nsq = NULL;
    // "If this clicked, the next stop is the singular value decomposition."
    at(0.4f);
    show(ns, 0.2f);
    at(2.1f);
    slap_in(svd, 0.025f, 14);
    // "Eigen-ideas for any matrix, even ones that aren't square."
    at(5.3f);
    show(any, 0.2f);
    at(7.2f);
    ns2 = blocks((const char *[]){ "2", "0", "1", "1", "3", "4" }, NULL, 2, 3, (an_vec2){ 1180, 560 }, 110);
    slap_whole(ns2, 10);
    nsq = word(NULL, "2 \xC3\x97 3", (an_vec2){ 1345, 840 }, 50, ORANGE, true, AN_ALIGN_CENTER, false);
    show(nsq, 0.2f);
    // "Seven minutes." the clock hits zero: a burst
    at(9.2f);
    an_play_async(S, an_text_set(tt, "0:00", &(an_text_set_opts){ .anim = { .duration = 1.0f / 240.0f } }));
    an_play_async(S, an_move_to(tag, (an_vec2){ 960, 540 }, &(an_anim_opts){ .duration = 0.15f }));
    an_play_async(S, an_scale_to(tag, (an_vec2){ 3.2f, 3.2f }, &(an_anim_opts){ .duration = 0.15f }));
    fade_async((an_obj *[]){ ns, svd, any, ns2, nsq }, 5, 0, 0.12f);
    an_play_async(S, an_shake(S, 30, &(an_anim_opts){ .duration = 0.5f }));
    burst((an_vec2){ 960, 540 }, 16, 260, 1200, 0.008f);
    // "And I didn't even need all of them." a hard cut to the title card
    at(10.3f);
    cut_to(tag, 0);
    burst_fade(0.3f, 0.2f);
    an_obj *title = word(NULL, "EIGENVECTORS", (an_vec2){ 960, 500 }, 180, MAGENTA, false, AN_ALIGN_CENTER, true);
    slap_in(title, 0.05f, 18);
    an_obj *sub = word(NULL, "the directions that don't turn", (an_vec2){ 960, 690 }, 60, CHARCOAL, false, AN_ALIGN_CENTER, false);
    at(11.4f);
    show(sub, 0.3f);
    // then black
    at(13.1f);
    an_obj *black = an_rect(S, &(an_rect_opts){ .pos = { 960, 540 }, .size = { 1960, 1120 }, .fill = CHARCOAL, .z = 50, .hidden = true,
                                                .riso = AN_OFF, .shadow = AN_OFF });
    cut_to(black, 1);
    at(NEW_DUR[CH_OUTRO - CH_COLD]);
}

// ---- 17 / 18: a stinger (0.8 s) and the recap title (1.9 s): a yellow panel, a burst, a word -------
static void ch_stinger(float len) {
    an_rect(S, &(an_rect_opts){ .pos = { 960, 540 }, .size = { 1960, 1120 }, .fill = YELLOW, .z = -10, .riso = AN_OFF, .shadow = AN_OFF });
    burst((an_vec2){ 960, 540 }, 14, 300, 1250, 0.006f);
    an_obj *t = word(NULL, sting_title, (an_vec2){ 960, 540 }, 150, MAGENTA, false, AN_ALIGN_CENTER, true);
    an_play_async(S, an_rotate_to(t, -0.04f, &(an_anim_opts){ .duration = 0.01f }));
    at(0.1f);
    slap_in(t, 0.025f, 20);
    at(len);
}

// ---- 19: the clock overlay (300 x 110): the tag, ticking from the fast version to the outro -------
static void ch_clock(void) {
    an_obj *tt;
    const double g0 = section_start(1), g1 = section_start(7);
    clock_tag((an_vec2){ 150, 55 }, 1, &tt, clock_str(clock_secs(g0)));
    clock_attach(tt, g0, (float)(g1 - g0));
    at((float)(g1 - g0));
}

static void new_durations(void) {
    NEW_DUR[CH_COLD - CH_COLD] = (float)CLIP[0];
    NEW_DUR[CH_HAND - CH_COLD] = (float)CLIP[2];
    NEW_DUR[CH_COORD_A - CH_COLD] = 13.9f;                  // clip 4: up to "Remember that arrow" (chapter 5 takes 17 s)
    NEW_DUR[CH_COORD_B - CH_COLD] = (float)(CLIP[4] - 30.9); // from "Apply it a hundred times"
    NEW_DUR[CH_FIB - CH_COLD] = 38.6f;                     // clip 5: up to "Google"
    NEW_DUR[CH_GOOGLE - CH_COLD] = 22.6f;                  // up to "Music"
    NEW_DUR[CH_MUSIC - CH_COLD] = 11.6f;                   // up to "Data"
    NEW_DUR[CH_DATA - CH_COLD] = (float)(CLIP[5] - 72.8);
    NEW_DUR[CH_OUTRO - CH_COLD] = (float)CLIP[7];
    NEW_DUR[CH_STING - CH_COLD] = (float)STING;
    NEW_DUR[CH_RECAP_T - CH_COLD] = 1.9f;
    NEW_DUR[CH_CLOCK - CH_COLD] = (float)(section_start(7) - section_start(1));
}

// ---- main -----------------------------------------------------------------------------------

int main(int argc, char **argv) {
    int chapter = -1, every = 30;
    float from = -1, to = -1; // chapter 7: seconds into it (stills around a moment)
    const char *dir = NULL, *mp4 = NULL, *seams = NULL;
    bool master = false; // --master 1: export 4:4:4 near-lossless (intermediates for assembly)
    for (int k = 1; k + 1 < argc; k += 2) {
        if (strcmp(argv[k], "--chapter") == 0) chapter = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--from") == 0) from = (float)atof(argv[k + 1]);
        else if (strcmp(argv[k], "--fps") == 0) FPS = atoi(argv[k + 1]) > 0 ? atoi(argv[k + 1]) : 30;
        else if (strcmp(argv[k], "--seams") == 0) seams = argv[k + 1];
        else if (strcmp(argv[k], "--title") == 0) sting_title = argv[k + 1];
        else if (strcmp(argv[k], "--master") == 0) master = atoi(argv[k + 1]) != 0;
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
        if (mp4 && rc7 == 0)
            rc7 = an_scene_export(S3, &(an_export_desc){ .path = mp4, .quality = master ? AN_EXPORT_MASTER : AN_EXPORT_SHARE }) > 0 ? 0 : 1;
        printf("chapter 7 range: frames 0..%lld\n", (long long)an_scene_length(S3));
        if (!dir && !mp4) an_scene_preview(S3);
        an_scene_destroy(S3);
        return rc7;
    }
    int64_t L = 0;
    int rc = 0;
    if (chapter >= CH_COLD && chapter <= CH_LAST) { // the new sections: their own scenes
        new_durations();
        const bool tag = chapter == CH_CLOCK;
        S = an_scene_create(&(an_scene_desc){ .width = tag ? 300 : 1920, .height = tag ? 110 : 1080, .fps = FPS, .seed = 12 + chapter,
                                              .style = { .grain_mode = AN_GRAIN_STATIC, .background = tag ? AN_BG_FLAT : AN_BG_PAPER } });
        if (!S) return 1;
        P = NULL;
        switch (chapter) {
        case CH_COLD: ch_cold_open(); break;
        case CH_HAND: ch_by_hand(); break;
        case CH_COORD_A: ch_coord_a(); break;
        case CH_COORD_B: ch_coord_b(); break;
        case CH_FIB: ch_fib(); break;
        case CH_GOOGLE: ch_google(); break;
        case CH_MUSIC: ch_music(); break;
        case CH_DATA: ch_data(); break;
        case CH_OUTRO: ch_outro(); break;
        case CH_STING: ch_stinger(NEW_DUR[CH_STING - CH_COLD]); break;
        case CH_RECAP_T: sting_title = "RECAP"; ch_stinger(NEW_DUR[CH_RECAP_T - CH_COLD]); break;
        default: ch_clock(); break;
        }
        printf("chapter %d: %.3f s (planned %.3f), frames 0..%lld\n", chapter, an_scene_time(S), (double)NEW_DUR[chapter - CH_COLD],
               (long long)an_scene_length(S));
        L = an_scene_length(S);
        goto output;
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

    L = an_scene_length(S);
output:
    if (dir) {
        const int64_t n = an_scene_save_frames(S, &(an_frames_desc){ .dir = dir, .every = every, .prefix = nm("ch%d", chapter) });
        if (n >= 0) printf("wrote %lld PNG frames (0..%lld, every %d) to %s\n", (long long)n, (long long)L, every, dir);
        rc = n >= 0 ? 0 : 1;
    }
    printf("chapter %d range: frames 0..%lld\n", chapter, (long long)L);
    if (seams && rc == 0) rc = save_seams(S, seams, chapter, 0, L);
    if (mp4 && rc == 0)
        rc = an_scene_export(S, &(an_export_desc){ .path = mp4, .quality = master ? AN_EXPORT_MASTER : AN_EXPORT_SHARE }) > 0 ? 0 : 1;
    if (!dir && !mp4) an_scene_preview(S);
    an_scene_destroy(S);
    return rc;
}
