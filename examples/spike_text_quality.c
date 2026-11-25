// spike_text_quality: small-text quality at code sizes (docs/text.md, problem 2).
// Renders a line of JetBrains Mono at 30 px through the real pipeline (4x MSAA,
// chaos 0, so snapped) and compares its coverage with stb_truetype's own
// 256-level rasterizer (stbtt_MakeGlyphBitmapSubpixel) at the same pen
// positions. Reports mean / max coverage error, the share of pixels off by more
// than a quarter, and stem widths; writes a zoomed comparison PNG.
//
//   ./build/examples/spike_text_quality [size] [out.png]
#include "anown.h"
#include "font_internal.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"

#include "stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 760, H = 80 };

int main(int argc, char **argv) {
    const float size = argc > 1 ? (float)atof(argv[1]) : 30.0f;
    const char *out = argc > 2 ? argv[2] : "out/text_quality.png";
    static const char *line = "int pivot = a[hi]; ll HH il1| mm";
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .style = { .grain_off = true, .vignette_off = true } });
    an_set_chaos(s, 0.0f);
    an_font *f = an_font_mono(s);
    const float ox = 20.0f, base = 52.0f;
    an_obj *t = an_text(s, &(an_text_opts){ .text = line, .font = f, .size = size, .pos = { ox, base } });

    void *pool = ani_pool_push();
    ani_offscreen off = { 0 };
    ani_renderer r = { 0 };
    if (!ani_gfx_setup_headless() || !ani_offscreen_make_ds(&off, W, H, 4) ||
        !ani_renderer_init(&r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4)) return 1;
    static unsigned char px[W * H * 4];
    ani_renderer_prepare(&r, s, 0, W, H);
    ani_offscreen_begin(&off, AN_CREAM);
    ani_renderer_draw(&r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&off, px);
    ani_pool_pop(pool);

    // ours: coverage from green (paper 237, black 0)
    static float ours[W * H], ref[W * H];
    for (int i = 0; i < W * H; i++) ours[i] = 1.0f - px[i * 4 + 1] / 237.0f;
    // stb at the same (snapped) pen positions: the letter's world origin is
    // text origin + pen (snapped to whole pixels from the origin; the origin is whole)
    const float k = size / (float)f->upem;
    for (int i = 0; i < an_text_length(t); i++) {
        const an_obj *g = an_text_glyph(t, i);
        const float pxf = ox + roundf(g->u.glyph.pen.x), pyf = base + roundf(g->u.glyph.pen.y);
        const float fx = pxf - floorf(pxf), fy = pyf - floorf(pyf);
        int bw, bh, bx, by;
        unsigned char *bm = stbtt_GetGlyphBitmapSubpixel(&f->info, k, k, fx, fy, g->u.glyph.glyph->index, &bw, &bh, &bx, &by);
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                const int X = (int)floorf(pxf) + bx + x, Y = (int)floorf(pyf) + by + y;
                if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                float *d = &ref[Y * W + X];
                *d = fminf(1.0f, *d + bm[y * bw + x] / 255.0f);
            }
        }
        stbtt_FreeBitmap(bm, NULL);
    }
    double sum = 0, mx = 0;
    int n = 0, over = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float a = ours[y * W + x], b = ref[y * W + x];
            if (a < 0.002f && b < 0.002f) continue; // empty in both
            const double d = fabs((double)a - (double)b);
            sum += d;
            mx = d > mx ? d : mx;
            over += d > 0.25;
            n++;
        }
    }
    printf("JetBrains Mono %.0f px, 4x MSAA (snapped) vs stb_truetype 256 levels, %d pixels with ink in either:\n", size, n);
    printf("  coverage error: mean %.4f, max %.3f; %d pixels (%.1f%%) off by more than 0.25\n", sum / n, mx, over,
           100.0 * over / n);
    // stem width: sum of coverage across each "l" and "H" stem, rows in the middle of
    // the x-height (where the stems are plain verticals)
    const float xh = (float)f->x_height * k;
    const int y0 = (int)(base - xh * 0.8f), y1 = (int)(base - xh * 0.2f);
    printf("  stem widths (px, coverage summed across the stem, rows %d..%d; the font's stem is %.2f px):\n", y0, y1,
           (float)f->stem * k);
    for (int i = 0; i < an_text_length(t); i++) {
        const an_obj *g = an_text_glyph(t, i);
        const ani_glyph *gl = g->u.glyph.glyph;
        const char ch = gl->index == ani_font_index(f, 'l') ? 'l' : gl->index == ani_font_index(f, 'H') ? 'H' : 0;
        if (!ch) continue;
        // the first stem: from the glyph's left bearing over the stem width + 2 px
        const int sx0 = (int)floorf(ox + roundf(g->u.glyph.pen.x) + (float)gl->x0 * k) - 1;
        const int sx1 = ch == 'H' ? sx0 + (int)ceilf((float)f->stem * k) + 3 : (int)ceilf(ox + roundf(g->u.glyph.pen.x) + (float)gl->x1 * k) + 1;
        double wo = 0, wr = 0, wo_min = 1e9, wo_max = 0;
        for (int y = y0; y <= y1; y++) {
            double ro = 0, rr = 0;
            for (int x = sx0; x < sx1; x++) {
                ro += ours[y * W + x];
                rr += ref[y * W + x];
            }
            wo += ro;
            wr += rr;
            wo_min = ro < wo_min ? ro : wo_min;
            wo_max = ro > wo_max ? ro : wo_max;
        }
        const int rows = y1 - y0 + 1;
        printf("    '%c' at pen %.0f: ours %.3f (rows %.2f..%.2f), stb %.3f%s\n", ch, g->u.glyph.pen.x, wo / rows, wo_min,
               wo_max, wr / rows, ch == 'l' ? "  (the 'l' includes its foot serif's row span; compare ours to stb)" : "");
    }
    // zoomed comparison: ours, stb, |difference| (x4 contrast), each scaled 4x
    const int Z = 4, cw = W, ch = 46, top = (int)base - 36;
    unsigned char *img = calloc((size_t)cw * Z * ch * Z * 3 * 3, 1);
    for (int panel = 0; panel < 3; panel++) {
        for (int y = 0; y < ch * Z; y++) {
            for (int x = 0; x < cw * Z; x++) {
                const int sx = x / Z, sy = top + y / Z;
                float v = 0;
                if (sy >= 0 && sy < H) {
                    const float a = ours[sy * W + sx], b = ref[sy * W + sx];
                    v = panel == 0 ? a : panel == 1 ? b : fminf(1.0f, fabsf(a - b) * 4.0f);
                }
                unsigned char *o = img + (((size_t)(panel * ch * Z + y) * cw * Z) + (size_t)x) * 3;
                const unsigned char g8 = (unsigned char)lroundf(255.0f * (1.0f - v));
                o[0] = panel == 2 ? 255 : g8;
                o[1] = g8;
                o[2] = g8;
            }
        }
    }
    stbi_write_png(out, cw * Z, ch * Z * 3, 3, img, cw * Z * 3);
    printf("  wrote %s (top: ours, middle: stb, bottom: |difference| x 4)\n", out);
    free(img);
    ani_renderer_shutdown(&r);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    an_scene_destroy(s);
    return 0;
}
