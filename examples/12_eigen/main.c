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
static const bool BUILT[NCH] = { true, false, false, false, false, false, false };

static const float PI = 3.14159265358979f;
#define MAGENTA AN_HEX(0xE6007E)
#define CYAN AN_HEX(0x00B4D8)
#define CHARCOAL AN_HEX(0x16161A)
#define ORANGE AN_HEX(0xFF6A00)

static const an_mat2 A = { 3, 1, 0, 2 }; // eigenvectors (1, 0) [3] and (1, -1) [2]

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

    void (*const chapters[NCH])(void) = { chapter0, NULL, NULL, NULL, NULL, NULL, NULL };
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
