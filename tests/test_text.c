// Text (docs/text.md): layout, kerning, glyph fills (stencil-then-cover,
// non-zero), winding overflow, per-letter transforms, snapping, draw-on,
// stagger, the code block, the size factor, and the shadow cut on text.
// Real Metal output (4x MSAA, readback).
//
// Every expected value comes from the fonts' own tables, read here by a small
// reader of our own (head, hhea, hmtx, cmap format 4, glyf; independent of
// stb_truetype and of src/text.c), and is worked out by hand in the comments.
// Kerning comes from stb's raw stbtt_GetGlyphKernAdvance (kern + GPOS).
//
// Font facts used below (font units, upem 1000 for both):
//   Archivo Black: ascent 878, descent -210, gap 0
//     H: advance 833, one contour, stems x 74..295 and 538..759, crossbar y 261..437, top 688
//     A: advance 778.  V: advance 778, left edge from (17, 688) down to (261, 0)
//     I: advance 389, a rectangle x 84..305, y 0..688 (perimeter 1818)
//     O: advance 833, outer x 45..788; counter x 271..562 (flat at y 312..376)
//     o: advance 667, outer x 36..631, y -12..540; counter x 235..432, y 123..405
//     x: top 528 (x-height).  kern A-V = -55
//   JetBrains Mono: ascent 1020, descent -300, gap 0; every advance 600; H stem 93..183
//
// paper #F3EDE2 = (243, 237, 226) (palette riot); fills here: black, blue
// #0078BF = (0, 120, 191), magenta #FF2E88 = (255, 46, 136); shadow #3255A4 =
// (50, 85, 164); lime #C6FF00 = (198, 255, 0) (riot's accent c, the highlight).
#include "anown.h"
#include "check.h"
#include "font_internal.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"
#include "text_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 640, H = 400 };

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
static bool near(int x, int y, double r, double g, double b, double tol) {
    const unsigned char *p = at(x, y);
    const bool ok = fabs(p[0] - r) <= tol && fabs(p[1] - g) <= tol && fabs(p[2] - b) <= tol;
    if (!ok) fprintf(stderr, "  (%d,%d) = %d %d %d, want %.1f %.1f %.1f\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}
#define PAPER(x, y) is(x, y, 243, 237, 226)
#define BLACK(x, y) is(x, y, 0, 0, 0)
#define BLUE(x, y) is(x, y, 0, 120, 191)
#define SHADOW(x, y) is(x, y, 50, 85, 164)
#define CHARCOAL(x, y) is(x, y, 22, 22, 26)

// no grain, vignette or boil (exact pixels); chaos 1 unless a test sets it
static an_scene *scene(bool shadow) {
    return an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 3,
                                             .style = { .grain_off = true, .vignette_off = true, .boil_off = true,
                                                        .shadow = shadow } });
}

// ---- an independent table reader ---------------------------------------------------------

typedef struct ttf {
    unsigned char *d;
    size_t n;
    uint32_t head, hhea, hmtx, cmap, loca, glyf;
} ttf;

static unsigned u16(const ttf *f, size_t o) { return (unsigned)f->d[o] << 8 | f->d[o + 1]; }
static int s16(const ttf *f, size_t o) { return (int16_t)u16(f, o); }
static uint32_t u32(const ttf *f, size_t o) { return (uint32_t)u16(f, o) << 16 | u16(f, o + 2); }

static bool ttf_open(ttf *f, const char *path) {
    *f = (ttf){ 0 };
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    f->n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->d = malloc(f->n);
    const bool ok = f->d && fread(f->d, 1, f->n, fp) == f->n;
    fclose(fp);
    if (!ok) return false;
    for (unsigned i = 0; i < u16(f, 4); i++) {
        const size_t r = 12 + 16 * (size_t)i;
        const uint32_t off = u32(f, r + 8);
        if (!memcmp(f->d + r, "head", 4)) f->head = off;
        if (!memcmp(f->d + r, "hhea", 4)) f->hhea = off;
        if (!memcmp(f->d + r, "hmtx", 4)) f->hmtx = off;
        if (!memcmp(f->d + r, "cmap", 4)) f->cmap = off;
        if (!memcmp(f->d + r, "loca", 4)) f->loca = off;
        if (!memcmp(f->d + r, "glyf", 4)) f->glyf = off;
    }
    return f->head && f->hhea && f->hmtx && f->cmap && f->loca && f->glyf;
}

static int ttf_upem(const ttf *f) { return (int)u16(f, f->head + 18); }
static int ttf_ascent(const ttf *f) { return s16(f, f->hhea + 4); }
static int ttf_descent(const ttf *f) { return s16(f, f->hhea + 6); }
static int ttf_gap(const ttf *f) { return s16(f, f->hhea + 8); }

// cmap format 4 (platform 3, encoding 1)
static int ttf_glyph(const ttf *f, unsigned c) {
    size_t sub = 0;
    for (unsigned i = 0; i < u16(f, f->cmap + 2); i++) {
        const size_t r = f->cmap + 4 + 8 * (size_t)i;
        if (u16(f, r) == 3 && u16(f, r + 2) == 1) sub = f->cmap + u32(f, r + 4);
    }
    if (!sub || u16(f, sub) != 4) return 0;
    const unsigned seg2 = u16(f, sub + 6);
    const size_t ends = sub + 14, starts = ends + seg2 + 2, deltas = starts + seg2, ranges = deltas + seg2;
    for (unsigned i = 0; i < seg2 / 2; i++) {
        if (c > u16(f, ends + 2 * i) || c < u16(f, starts + 2 * i)) continue;
        const unsigned ro = u16(f, ranges + 2 * i);
        if (!ro) return (int)((c + (unsigned)s16(f, deltas + 2 * i)) & 0xFFFF);
        const unsigned g = u16(f, ranges + 2 * i + ro + 2 * (c - u16(f, starts + 2 * i)));
        return g ? (int)((g + (unsigned)s16(f, deltas + 2 * i)) & 0xFFFF) : 0;
    }
    return 0;
}

static int ttf_advance(const ttf *f, int g) {
    const unsigned nh = u16(f, f->hhea + 34);
    return (int)u16(f, f->hmtx + 4 * (size_t)((unsigned)g < nh ? (unsigned)g : nh - 1));
}

// A simple glyph's points (font units, y up): count, and the first `max` points.
static int ttf_points(const ttf *f, int g, int *xs, int *ys, int max, int *ncontours) {
    const bool long_loca = s16(f, f->head + 50) != 0;
    const size_t off = f->glyf + (long_loca ? u32(f, f->loca + 4 * (size_t)g) : 2 * (size_t)u16(f, f->loca + 2 * (size_t)g));
    const int nc = s16(f, off);
    *ncontours = nc;
    if (nc <= 0) return 0;
    const int npts = (int)u16(f, off + 10 + 2 * (size_t)(nc - 1)) + 1;
    size_t p = off + 10 + 2 * (size_t)nc;
    p += 2 + u16(f, p);
    unsigned char *fl = malloc((size_t)npts);
    for (int i = 0; i < npts;) {
        const unsigned char b = f->d[p++];
        fl[i++] = b;
        if (b & 8) for (int r = f->d[p++]; r > 0 && i < npts; r--) fl[i++] = b;
    }
    int v = 0;
    for (int i = 0; i < npts; i++) { // x: short (bit 1, sign bit 4) or same/long
        if (fl[i] & 2) v += (fl[i] & 16) ? f->d[p++] : -f->d[p++];
        else if (!(fl[i] & 16)) { v += s16(f, p); p += 2; }
        if (i < max) xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) { // y: bits 2 and 5
        if (fl[i] & 4) v += (fl[i] & 32) ? f->d[p++] : -f->d[p++];
        else if (!(fl[i] & 32)) { v += s16(f, p); p += 2; }
        if (i < max) ys[i] = v;
    }
    free(fl);
    return npts;
}

static ttf g_archivo, g_mono;

// The tables say what the comments above say (so the hand computations below
// rest on the file we ship).
static void test_font_tables(void) {
    CHECK(ttf_open(&g_archivo, "assets/fonts/ArchivoBlack-Regular.ttf"));
    CHECK(ttf_open(&g_mono, "assets/fonts/JetBrainsMono-Regular.ttf"));
    const ttf *a = &g_archivo, *m = &g_mono;
    CHECK(ttf_upem(a) == 1000 && ttf_ascent(a) == 878 && ttf_descent(a) == -210 && ttf_gap(a) == 0);
    CHECK(ttf_upem(m) == 1000 && ttf_ascent(m) == 1020 && ttf_descent(m) == -300 && ttf_gap(m) == 0);
    CHECK(ttf_advance(a, ttf_glyph(a, 'H')) == 833 && ttf_advance(a, ttf_glyph(a, 'A')) == 778 &&
          ttf_advance(a, ttf_glyph(a, 'V')) == 778 && ttf_advance(a, ttf_glyph(a, 'I')) == 389);
    CHECK(ttf_advance(m, ttf_glyph(m, 'H')) == 600 && ttf_advance(m, ttf_glyph(m, '0')) == 600);
    int xs[16], ys[16], nc;
    // H: 12 points, stems 74..295 and 538..759, crossbar 261..437
    CHECK(ttf_points(a, ttf_glyph(a, 'H'), xs, ys, 16, &nc) == 12 && nc == 1);
    CHECK(xs[4] == 74 && xs[3] == 295 && xs[0] == 538 && xs[11] == 759 && ys[1] == 261 && ys[7] == 437 && ys[5] == 688);
    // V's left edge: (17, 688) -> (261, 0)
    CHECK(ttf_points(a, ttf_glyph(a, 'V'), xs, ys, 16, &nc) == 8 && xs[1] == 17 && ys[1] == 688 && xs[0] == 261 && ys[0] == 0);
    // I: the rectangle 84..305 x 0..688
    CHECK(ttf_points(a, ttf_glyph(a, 'I'), xs, ys, 16, &nc) == 4 && nc == 1);
    CHECK(xs[0] == 84 && ys[0] == 0 && xs[1] == 84 && ys[1] == 688 && xs[2] == 305 && xs[3] == 305);
    // mono H stem 93..183 (the size factor's stem): the outline's two leftmost verticals
    an_scene *s = scene(false);
    CHECK(an_font_mono(s)->stem == 90 && an_font_display(s)->stem == 221);
    CHECK(an_font_display(s)->x_height == 528);
    an_scene_destroy(s);
}

// ---- layout --------------------------------------------------------------------------------

static float pen_x(const an_obj *t, int i) { return an_text_glyph(t, i)->u.glyph.pen.x; }
static float pen_y(const an_obj *t, int i) { return an_text_glyph(t, i)->u.glyph.pen.y; }

static void test_layout(void) {
    an_scene *s = scene(false);
    an_font *mono = an_font_mono(s), *arch = an_font_display(s);
    // "HHHH" at 30 px: advances 600 x 30 / 1000 = 18 px; width 72
    an_obj *t = an_text(s, &(an_text_opts){ .text = "HHHH", .font = mono, .size = 30 });
    CHECK(an_text_length(t) == 4);
    for (int i = 0; i < 4; i++) CHECK_NEAR(pen_x(t, i), 18.0 * i, 1e-4);
    CHECK_NEAR(pen_x(t, 3) + 18.0, 72.0, 1e-4);
    // spaces and line breaks have no letter
    CHECK(an_text_length(an_text(s, &(an_text_opts){ .text = "a b\n c", .font = mono })) == 3);
    // line height at 28 px: (1020 + 300 + 0) x 0.028 = 36.96
    t = an_text(s, &(an_text_opts){ .text = "H\nH", .font = mono, .size = 28 });
    CHECK_NEAR(pen_y(t, 0), 0.0, 1e-5);
    CHECK_NEAR(pen_y(t, 1), 36.96, 1e-4);
    CHECK_NEAR(pen_x(t, 1), 0.0, 1e-5);
    // alignment ("HH" is 36 px wide at 30 px): centered starts at -18, right at -36
    CHECK_NEAR(pen_x(an_text(s, &(an_text_opts){ .text = "HH", .font = mono, .size = 30, .align = AN_ALIGN_CENTER }), 0), -18.0, 1e-4);
    CHECK_NEAR(pen_x(an_text(s, &(an_text_opts){ .text = "HH", .font = mono, .size = 30, .align = AN_ALIGN_RIGHT }), 0), -36.0, 1e-4);
    // per line: "HHHH\nHH" centered: line 0 from -36, line 1 from -18
    t = an_text(s, &(an_text_opts){ .text = "HHHH\nHH", .font = mono, .size = 30, .align = AN_ALIGN_CENTER });
    CHECK_NEAR(pen_x(t, 0), -36.0, 1e-4);
    CHECK_NEAR(pen_x(t, 4), -18.0, 1e-4);
    // anchors at 30 px (ascent 30.6, descent 9.0, line 39.6):
    //   top: baseline at +30.6; bottom of 2 lines: last baseline at -9, first at -48.6;
    //   middle of 1 line: block -30.6..+9, center -10.8, so the baseline is at +10.8
    CHECK_NEAR(pen_y(an_text(s, &(an_text_opts){ .text = "H", .font = mono, .size = 30, .anchor = AN_ANCHOR_TOP }), 0), 30.6, 1e-4);
    CHECK_NEAR(pen_y(an_text(s, &(an_text_opts){ .text = "H\nH", .font = mono, .size = 30, .anchor = AN_ANCHOR_BOTTOM }), 0), -48.6, 1e-4);
    CHECK_NEAR(pen_y(an_text(s, &(an_text_opts){ .text = "H", .font = mono, .size = 30, .anchor = AN_ANCHOR_MIDDLE }), 0), 10.8, 1e-4);
    // kerning: stb's raw value for A-V is -55; at 120 px V's pen is (778 - 55) x 0.12 = 86.76,
    // and without kerning 778 x 0.12 = 93.36
    stbtt_fontinfo fi;
    CHECK(stbtt_InitFont(&fi, g_archivo.d, 0));
    CHECK(stbtt_GetGlyphKernAdvance(&fi, ttf_glyph(&g_archivo, 'A'), ttf_glyph(&g_archivo, 'V')) == -55);
    t = an_text(s, &(an_text_opts){ .text = "AV", .font = arch, .size = 120 });
    CHECK_NEAR(pen_x(t, 1), 86.76, 1e-3);
    t = an_text(s, &(an_text_opts){ .text = "AV", .font = arch, .size = 120, .kerning = AN_OFF });
    CHECK_NEAR(pen_x(t, 1), 93.36, 1e-3);
    // the pivot: middle of the advance at half the x-height, (416.5 x 0.12, -264 x 0.12)
    t = an_text(s, &(an_text_opts){ .text = "H", .font = arch, .size = 120, .pos = { 10, 20 } });
    CHECK_NEAR(an_text_glyph(t, 0)->u.glyph.pivot.x, 49.98, 1e-3);
    CHECK_NEAR(an_text_glyph(t, 0)->u.glyph.pivot.y, -31.68, 1e-3);
    CHECK_NEAR(an_text_glyph(t, 0)->pos.x, 49.98, 1e-3); // pos = pen + pivot (in the text's space)
    an_scene_destroy(s);
}

// ---- glyph fills -------------------------------------------------------------------------

// Archivo "H" at 120 px (k = 0.12), pen (100, 300), black, chaos 0.
//   stems  x 100 + 74k = 108.88 .. 135.40   and   164.56 .. 191.08
//   top    y 300 - 688k = 217.44;  crossbar y 300 - 437k = 247.56 .. 300 - 261k = 268.68
// Columns 109..134 and 165..190 are fully inside the stems (column c covers
// [c, c+1)); 136..163 are the gap; 107 and 193 are outside.
static void test_h(void) {
    an_scene *s = scene(false);
    an_set_chaos(s, 0.0f);
    an_text(s, &(an_text_opts){ .text = "H", .size = 120, .pos = { 100, 300 } });
    render(s, 0);
    CHECK(BLACK(109, 250) && BLACK(134, 250) && BLACK(165, 250) && BLACK(190, 250));
    CHECK(BLACK(120, 218) && BLACK(120, 299) && PAPER(120, 216) && PAPER(120, 300));
    CHECK(PAPER(107, 250) && PAPER(193, 250));
    CHECK(PAPER(136, 230) && PAPER(163, 230) && PAPER(150, 246)); // above the crossbar
    CHECK(BLACK(136, 248) && BLACK(163, 267) && BLACK(150, 258));  // the crossbar
    CHECK(PAPER(150, 270) && PAPER(136, 299));                     // below it
    an_scene_destroy(s);

    // "O": the counter is a hole (the fill follows the non-zero rule).
    //   outer x 100 + 45k = 105.4, counter x 132.52 .. 167.44 at y 300 - 344k = 258.72
    s = scene(false);
    an_set_chaos(s, 0.0f);
    an_text(s, &(an_text_opts){ .text = "O", .size = 120, .pos = { 100, 300 } });
    render(s, 0);
    CHECK(PAPER(149, 258) && PAPER(135, 258) && PAPER(165, 258));
    CHECK(BLACK(115, 258) && BLACK(106, 258) && BLACK(185, 258));
    an_scene_destroy(s);
}

// V's left edge in row y, measured from red coverage: the first partly red column
// c with coverage f puts the edge at c + 1 - f. Coverage from green (paper 237,
// red 0). 4x MSAA quantizes f to quarters, so this is good to about 0.25 px.
static double red_edge(int y, int x0, int x1) {
    for (int x = x0; x < x1; x++) {
        const unsigned char *p = at(x, y);
        if (p[0] > p[1] + 8) { // reddish: partly or fully red
            const double f = 1.0 - p[1] / 237.0;
            return x + 1.0 - f;
        }
    }
    return -1;
}

// Kerning in pixels: "AV" at 120 px, pen (100, 300), V red. V's left edge runs
// from (17, 688) to (261, 0): at row y (center y + 0.5), in units
//   yu = (300 - y - 0.5) / 0.12,   xu = 17 + (688 - yu) x 244 / 688
// and on screen x = 100 + penV + xu x 0.12. The layout pen is 86.76 kerned and
// 93.36 not; at chaos 0 letters snap to whole pixels from the text's origin, so
// penV is 87 kerned and 93 not (6 px apart).
// Rows 222..230 are near the top, where A (black, its apex far to the left) is
// nowhere near V. The checks must fail when the kerning table is ignored: the
// unkerned render is checked against the kerned expectation and must miss it.
static double v_edge_expected(int y, double pen_v) {
    const double yu = (300.0 - y - 0.5) / 0.12;
    return 100.0 + pen_v + (17.0 + (688.0 - yu) * 244.0 / 688.0) * 0.12;
}

static void test_kerning_pixels(void) {
    for (int kern = 1; kern >= 0; kern--) {
        an_scene *s = scene(false);
        an_set_chaos(s, 0.0f);
        an_obj *t = an_text(s, &(an_text_opts){ .text = "AV", .size = 120, .pos = { 100, 300 },
                                                 .kerning = kern ? AN_AUTO : AN_OFF });
        an_obj_set_fill(an_text_glyph(t, 1), AN_HEX(0xFF0000));
        render(s, 0);
        int hits_kerned = 0, hits_unkerned = 0;
        for (int y = 222; y <= 230; y += 4) {
            const double e = red_edge(y, 150, 260);
            hits_kerned += fabs(e - v_edge_expected(y, 87.0)) <= 0.4;
            hits_unkerned += fabs(e - v_edge_expected(y, 93.0)) <= 0.4;
            const double want = v_edge_expected(y, kern ? 87.0 : 93.0);
            if (fabs(e - want) > 0.4) fprintf(stderr, "  row %d: V edge %.2f, want %.2f\n", y, e, want);
        }
        if (kern) CHECK(hits_kerned == 3 && hits_unkerned == 0);
        else CHECK(hits_kerned == 0 && hits_unkerned == 3); // ignoring the table fails the kerned checks
        an_scene_destroy(s);
    }
}

// Overlaps: two "I"s of one text (one batch) overlapping. "I" at 120 px is the
// rectangle x 100 + 84k = 110.08 .. 136.60 (pen 100), y 217.44 .. 300.
// Letter 1's layout pen is 46.68; it's moved to pos = letter 0's pos + (10, 0),
// and calm letters snap their layout pen (46.68 -> 47, +0.32), so letter 1 spans
// x 120.40 .. 146.92. The overlap 120.40 .. 136.60 has winding 2: non-zero fills
// it (even-odd would leave a hole).
static void test_overlap(void) {
    an_scene *s = scene(false);
    an_set_chaos(s, 0.0f);
    an_obj *t = an_text(s, &(an_text_opts){ .text = "II", .size = 120, .pos = { 100, 300 } });
    an_obj *g0 = an_text_glyph(t, 0), *g1 = an_text_glyph(t, 1);
    an_obj_set_pos(g1, (an_vec2){ g0->pos.x + 10.0f, g0->pos.y });
    render(s, 0);
    CHECK(BLACK(112, 260) && BLACK(128, 260) && BLACK(145, 260) && PAPER(148, 260) && PAPER(108, 260));
    an_scene_destroy(s);

    // Translucent letters (fill alpha 0.5): the text is isolated as a whole, and
    // the cover paints each sample once, so the overlap is no darker:
    //   0.5 black over paper = (121.5, 118.5, 113)
    s = scene(false);
    an_set_chaos(s, 0.0f);
    t = an_text(s, &(an_text_opts){ .text = "II", .size = 120, .pos = { 100, 300 }, .fill = { 0, 0, 0, 0.5f } });
    g0 = an_text_glyph(t, 0);
    an_obj_set_pos(an_text_glyph(t, 1), (an_vec2){ g0->pos.x + 10.0f, g0->pos.y });
    render(s, 0);
    CHECK(near(112, 260, 121.5, 118.5, 113, 2) && near(128, 260, 121.5, 118.5, 113, 2) && near(145, 260, 121.5, 118.5, 113, 2));
    CHECK(g_r.layers_rendered == 1);
    an_scene_destroy(s);
}

// Winding overflow (docs/text.md, problem 1): the counter is 4 bits, mod 16.
// n copies of "I" stacked on one spot give winding n inside: 15 and 17 read as
// nonzero (filled), 16 reads as 0 (that spot stays paper). Stacking: every
// letter's pos is set to letter 0's; snapping moves each by under half a pixel,
// so (123, 260), 13 px inside every copy, is covered n times.
static void test_winding_overflow(void) {
    for (int n = 15; n <= 17; n++) {
        char str[32];
        memset(str, 'I', (size_t)n);
        str[n] = 0;
        an_scene *s = scene(false);
        an_set_chaos(s, 0.0f);
        an_obj *t = an_text(s, &(an_text_opts){ .text = str, .size = 120, .pos = { 100, 300 } });
        for (int i = 1; i < n; i++) an_obj_set_pos(an_text_glyph(t, i), an_text_glyph(t, 0)->pos);
        render(s, 0);
        if (n == 16) CHECK(PAPER(123, 260));
        else CHECK(BLACK(123, 260));
        CHECK(PAPER(108, 260)); // outside every copy
        an_scene_destroy(s);
    }
}

// ---- per-letter transforms -----------------------------------------------------------------

// "I I I" at 120 px, pen (100, 300). Letter 1 is rotated 90 degrees about its
// pivot: (advance/2, -x-height/2) = (23.34, -31.68) from its pen. Relative to
// the pivot the "I" is x -13.26..13.26, y -50.88..31.68; rotated clockwise
// ((x, y) -> (-y, x)) it becomes x -31.68..50.88, y -13.26..13.26.
// Its neighbors don't move: their pixels match an unrotated render.
static void test_rotate_letter(void) {
    unsigned char *base = malloc((size_t)W * H * 4);
    float pivot_x = 0, pivot_y = 0, left_end = 0, right_start = 0;
    for (int rot = 0; rot < 2; rot++) {
        an_scene *s = scene(false);
        an_set_chaos(s, 0.0f);
        an_obj *t = an_text(s, &(an_text_opts){ .text = "I I I", .size = 120, .pos = { 100, 300 } });
        an_obj *g = an_text_glyph(t, 1);
        pivot_x = 100.0f + g->pos.x; // pos = pen + pivot
        pivot_y = 300.0f + g->pos.y;
        left_end = 100.0f + an_text_glyph(t, 0)->u.glyph.pen.x + 36.6f + 1.0f;
        right_start = 100.0f + an_text_glyph(t, 2)->u.glyph.pen.x + 10.08f - 1.0f;
        if (rot) an_obj_set_rotation(g, 1.5707963f);
        render(s, 0);
        if (!rot) {
            memcpy(base, g_px, (size_t)W * H * 4);
            CHECK(BLACK((int)pivot_x, (int)pivot_y - 40)); // inside the upright I
        } else {
            CHECK(BLACK((int)(pivot_x + 40), (int)pivot_y));
            CHECK(BLACK((int)(pivot_x - 25), (int)(pivot_y + 10)));
            CHECK(PAPER((int)pivot_x, (int)pivot_y - 40)); // the upright I's top is gone
            // the rotated bar stays clear of the neighbors
            CHECK(pivot_x - 31.68f > left_end && pivot_x + 50.88f < right_start);
            int diff = 0;
            for (int y = 200; y < 310; y++) {
                for (int x = 95; x < (int)left_end; x++) diff += memcmp(at(x, y), base + ((size_t)y * W + x) * 4, 3) != 0;
                for (int x = (int)right_start + 1; x < (int)right_start + 30; x++)
                    diff += memcmp(at(x, y), base + ((size_t)y * W + x) * 4, 3) != 0;
            }
            CHECK(diff == 0);
        }
        an_scene_destroy(s);
    }
    free(base);
}

// Snapping (docs/text.md, problem 2): relative to the text's own origin; only
// for a translation x uniform scale, and an unrotated, uniformly scaled letter.
static void test_snap(void) {
    const an_vec2 pen = { 16.8f, 0.0f }; // the mono's advance at 28 px
    ani_affine w = { 1, 0, 0, 1, 0, 0 };
    // origin 100.37: the pen offset 16.8 becomes 17 (+0.2)
    CHECK(ani_text_snap((ani_affine){ 1, 0, 0, 1, 100.37f, 50 }, &w, pen, 0.0f, (an_vec2){ 1, 1 }));
    CHECK_NEAR(w.tx, 0.2, 1e-5);
    CHECK_NEAR(w.ty, 0.0, 1e-6);
    // uniform scale 1.5: 16.8 x 1.5 = 25.2 -> 25 (-0.2)
    w = (ani_affine){ 1, 0, 0, 1, 0, 0 };
    CHECK(ani_text_snap((ani_affine){ 1.5f, 0, 0, 1.5f, 0, 0 }, &w, pen, 0.0f, (an_vec2){ 1, 1 }));
    CHECK_NEAR(w.tx, -0.2, 1e-5);
    // no snapping under rotation or non-uniform scale (text or letter)
    w = (ani_affine){ 1, 0, 0, 1, 0, 0 };
    CHECK(!ani_text_snap((ani_affine){ 0.995f, 0.0998f, -0.0998f, 0.995f, 0, 0 }, &w, pen, 0.0f, (an_vec2){ 1, 1 }));
    CHECK(!ani_text_snap((ani_affine){ 2, 0, 0, 1, 0, 0 }, &w, pen, 0.0f, (an_vec2){ 1, 1 }));
    CHECK(!ani_text_snap((ani_affine){ 1, 0, 0, 1, 0, 0 }, &w, pen, 0.1f, (an_vec2){ 1, 1 }));
    CHECK(!ani_text_snap((ani_affine){ 1, 0, 0, 1, 0, 0 }, &w, pen, 0.0f, (an_vec2){ 1, 2 }));
    CHECK(w.tx == 0.0f && w.ty == 0.0f);

    // In pixels: "HH" in the mono at 28 px (advance 16.8), origin x = 100.37.
    // Snapped, letter 1 sits exactly 17 px right of letter 0, so the two letters
    // are pixel-identical (unsnapped, their sub-pixel phases would differ by 0.8).
    an_scene *s = scene(false);
    an_set_chaos(s, 0.0f);
    an_text(s, &(an_text_opts){ .text = "HH", .font = an_font_mono(s), .size = 28, .pos = { 100.37f, 200 } });
    render(s, 0);
    int diff = 0, ink = 0;
    for (int y = 175; y < 202; y++) {
        for (int x = 100; x < 117; x++) {
            diff += memcmp(at(x, y), at(x + 17, y), 3) != 0;
            ink += at(x, y)[0] < 128;
        }
    }
    CHECK(diff == 0 && ink > 40);
    an_scene_destroy(s);
}

// ---- draw-on, typewriter, stagger ---------------------------------------------------------

// "III" at 120 px, pen (100, 300), blue fill, black outline (width 4), drawn on
// over 1 s, linear, at 60 fps. Each "I" outline is the rectangle's perimeter,
// 1818 units = 218.16 px; total 654.48. Letter k is complete when progress
// D = f / 60 reaches (k + 1) / 3: frame 20 for letter 0, 40 for letter 1. Its
// fill pops in then. Letter centers: pen + (194.5k', -344k') = pen + (23.34, -41.28),
// pens 0, 46.68 (snapped to 47), 93.36 (93). Checked a frame either side.
static void test_draw_on(void) {
    an_scene *s = scene(false);
    an_set_chaos(s, 0.0f);
    an_obj *t = an_text(s, &(an_text_opts){ .text = "III", .size = 120, .pos = { 100, 300 }, .undrawn = true,
                                             .fill = AN_HEX(0x0078BF), .stroke = AN_BLACK, .stroke_width = 4 });
    an_play(s, an_draw(t, &(an_anim_opts){ .duration = 1.0f, .ease = AN_EASE_LINEAR }));
    const int cx0 = 123, cx1 = 123 + 47, cy = 258;
    render(s, 0);
    CHECK(PAPER(cx0, cy) && PAPER(110, 260));          // nothing yet
    render(s, 19);
    CHECK(PAPER(cx0, cy));                             // outline almost closed, no fill
    render(s, 21);
    CHECK(BLUE(cx0, cy) && PAPER(cx1, cy));            // letter 0 filled, letter 1 not
    render(s, 39);
    CHECK(PAPER(cx1, cy));
    render(s, 41);
    CHECK(BLUE(cx1, cy));
    an_scene_destroy(s);

    // typewriter (no outline): letter k appears when D x 3 >= k + 1
    s = scene(false);
    an_set_chaos(s, 0.0f);
    t = an_text(s, &(an_text_opts){ .text = "III", .size = 120, .pos = { 100, 300 }, .undrawn = true,
                                     .fill = AN_HEX(0x0078BF) });
    an_play(s, an_draw(t, &(an_anim_opts){ .duration = 1.0f, .ease = AN_EASE_LINEAR }));
    render(s, 19);
    CHECK(PAPER(cx0, cy));
    render(s, 21);
    CHECK(BLUE(cx0, cy) && PAPER(cx1, cy));
    render(s, 41);
    CHECK(BLUE(cx1, cy));
    an_scene_destroy(s);

    // per-letter stagger: letters start hidden; fades of 0.5 s, 0.25 s apart.
    // Letter 1 fades over frames 15..45: frame 14 paper, frame 30 half
    // (isolated layer at 0.5: (121.5, 178.5, 208.5)), frame 45 blue.
    s = scene(false);
    an_set_chaos(s, 0.0f);
    t = an_text(s, &(an_text_opts){ .text = "III", .size = 120, .pos = { 100, 300 }, .letters_hidden = true,
                                     .fill = AN_HEX(0x0078BF) });
    an_anim fades[3];
    for (int i = 0; i < 3; i++)
        fades[i] = an_fade_to(an_text_glyph(t, i), 1.0f, &(an_anim_opts){ .duration = 0.5f, .ease = AN_EASE_LINEAR });
    an_stagger(s, 3, fades, 0.25f);
    render(s, 14);
    CHECK(PAPER(cx1, cy));
    render(s, 30);
    CHECK(near(cx1, cy, 121.5, 178.5, 208.5, 2));
    render(s, 45);
    CHECK(BLUE(cx1, cy) && BLUE(cx0, cy));
    an_scene_destroy(s);
}

// ---- code block ----------------------------------------------------------------------------

// an_code at pos (40, 40), mono 30 px: advance 18, pitch round(39.6) = 40,
// pad x 18, pad y 20, first baseline 20 + round(30.6) = 51 (local).
// Lines 9..11, so 2 digits: gutter (2 + 2) x 18 = 72, code from x 18 + 72 = 90.
// Numbers are right-aligned at 18 + 2 x 18 = 54: "9" and "10" end there.
// Line 10's bar: baseline 51 + 40 = 91 -> 131 on screen; the line box is
// 131 - 30.6 .. 131 + 9 = 100.4 .. 140.0, so rows 101..139 are lime, 100 is
// half lime (see below) and 99 and 140 are the box's paper. Column 170: the bar spans
// x 90 - 9 .. 90 + 36 + 9 = 81..135 (+40: 121..175); "cd" ends at 126 + 40.
static void test_code(void) {
    an_scene *s = scene(false);
    an_set_chaos(s, 0.0f);
    an_obj *c = an_code(s, &(an_code_opts){ .pos = { 40, 40 }, .code = "ab\ncd\nef", .line_numbers = true, .first_line = 9 });
    CHECK(c && an_obj_kind(c) == AN_KIND_CODE && an_code_line(c, 8) == NULL && an_code_line(c, 12) == NULL);
    CHECK(an_text_length(an_code_text(c, 10)) == 2);
    // the numbers: right edges at 54 (pen of the last digit + 18)
    int found = 0;
    for (an_obj *o = c->first_child; o; o = o->next_sibling) {
        if (o->kind != AN_KIND_TEXT || !(strcmp(o->u.text.str, "9") == 0 || strcmp(o->u.text.str, "10") == 0)) continue;
        const int n = an_text_length(o);
        CHECK_NEAR(o->pos.x + an_text_glyph(o, n - 1)->u.glyph.pen.x + 18.0f, 54.0, 1e-4);
        CHECK_NEAR(o->pos.x + an_text_glyph(o, 0)->u.glyph.pen.x, 54.0 - 18.0 * n, 1e-4);
        found++;
    }
    CHECK(found == 2);
    an_obj_set_opacity(an_code_line(c, 10), 1.0f);
    render(s, 0);
    CHECK(is(170, 101, 198, 255, 0) && is(170, 120, 198, 255, 0) && is(170, 139, 198, 255, 0));
    CHECK(PAPER(170, 99) && PAPER(170, 140));
    // row 100: the bar starts at y 100.4; the 4x MSAA samples sit at y + 0.125,
    // 0.375, 0.625 and 0.875, so 2 of 4 are inside: half lime, half paper
    CHECK(near(170, 100, (198 + 243) / 2.0, (255 + 237) / 2.0, (0 + 226) / 2.0, 1));
    an_scene_destroy(s);
}

// ---- the size factor -------------------------------------------------------------------------

// factor = min(clamp((stem - 3) / 5, 0, 1), 0.3 stem / (2 x 1.5)) at scene chaos 1
static void test_size_factor(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H }); // default style, boil 1.5
    an_font *mono = an_font_mono(s), *arch = an_font_display(s);
    an_obj *m30 = an_text(s, &(an_text_opts){ .text = "x", .font = mono, .size = 30 });   // stem 2.7: 0
    an_obj *a48 = an_text(s, &(an_text_opts){ .text = "x", .font = arch, .size = 48 });   // 10.608: min(1, 1.0608)
    an_obj *a28 = an_text(s, &(an_text_opts){ .text = "x", .font = arch, .size = 28 });   // 6.188: min(0.6376, 0.6188)
    an_obj *m30x4 = an_text(s, &(an_text_opts){ .text = "x", .font = mono, .size = 30, .scale = { 4, 4 } }); // 10.8: 1
    an_obj *off = an_text(s, &(an_text_opts){ .text = "x", .font = mono, .size = 30, .size_chaos = AN_OFF });
    an_obj *code = an_code(s, &(an_code_opts){ .code = "x" });
    an_seek(s, 0);
    CHECK(an_obj_chaos(an_text_glyph(m30, 0)) == 0.0f);
    CHECK_NEAR(an_obj_chaos(an_text_glyph(a48, 0)), 1.0, 1e-6);
    CHECK_NEAR(an_obj_chaos(an_text_glyph(a28, 0)), 0.6188, 1e-4);
    CHECK_NEAR(an_obj_chaos(an_text_glyph(m30x4, 0)), 1.0, 1e-6);
    CHECK_NEAR(an_obj_chaos(an_text_glyph(off, 0)), 1.0, 1e-6);
    CHECK(an_obj_chaos(an_text_glyph(an_code_text(code, 1), 0)) == 0.0f);
    // the code's box keeps full chaos
    an_obj *box = NULL;
    for (an_obj *o = code->first_child; o; o = o->next_sibling) if (o->kind == AN_KIND_RECT && o->z == -2) box = o;
    CHECK(box && an_obj_chaos(box) == 1.0f);
    an_scene_destroy(s);
}

// ---- shadow cut on text ---------------------------------------------------------------------

// Archivo "o" at 200 px (k = 0.2), pen (100, 300), blue halftone (cell 20,
// density 0.5: dot radius sqrt(0.5 / pi) x 20 = 7.98 px), shadow (8, 8), chaos 1,
// no boil. The pattern is anchored at the text's origin: dots at
// (100 + 10 + 20i, 300 + 10 + 20j), gaps at the cell corners (100 + 20i, 300 + 20j).
//   P = (120, 240): a cell corner (13.4 px from the nearest dots) inside the ring
//       (units (102.5, 297.5): the ring spans x 36..235 there). P - (8, 8) is in
//       the ring too, so the shadow covers P's footprint: the cut must show what's
//       under the letter there, not shadow.
//   Q = (230, 246): just right of the ring (units x 652.5 > 631), inside the
//       shadow (Q - (8, 8) is in the ring's right side): shadow.
//   C = (166, 247): the counter's middle (units (332.5, 262.5)); C - (8, 8) is in
//       the counter too: not in the silhouette, not under the shadow.
//   D = (110, 250): a dot center in the ring: blue.
static void test_text_shadow_cut(void) {
    for (int variant = 0; variant < 3; variant++) {
        // 0: on paper. 1: on a charcoal rect. 2: on the rect, with paper_under.
        an_scene *s = scene(true);
        if (variant) an_rect(s, &(an_rect_opts){ .pos = { 170, 250 }, .size = { 200, 200 }, .fill = AN_HEX(0x16161A), .shadow = AN_OFF });
        an_text(s, &(an_text_opts){ .text = "o", .size = 200, .pos = { 100, 300 }, .fill = AN_HEX(0x0078BF),
                                     .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f },
                                     .paper_under = variant == 2 });
        render(s, 0);
        CHECK(BLUE(110, 250));
        if (variant == 0) CHECK(PAPER(120, 240) && SHADOW(230, 246) && PAPER(166, 247));
        if (variant == 1) CHECK(CHARCOAL(120, 240) && CHARCOAL(166, 247));
        if (variant == 2) CHECK(PAPER(120, 240) && CHARCOAL(166, 247)); // the counter isn't silhouette
        an_scene_destroy(s);
    }
}

// Text and shadowed shapes overlapping in ONE layer: a group at opacity 0.5 holds
// a blue rect, a magenta "I" (120 px) and a magenta "o" (200 px), all shadowed,
// shadow offset (20, 0), chaos 1, no boil. Shadows are opaque, so inside the
// layer each sample holds one color and the composite is (c + paper) / 2.
//   rect  x 120..280, y 120..280; its shadow shows in the strip x 280..300
//   "I"   pen (239.92, 232.56): x 250..276.52, y 150..232.56; shadow x 270..296.52
//   "o"   pen (218.3, 302.8): counter x 265.3..304.7 at y 250 (units y 264); ring
//         left x 225.5..265.3; its shadow (ring + 20) covers x 245.5..285.3 there
//   (a) (288, 190): both shadows (the rect's strip and the "I"'s): shadow
//   (b) (277, 190): the "I"'s shadow over the rect's fill: shadow
//   (c) (290, 250): in the "o"'s counter, over the rect's shadow strip; the
//       counter shifted by 20 (x 285.3..324.7) still contains it, so the "o"'s own
//       shadow doesn't reach: the rect's shadow shows through the counter
//   (d) (263, 190): inside the "I": magenta
//   (e) (200, 200): the rect: blue
//   (f) (240, 250): the "o"'s ring over the rect: magenta
//   (g) (270, 250): the "o"'s counter over the rect, under the "o"'s shadow: shadow
//   shadow (50, 85, 164) -> (146.5, 161, 195); magenta (255, 46, 136) -> (249, 141.5, 181);
//   blue (0, 120, 191) -> (121.5, 178.5, 208.5)
static void test_text_in_layer(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 3,
        .style = { .grain_off = true, .vignette_off = true, .boil_off = true, .shadow = true, .shadow_offset = { 20, 0 } } });
    an_obj *g = an_group(s, NULL);
    an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 200, 200 }, .size = { 160, 160 }, .fill = AN_HEX(0x0078BF) });
    an_text(s, &(an_text_opts){ .parent = g, .text = "I", .size = 120, .pos = { 239.92f, 232.56f }, .fill = AN_HEX(0xFF2E88) });
    an_text(s, &(an_text_opts){ .parent = g, .text = "o", .size = 200, .pos = { 218.3f, 302.8f }, .fill = AN_HEX(0xFF2E88) });
    an_obj_set_opacity(g, 0.5f);
    render(s, 0);
    CHECK(near(288, 190, 146.5, 161, 195, 2));
    CHECK(near(277, 190, 146.5, 161, 195, 2));
    CHECK(near(290, 250, 146.5, 161, 195, 2));
    CHECK(near(263, 190, 249, 141.5, 181, 2));
    CHECK(near(200, 200, 121.5, 178.5, 208.5, 2));
    CHECK(near(240, 250, 249, 141.5, 181, 2));
    CHECK(near(270, 250, 146.5, 161, 195, 2));
    CHECK(g_r.layers_rendered == 1);
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_text: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    test_font_tables();
    test_layout();
    test_h();
    test_kerning_pixels();
    test_overlap();
    test_winding_overflow();
    test_rotate_letter();
    test_snap();
    test_draw_on();
    test_code();
    test_size_factor();
    test_text_shadow_cut();
    test_text_in_layer();

    free(g_archivo.d);
    free(g_mono.d);
    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_text");
}
