// Style (docs/layers.md, 5B): riso offset, hard shadow, paper grain at 12 Hz,
// vignette, palettes. Real Metal output (4x MSAA, readback); every expected
// coordinate and color is worked out by hand in the comments.
//
// paper    #F3EDE2 = (243, 237, 226)   (palette riot's paper)
// charcoal #16161A = ( 22,  22,  26)   (palette riot's ink = default shadow color)
// blue     #0078BF = (  0, 120, 191)
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"

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
#define CHARCOAL(x, y) is(x, y, 22, 22, 26)
#define SHADOWBLUE(x, y) is(x, y, 50, 85, 164) // #3255A4, the palettes' default shadow color

static bool near(int x, int y, double r, double g, double b, double tol) {
    const unsigned char *p = at(x, y);
    const bool ok = fabs(p[0] - r) <= tol && fabs(p[1] - g) <= tol && fabs(p[2] - b) <= tol;
    if (!ok) fprintf(stderr, "  (%d,%d) = %d %d %d, want %.2f %.2f %.2f\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}

// effects isolated: no grain, no vignette, no boil (riso/shadow tests); chaos 1
static an_scene *scene(bool shadow_default) {
    return an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 5,
                                             .style = { .grain_off = true, .vignette_off = true, .boil_off = true, .shadow = shadow_default } });
}

// ---- riso ---------------------------------------------------------------------------
// Rect center (100, 80), 60x40, blue fill, charcoal ink width 4, offset (+4, -3).
//   fill    x 70..130 -> 74..134,  y 60..100 -> 57..97
//   ink     outline x = 70 and 130 (+-2): bands 68..72, 128..132; y = 60, 100: 58..62, 98..102
// So: column 133 fill, 134 paper (row 80); columns 72..73 paper between the ink's
// inner edge (72) and the fill (74); row 57 fill, 56 paper (column 100).
static void test_riso(void) {
    an_scene *s = scene(false);
    an_rect(s, &(an_rect_opts){ .pos = { 100, 80 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 4 });
    render(s, 0);
    CHECK(BLUE(133, 80) && PAPER(134, 80));
    CHECK(PAPER(72, 80) && PAPER(73, 80) && BLUE(74, 80));
    CHECK(CHARCOAL(70, 80));
    CHECK(BLUE(100, 57) && PAPER(100, 56));
    an_scene_destroy(s);
}

// Rotated 90 degrees: the offset stays (+4, -3) on screen. The 60x40 rect now
// spans x 80..120, y 50..110; ink bands x 78..82, 118..122; y 48..52, 108..112.
//   fill x 84..124, y 47..107 -> column 123 fill, 124 paper; 82..83 paper; row 47 fill, 46 paper
static void test_riso_rotated(void) {
    an_scene *s = scene(false);
    an_rect(s, &(an_rect_opts){ .pos = { 100, 80 }, .size = { 60, 40 }, .rotation = 1.57079632679f,
                                .fill = AN_HEX(0x0078BF), .stroke = AN_HEX(0x16161A), .stroke_width = 4 });
    render(s, 0);
    CHECK(BLUE(123, 80) && PAPER(124, 80));
    CHECK(PAPER(82, 80) && PAPER(83, 80) && BLUE(84, 80));
    CHECK(BLUE(100, 47) && PAPER(100, 46));
    an_scene_destroy(s);
}

// Fill-only objects aren't offset unless forced; ink + AN_OFF isn't offset;
// an object chaos of 0.5 halves the offset.
//   A: fill-only, center (240, 60), 40x40 -> x 220..260: column 259 blue, 260 paper
//   B: same at (240, 150) with riso AN_ON -> x 224..264, y 127..167
//   C: same at (160, 150), AN_ON, object chaos 0.5 -> offset (+2, -1.5) -> x 142..182
//   D: ink + AN_OFF at (60, 160), 40x40 -> fill x 40..80, y 140..180 (not offset)
static void test_riso_rules(void) {
    an_scene *s = scene(false);
    an_rect(s, &(an_rect_opts){ .pos = { 240, 60 }, .size = { 40, 40 }, .fill = AN_HEX(0x0078BF) });
    an_rect(s, &(an_rect_opts){ .pos = { 240, 150 }, .size = { 40, 40 }, .fill = AN_HEX(0x0078BF), .riso = AN_ON });
    an_obj *c = an_rect(s, &(an_rect_opts){ .pos = { 160, 150 }, .size = { 40, 40 }, .fill = AN_HEX(0x0078BF), .riso = AN_ON });
    an_set_chaos(c, 0.5f);
    an_rect(s, &(an_rect_opts){ .pos = { 60, 160 }, .size = { 40, 40 }, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 4, .riso = AN_OFF });
    render(s, 0);
    CHECK(BLUE(259, 60) && PAPER(260, 60) && PAPER(219, 60) && BLUE(220, 60));  // A: not offset
    CHECK(BLUE(263, 150) && PAPER(264, 150) && PAPER(223, 150) && BLUE(224, 150)); // B: +4
    CHECK(BLUE(181, 150) && PAPER(182, 150) && PAPER(141, 150) && BLUE(142, 150)); // C: +2
    // D: not offset. Ink bands x 38..42 and 78..82, y 138..142. Unoffset fill is
    // x 40..80, y 140..180; offset it would be x 44..84, y 137..177.
    CHECK(BLUE(43, 160));  // fill (offset: paper, fill would start at 44)
    CHECK(PAPER(83, 160)); // paper (offset: fill would reach 84)
    CHECK(PAPER(60, 137)); // paper (offset: fill top would be 137)
    an_scene_destroy(s);

    // riso_off and chaos 0 turn it off
    an_scene *t = an_scene_create(&(an_scene_desc){ .width = W, .height = H,
                                                    .style = { .grain_off = true, .vignette_off = true, .boil_off = true, .riso_off = true } });
    an_rect(t, &(an_rect_opts){ .pos = { 240, 150 }, .size = { 40, 40 }, .fill = AN_HEX(0x0078BF), .riso = AN_ON });
    render(t, 0);
    CHECK(BLUE(259, 150) && PAPER(260, 150));
    an_scene_destroy(t);
}

// ---- shadow -------------------------------------------------------------------------
// Rect center (100, 100), 60x40, blue, no ink (so no riso), shadow (+8, +8) in the default
// shadow color, riso blue #3255A4 = (50, 85, 164). No ink, so no ink clearance either.
//   fill   x 70..130, y 80..120;   shadow x 78..138, y 88..128
// Zero blur: at row 124 (below the fill) column 137 is exactly shadow blue and 138
// exactly paper; at column 110, row 127 shadow and 128 paper; column 78 shadow
// and 77 paper. No intermediate values on pixel-aligned edges.
static void test_shadow(void) {
    an_scene *s = scene(false);
    an_rect(s, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF), .shadow = AN_ON });
    render(s, 0);
    CHECK(SHADOWBLUE(137, 124) && PAPER(138, 124));
    CHECK(SHADOWBLUE(110, 127) && PAPER(110, 128));
    CHECK(SHADOWBLUE(78, 124) && PAPER(77, 124));
    CHECK(BLUE(100, 100)); // the fill is on top of its shadow
    an_scene_destroy(s);

    // scene default on; AN_OFF opts out; AN_ON in a scene whose default is off was above
    an_scene *t = scene(true);
    an_rect(t, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF), .shadow = AN_OFF });
    an_rect(t, &(an_rect_opts){ .pos = { 240, 60 }, .size = { 40, 40 }, .fill = AN_HEX(0x0078BF) });
    render(t, 0);
    CHECK(PAPER(137, 124));                   // opted out
    CHECK(SHADOWBLUE(266, 86) && PAPER(268, 86)); // default: shadow x 228..268, y 48..88
    an_scene_destroy(t);
}

// Shadow = ONE paint: the union of the fill and ink silhouettes. With a shadow
// color at alpha 0.5, where the fill's shadow and the ink's shadow overlap the
// color must be the same as where only one of them is: 0.5 charcoal + 0.5 paper
// = (132.5, 129.5, 126). Rect center (100, 100), 60x40, ink 4, riso (+4, -3).
// The shadow clears the ink: offset = (8, 8) + ink width 4 on each axis = (12, 12).
//   fill shadow:  fill x 74..134, y 77..117 + 12 -> x 86..146, y 89..129
//   ink shadow:   outline x 70, 130 and y 80, 120, + 12, +-2 -> bands x 80..84,
//                 140..144 and y 90..94, 130..134
//   object:       fill x 74..134, y 77..117; ink x 68..132, y 78..122
//   (142, 110): both shadows;  (110, 127): fill shadow only;  (110, 132): ink shadow only
static void test_shadow_union(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H,
        .style = { .grain_off = true, .vignette_off = true, .boil_off = true, .shadow_color = { 22 / 255.0f, 22 / 255.0f, 26 / 255.0f, 0.5f } } });
    an_rect(s, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 4, .shadow = AN_ON });
    render(s, 0);
    CHECK(g_r.mesh.nlayers == 1); // translucent shadow: isolated, stencil-tested
    CHECK(near(142, 110, 132.5, 129.5, 126, 1));
    CHECK(near(110, 127, 132.5, 129.5, 126, 1));
    CHECK(near(110, 132, 132.5, 129.5, 126, 1));
    CHECK(PAPER(147, 110) && PAPER(110, 135)); // just past both shadows
    an_scene_destroy(s);
}

// ---- the shadow cut ---------------------------------------------------------------
// The shadow never shows where the object itself is: its silhouette (the fill where
// it's registered AND where riso moved it, plus the ink) is cut out of its shadow
// with the stencil (docs/layers.md "Shadow cut").
//
// Rect center (100, 80), 60x40, blue, charcoal ink 4, riso (+4, -3), shadow offset
// (1, 1) + ink width 4 = (5, 5), opaque shadow blue:
//   registered fill  x 70..130, y 60..100        riso fill  x 74..134, y 57..97
//   ink bands        x 68..72, 128..132 (y 58..102);  y 58..62, 98..102 (x 68..132)
//   ink shadow       bands + 5: x 73..77, 133..137 (y 63..107);  y 63..67, 103..107
//   fill shadow      riso fill + 5: x 79..139, y 62..102
// The riso sliver inside the outline is in the registered fill but in neither the
// moved fill nor the ink: columns 72..73 at row 80, row 97 at column 100. Column 73
// is under the ink's shadow and row 97 under the fill's shadow; both must be paper.
// Outside the silhouette the shadow shows: row 80, columns 134..138 (139 paper);
// column 100, rows 103..106 (102 is the gap between fill shadow and ink shadow
// bands: paper; 107 paper).
static void test_shadow_cut_riso(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 5,
        .style = { .grain_off = true, .vignette_off = true, .boil_off = true, .shadow_offset = { 1, 1 } } });
    an_rect(s, &(an_rect_opts){ .pos = { 100, 80 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 4, .shadow = AN_ON });
    render(s, 0);
    // the sliver: paper, not shadow
    CHECK(CHARCOAL(71, 80) && PAPER(72, 80) && PAPER(73, 80) && BLUE(74, 80));
    CHECK(BLUE(100, 96) && PAPER(100, 97) && CHARCOAL(100, 98));
    // the crescent outside the silhouette: shadow
    CHECK(BLUE(133, 80) && SHADOWBLUE(134, 80) && SHADOWBLUE(138, 80) && PAPER(139, 80));
    CHECK(CHARCOAL(100, 101) && PAPER(100, 102) && SHADOWBLUE(100, 103) && SHADOWBLUE(100, 106) && PAPER(100, 107));
    an_scene_destroy(s);

    // The moved fill's strip beyond the registered fill and the ink (columns 132..133)
    // is silhouette too. It only shows when the fill has gaps, so: the same rect
    // with a halftone, cell 20, density 0.5 (radius 7.98), anchored to the moved
    // fill (center (104, 77)): dots at x 74, 94, 114, 134 and y 67, 87.
    //   (133, 77): center (133.5, 77.5) is 9.5 px from the dot at (134, 87) and
    //   10.5 from (134, 67): a gap, 1.5 px clear of the dot edge; under both
    //   shadows (fill shadow x 79..139, ink shadow band 133..137): paper.
    //   (134, 77): right of the moved fill, under the shadow: shadow blue.
    an_scene *t = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 5,
        .style = { .grain_off = true, .vignette_off = true, .boil_off = true, .shadow_offset = { 1, 1 } } });
    an_rect(t, &(an_rect_opts){ .pos = { 100, 80 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 4, .shadow = AN_ON,
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
    render(t, 0);
    CHECK(PAPER(133, 77) && SHADOWBLUE(134, 77));
    CHECK(PAPER(73, 80)); // the sliver, halftone or not
    an_scene_destroy(t);
}

// Halftone gaps show what's under the object, not its own shadow. Rect center
// (100, 100), 80x80 -> local -40..40, screen 60..140; fill only (so no riso),
// halftone cell 20, density 0.5: dots at local 20k + 10 -> screen 70, 90, 110, 130,
// radius 7.98. Shadow (8, 8): x 68..148, y 68..148.
//   (110, 110) dot center: blue
//   (100, 100), (80, 120), (139, 139): pixel centers 13.4 px or more from every dot
//     center, 5.4 px outside the dots, inside the fill and the shadow: paper
//   (145, 145), (141, 100): outside the fill, inside the shadow: shadow blue; (148, 100) paper
static void test_shadow_cut_halftone(void) {
    an_scene *s = scene(false);
    an_rect(s, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 80, 80 }, .fill = AN_HEX(0x0078BF), .shadow = AN_ON,
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
    render(s, 0);
    CHECK(BLUE(110, 110));
    CHECK(PAPER(100, 100) && PAPER(80, 120) && PAPER(139, 139));
    CHECK(SHADOWBLUE(145, 145) && SHADOWBLUE(141, 100) && PAPER(148, 100));
    an_scene_destroy(s);
}

// .paper_under: the silhouette is filled with the background color before the
// object's paints, so gaps show paper even over another object. Each case sits
// on a charcoal rect drawn first, so "beneath" (charcoal) and "paper" differ.
//   1. The halftone rect of test_shadow_cut_halftone (screen 60..140, dots at 70,
//      90, 110, 130), no shadow, over charcoal 50..150: gap (100, 100) is charcoal
//      without the flag and paper with it; dot center (110, 110) blue either way;
//      (55, 100), outside the top rect, charcoal either way.
//   2. The riso rect of test_shadow_cut_riso (registered fill x 70..130, moved fill
//      from x 74, ink 68..72), over charcoal 40..160 x 40..120: the sliver (73, 80)
//      is charcoal without the flag and paper with it; (66, 80), left of the ink,
//      charcoal either way.
//   3. Case 1 with shadow on (offset 8: shadow x 68..148): gap (100, 100) is paper,
//      neither shadow nor charcoal; (145, 145), outside the fill, is shadow blue.
static void test_paper_under(void) {
    for (int flag = 0; flag < 2; flag++) {
        an_scene *s = scene(false);
        an_rect(s, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 100, 100 }, .fill = AN_HEX(0x16161A) });
        an_rect(s, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 80, 80 }, .fill = AN_HEX(0x0078BF), .paper_under = flag,
                                    .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
        render(s, 0);
        CHECK(flag ? PAPER(100, 100) : CHARCOAL(100, 100));
        CHECK(BLUE(110, 110) && CHARCOAL(55, 100));
        an_scene_destroy(s);

        an_scene *t = scene(false);
        an_rect(t, &(an_rect_opts){ .pos = { 100, 80 }, .size = { 120, 80 }, .fill = AN_HEX(0x16161A) });
        an_rect(t, &(an_rect_opts){ .pos = { 100, 80 }, .size = { 60, 40 }, .fill = AN_HEX(0x0078BF),
                                    .stroke = AN_HEX(0x16161A), .stroke_width = 4, .paper_under = flag });
        render(t, 0);
        CHECK(flag ? PAPER(73, 80) : CHARCOAL(73, 80));
        CHECK(CHARCOAL(66, 80) && BLUE(74, 80));
        an_scene_destroy(t);
    }
    an_scene *u = scene(false);
    an_rect(u, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 100, 100 }, .fill = AN_HEX(0x16161A) });
    an_rect(u, &(an_rect_opts){ .pos = { 100, 100 }, .size = { 80, 80 }, .fill = AN_HEX(0x0078BF), .paper_under = true,
                                .shadow = AN_ON, .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 20, .density = 0.5f } });
    render(u, 0);
    CHECK(PAPER(100, 100) && SHADOWBLUE(145, 145) && BLUE(110, 110));
    an_scene_destroy(u);
}

// Per MSAA sample, with boil on. No hand pixel list is possible once the outlines
// boil, so the reference is per sample, from three renders of the same geometry:
//   A: the scene;  B: the same scene with shadows off;
//   M: only the silhouette commands, white on black: M = 255 s / 4, s = samples
//      inside the silhouette (0..4).
// Inside the silhouette a sample of A must equal B's (the object, never its
// shadow); outside, B's is paper and A's is paper or shadow. So per pixel:
//   A - B = k (shadow - paper) / 4  with whole k, 0 <= k <= 4 - s,
// and s = 4 (fully inside: halftone gaps, the riso sliver) means A == B: exactly on
// the main target; within 2 levels when the translucent shadow isolates the object
// (8-bit layer resolve, then composite). A shadow sample leaking into a gap is
// at least 0.5 x 193 / 4 = 24 levels in red, far outside either tolerance.
typedef struct cut_stats {
    long full, edge, shadowed, bad;
} cut_stats;

static void render_silhouette_mask(an_scene *s, int64_t frame) {
    void *pool = ani_pool_push();
    CHECK(ani_renderer_prepare(&g_r, s, frame, W, H));
    ani_mesh *m = &g_r.mesh;
    CHECK(m->nlayers == 0); // nothing drawn or bound yet, so the buffer can be written again
    int sil = 0;
    for (int c = 0; c < m->ncmds; c++) {
        ani_cmd *cmd = &m->cmds[c];
        if (cmd->kind != ANI_CMD_TRIS) continue;
        if (cmd->paint != ANI_PAINT_SILHOUETTE) {
            cmd->count = 0;
            continue;
        }
        sil++;
        cmd->paint = ANI_PAINT_DIRECT;
        for (int i = cmd->first; i < cmd->first + cmd->count; i++) {
            m->v[i].r = m->v[i].g = m->v[i].b = m->v[i].a = 1.0f;
            m->v[i].pat = 0.0f;
        }
    }
    CHECK(sil > 0);
    sg_write_buffer_transient(&(sg_write_buffer_desc){
        .src.data = { .ptr = m->v, .size = (size_t)m->count * sizeof(ani_vertex) },
        .dst.buffer = g_r.vbuf,
    });
    ani_offscreen_begin(&g_off, AN_BLACK);
    ani_renderer_draw(&g_r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&g_off, g_px);
    ani_pool_pop(pool);
}

static an_scene *cut_scene(bool shadow, float shadow_alpha) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 77,
        .style = { .grain_off = true, .vignette_off = true, .shadow_offset = { 2, 2 },
                   .shadow_color = { 50 / 255.0f, 85 / 255.0f, 164 / 255.0f, shadow_alpha } } });
    const an_toggle sh = shadow ? AN_ON : AN_OFF;
    an_rect(s, &(an_rect_opts){ .pos = { 105, 90 }, .size = { 120, 80 }, .rotation = 0.2f, .fill = AN_HEX(0x0078BF),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 5, .shadow = sh,
                                .brush = { .taper_in = 20, .taper_out = 30, .overshoot = 0.6f },
                                .pattern = { .kind = AN_PATTERN_HALFTONE, .size = 14, .density = 0.45f, .angle = 0.3f } });
    an_circle(s, &(an_circle_opts){ .pos = { 245, 105 }, .radius = 42, .fill = AN_HEX(0xFF2E88), .stroke = AN_HEX(0x16161A),
                                    .stroke_width = 4, .shadow = sh,
                                    .pattern = { .kind = AN_PATTERN_HATCH, .size = 9, .density = 0.4f, .angle = -0.5f } });
    return s;
}

// shadow over paper, minus paper, per channel: the full per-pixel difference k = 4
static void check_cut(const unsigned char *a, const unsigned char *b, const unsigned char *mask, const double sp[3],
                      int tol_full, cut_stats *st) {
    for (int i = 0; i < W * H; i++) {
        const unsigned char *pa = a + i * 4, *pb = b + i * 4;
        const int sm = (int)lround(mask[i * 4] / 63.75);
        bool ok = fabs(mask[i * 4] - 63.75 * sm) <= 1.5;
        const double d[3] = { pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2] };
        if (sm == 4) {
            st->full++;
            for (int c = 0; c < 3; c++) ok = ok && fabs(d[c]) <= tol_full;
        } else {
            if (sm > 0) st->edge++;
            const long k = lround(4.0 * d[0] / sp[0]);
            ok = ok && k >= 0 && k <= 4 - sm;
            for (int c = 0; c < 3; c++) ok = ok && fabs(d[c] - (double)k * sp[c] / 4.0) <= 2.0;
            if (k > 0) st->shadowed++;
        }
        if (!ok) {
            if (st->bad < 5)
                fprintf(stderr, "  (%d,%d) s=%d: A %d %d %d, B %d %d %d\n", i % W, i / W, sm, pa[0], pa[1], pa[2], pb[0],
                        pb[1], pb[2]);
            st->bad++;
        }
    }
}

static void test_shadow_cut_samples(void) {
    const size_t bytes = ani_offscreen_frame_bytes(&g_off);
    unsigned char *a = malloc(bytes), *b = malloc(bytes), *mask = malloc(bytes);
    const int64_t frames[3] = { 0, 7, 19 }; // boil steps 0, 2, 7 at 30 fps
    const float alphas[2] = { 1.0f, 0.5f }; // opaque: main target; translucent: isolated layer
    for (int ai = 0; ai < 2; ai++) {
        cut_stats st = { 0 };
        const float al = alphas[ai];
        const double sp[3] = { al * (50 - 243), al * (85 - 237), al * (164 - 226) };
        an_scene *sa = cut_scene(true, al), *sb = cut_scene(false, al), *sm = cut_scene(true, 1.0f);
        for (int f = 0; f < 3; f++) {
            render(sa, frames[f]);
            if (f == 0) CHECK(g_r.mesh.nlayers == (ai == 1 ? 2 : 0));
            memcpy(a, g_px, bytes);
            render(sb, frames[f]);
            memcpy(b, g_px, bytes);
            render_silhouette_mask(sm, frames[f]);
            memcpy(mask, g_px, bytes);
            check_cut(a, b, mask, sp, ai == 1 ? 2 : 0, &st); // layer: 8-bit resolve + composite, <= 2 (forced vs direct)
        }
        printf("  shadow cut per sample (shadow alpha %.1f, boil on, 3 frames): %ld pixels fully inside, %ld edge pixels, "
               "%ld with shadow; %ld wrong\n", al, st.full, st.edge, st.shadowed, st.bad);
        CHECK(st.bad == 0);
        CHECK(st.full > 10000 && st.edge > 1000 && st.shadowed > 2000);
        an_scene_destroy(sa);
        an_scene_destroy(sb);
        an_scene_destroy(sm);
    }
    free(a);
    free(b);
    free(mask);
}

// ---- paper grain --------------------------------------------------------------------
// Expected per scene pixel (x, y), straight from the documented formula:
//   paper + (an_hash_float(seed, x | y << 16, step) - 0.5) * grain,  step = (N * 12) / fps
static void test_grain(void) {
    const uint64_t seed = 0x123456789ABCDEFull;
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = seed,
                                                    .style = { .vignette_off = true } });
    render(s, 0);
    int bad = 0;
    const double paper[3] = { 243, 237, 226 };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const double g = (an_hash_float(seed, (uint64_t)x | (uint64_t)y << 16, 0) - 0.5) * 0.06 * 255.0;
            const unsigned char *p = at(x, y);
            for (int c = 0; c < 3; c++) {
                double want = paper[c] + g;
                want = want < 0 ? 0 : (want > 255 ? 255 : want);
                if (fabs(p[c] - want) > 1.0) {
                    if (bad < 3) fprintf(stderr, "  grain (%d,%d) ch %d = %d, want %.2f\n", x, y, c, p[c], want);
                    bad++;
                }
            }
        }
    }
    printf("  grain: %d of %d channel values off by more than 1\n", bad, W * H * 3);
    CHECK(bad == 0);

    // 12 Hz at 30 fps: steps (N*12)/30 = 0,0,0,1,1,2,... change at frames 3, 5, 8, 10
    unsigned char *f[6];
    for (int k = 0; k < 6; k++) {
        render(s, k);
        f[k] = malloc((size_t)W * H * 4);
        memcpy(f[k], g_px, (size_t)W * H * 4);
    }
    const size_t n = (size_t)W * H * 4;
    CHECK(memcmp(f[0], f[2], n) == 0); // step 0
    CHECK(memcmp(f[2], f[3], n) != 0); // 0 -> 1 at frame 3
    CHECK(memcmp(f[3], f[4], n) == 0); // step 1
    CHECK(memcmp(f[4], f[5], n) != 0); // 1 -> 2 at frame 5
    render(s, 3);
    CHECK(memcmp(g_px, f[3], n) == 0); // deterministic: frame 3 again
    an_scene_destroy(s);

    // every frame: frames 3 and 4 differ
    an_scene *e = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = seed,
                                                    .style = { .vignette_off = true, .grain_every_frame = true } });
    render(e, 3);
    memcpy(f[0], g_px, n);
    render(e, 4);
    CHECK(memcmp(f[0], g_px, n) != 0);
    an_scene_destroy(e);

    // chaos 0: exactly paper, every pixel
    an_scene *z = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .seed = seed });
    an_set_chaos(z, 0);
    render(z, 0);
    int not_paper = 0;
    for (int i = 0; i < W * H; i++) not_paper += !(g_px[i * 4] == 243 && g_px[i * 4 + 1] == 237 && g_px[i * 4 + 2] == 226);
    CHECK(not_paper == 0);
    an_scene_destroy(z);
    for (int k = 0; k < 6; k++) free(f[k]);
}

// ---- vignette -----------------------------------------------------------------------
// factor = 1 - 0.25 * smoothstep(0.4, 1, r), r = |p - c| / |half| / sqrt(2), at pixel centers.
//   center pixel (160, 100): r = |(0.5/160, 0.5/100)| / 1.4142 = 0.0039 -> factor 1
//   corner pixel (0, 0): d = (-0.996875, -0.995), |d| = 1.408469, r = 0.995938,
//     t = (r - 0.4) / 0.6 = 0.993230, smoothstep = t^2 (3 - 2t) = 0.999864,
//     factor = 0.750034 -> paper * 0.750034 = (182.26, 177.76, 169.51)
static void test_vignette(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .style = { .grain_off = true } });
    render(s, 0);
    CHECK(near(160, 100, 243, 237, 226, 1));
    CHECK(near(0, 0, 182.26, 177.76, 169.51, 1));
    an_scene_destroy(s);
}

// ---- palettes -----------------------------------------------------------------------
static void test_palettes(void) {
    const an_palette *r = an_palette_named("riot"), *h = an_palette_named("hazard"), *a = an_palette_named("acid");
    CHECK(r == &AN_PALETTE_RIOT && h == &AN_PALETTE_HAZARD && a == &AN_PALETTE_ACID);
    CHECK(an_palette_named("nope") == NULL && an_palette_named(NULL) == NULL);
    // hex values from docs/layers.md, by hand
    CHECK(r->a.r == 0.0f && r->a.g == 194 / 255.0f && r->a.b == 1.0f);        // #00C2FF
    CHECK(r->b.r == 1.0f && r->b.g == 46 / 255.0f && r->b.b == 136 / 255.0f);  // #FF2E88
    CHECK(r->c.r == 198 / 255.0f && r->c.g == 1.0f && r->c.b == 0.0f);        // #C6FF00
    CHECK(h->a.r == 1.0f && h->a.g == 106 / 255.0f && h->a.b == 19 / 255.0f);  // #FF6A13
    CHECK(a->paper.r == 243 / 255.0f && a->ink.b == 26 / 255.0f);
    // the scene takes paper and the shadow color from the palette
    an_scene *s = an_scene_create(&(an_scene_desc){ .style = { .palette = &AN_PALETTE_HAZARD } });
    CHECK(s->background.r == 243 / 255.0f && s->style.shadow_color.r == 50 / 255.0f && s->style.shadow_color.b == 164 / 255.0f);
    an_scene_destroy(s);
}

// The shadow's ink is the ink's own triangles, built once, drawn again
// translated: same vertex count, each vertex = ink vertex + offset, shadow color.
// Offset for ink width 6 with the default (8, 8): (14, 14).
static void test_shadow_built_once(void) {
    an_scene *s = scene(false);
    an_stroke(s, &(an_stroke_opts){ .segs = (an_seg[]){ AN_MOVE(40, 40), AN_CUBIC(140, 0, 180, 160, 280, 120) },
                                    .seg_count = 2, .stroke = AN_HEX(0x16161A), .stroke_width = 6,
                                    .brush = { .taper_in = 20, .taper_out = 30, .overshoot = 0.5f }, .shadow = AN_ON });
    ani_mesh m;
    ani_mesh_init(&m);
    CHECK(ani_tess_scene(&m, s, 0));
    // commands after the background: [shadow ink copy] then [ink]
    const ani_cmd *sh = NULL, *ink = NULL;
    for (int i = 1; i < m.ncmds; i++) {
        const ani_cmd *c = &m.cmds[i];
        const ani_vertex *v = &m.v[c->first];
        if (v->r == 22 / 255.0f) ink = c;
        else if (v->r == 50 / 255.0f) sh = c;
    }
    CHECK(sh && ink && sh < ink);
    if (sh && ink) {
        CHECK(sh->count == ink->count && ink->count > 100);
        int bad = 0;
        for (int i = 0; i < ink->count; i++) {
            const ani_vertex a = m.v[ink->first + i], b = m.v[sh->first + i];
            bad += !(b.x == a.x + 14.0f && b.y == a.y + 14.0f && b.r == 50 / 255.0f && b.a == 1.0f);
        }
        CHECK(bad == 0);
    }
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

// The per-object chaos multiplier passes down the tree and is animatable.
// Fill-only rect forced to misregister (riso AN_ON), center (240, 100), 40 x 40,
// so unshifted x 220..260. Group chaos 0.5, child chaos 0.5: effective 0.25,
// offset (+1, -0.75): fill x 221..261 -> column 260 blue, 261 paper; 220 paper.
// Then the child animates 0.5 -> 1.0 linearly over 1 s at 60 fps: at frame 60
// effective = 0.5 x 1.0 = 0.5, offset (+2, -1.5): column 261 blue, 262 paper.
static void test_chaos_tree(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60,
        .style = { .grain_off = true, .vignette_off = true, .boil_off = true } });
    an_obj *g = an_group(s, NULL);
    an_obj *r = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 240, 100 }, .size = { 40, 40 },
                                            .fill = AN_HEX(0x0078BF), .riso = AN_ON });
    an_set_chaos(g, 0.5f);
    an_set_chaos(r, 0.5f);
    an_play(s, an_chaos_to(r, 1.0f, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    render(s, 0);
    CHECK(BLUE(260, 100) && PAPER(261, 100) && PAPER(220, 100) && BLUE(221, 100));
    render(s, 60);
    CHECK(BLUE(261, 100) && PAPER(262, 100) && PAPER(221, 100) && BLUE(222, 100));
    an_seek(s, 30);
    CHECK_NEAR(an_obj_chaos(r), 0.5 * 0.75, 1e-6);
    an_scene_destroy(s);
}

// Grain modes. At 30 fps, frame 30 is grain step (30 x 12) / 30 = 12 when
// animated; static grain uses step 0 on every frame. So static frame 30 must
// equal animated frame 0 byte for byte, and animated frame 30 must differ.
// AN_GRAIN_AUTO follows the renderer: static when set up by a share export.
static void test_grain_modes(void) {
    const size_t bytes = (size_t)W * H * 4;
    unsigned char *anim0 = malloc(bytes);
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 5,
                                                    .style = { .grain_mode = AN_GRAIN_ANIMATED } });
    an_wait(s, 1.5f);
    render(s, 0);
    memcpy(anim0, g_px, bytes);
    render(s, 30);
    CHECK(memcmp(anim0, g_px, bytes) != 0);
    an_scene_destroy(s);
    for (int auto_mode = 0; auto_mode < 2; auto_mode++) {
        s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 5,
                                              .style = { .grain_mode = auto_mode ? AN_GRAIN_AUTO : AN_GRAIN_STATIC } });
        an_wait(s, 1.5f);
        g_r.grain_auto_static = auto_mode == 1;
        render(s, 30);
        CHECK(memcmp(anim0, g_px, bytes) == 0);
        render(s, 45);
        CHECK(memcmp(anim0, g_px, bytes) == 0);
        g_r.grain_auto_static = false;
        render(s, 30); // AUTO without a share export: animated again
        if (auto_mode) CHECK(memcmp(anim0, g_px, bytes) != 0);
        an_scene_destroy(s);
    }
    free(anim0);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_style: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    test_riso();
    test_riso_rotated();
    test_riso_rules();
    test_shadow();
    test_shadow_union();
    test_shadow_built_once();
    test_shadow_cut_riso();
    test_shadow_cut_halftone();
    test_shadow_cut_samples();
    test_paper_under();
    test_chaos_tree();
    test_grain();
    test_grain_modes();
    test_vignette();
    test_palettes();

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_style");
}
